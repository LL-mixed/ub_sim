import concurrent.futures
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


SCRIPT = Path(__file__).resolve().parents[1] / "scripts/codex_network_audit.py"
SPEC = importlib.util.spec_from_file_location("codex_network_audit", SCRIPT)
AUDIT = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(AUDIT)


class NetworkAuditTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.log = Path(self.temp.name) / "operations.jsonl"
        self.fields = [
            "--kind", "fixture", "--target", "local-unit-test",
            "--purpose", "verify recorder", "--operation", "fixture command",
            "--data-sent", "none", "--authorization", "unit test",
            "--network-scope", "no network", "--risk", "low",
            "--risk-reason", "local fixture only",
        ]

    def cli(self, *arguments, log=None):
        return subprocess.run(
            [sys.executable, str(SCRIPT), "--log", str(log or self.log), *arguments],
            capture_output=True, text=True, check=False,
        )

    def events(self):
        return [json.loads(line) for line in self.log.read_text().splitlines()]

    def test_begin_is_durable_before_command_and_output_is_not_copied(self):
        code = (
            "import json,sys; "
            "assert json.loads(open(sys.argv[1]).readline())['phase']=='begin'; "
            "print('private-output-' + 'not-for-audit')"
        )
        result = self.cli("run", *self.fields, "--", sys.executable, "-c", code,
                          str(self.log))
        self.assertEqual(result.returncode, 0, result.stderr)
        events = self.events()
        self.assertEqual([item["phase"] for item in events], ["begin", "end"])
        self.assertEqual(events[0]["id"], events[1]["id"])
        self.assertEqual(events[1]["status"], "completed")
        self.assertNotIn("private-output-not-for-audit", self.log.read_text())
        self.assertEqual(os.stat(self.log).st_mode & 0o777, 0o600)

    def test_failed_command_keeps_exit_code(self):
        result = self.cli("run", *self.fields, "--", sys.executable,
                          "-c", "raise SystemExit(7)")
        self.assertEqual(result.returncode, 7)
        self.assertEqual(self.events()[-1]["exit_code"], 7)
        self.assertEqual(self.events()[-1]["status"], "failed")

    def test_failed_spawn_is_recorded(self):
        result = self.cli("run", *self.fields, "--", str(self.log.parent / "absent"))
        self.assertEqual(result.returncode, 127)
        self.assertEqual(self.events()[-1]["details"], "FileNotFoundError")

    def test_cannot_write_begin_prevents_command(self):
        marker = self.log.parent / "must-not-exist"
        result = self.cli("run", *self.fields, "--", sys.executable, "-c",
                          "import pathlib,sys; pathlib.Path(sys.argv[1]).touch()",
                          str(marker), log=self.log.parent / "absent" / "log")
        self.assertEqual(result.returncode, 78)
        self.assertFalse(marker.exists())

    def test_manual_unknown_and_invalid_finish(self):
        result = self.cli("begin", *self.fields)
        self.assertEqual(result.returncode, 0, result.stderr)
        operation_id = result.stdout.strip()
        status = json.loads(self.cli("status").stdout)
        self.assertEqual(status["unfinished"][0]["id"], operation_id)
        result = self.cli("finish", "--id", "missing", "--status", "completed",
                          "--details", "must reject")
        self.assertEqual(result.returncode, 78)
        result = self.cli("finish", "--id", operation_id, "--status", "unknown",
                          "--details", "observation interrupted")
        self.assertEqual(result.returncode, 0)
        result = self.cli("finish", "--id", operation_id, "--status", "completed",
                          "--details", "duplicate must reject")
        self.assertEqual(result.returncode, 78)
        status = json.loads(self.cli("status").stdout)
        self.assertEqual(status["unfinished"], [])
        self.assertEqual(len(status["unknown"]), 1)
        self.assertEqual(status["events"], 2)

    def test_redaction(self):
        fixtures = [
            ("https://alice:private-password@example.test/a", "private-password"),
            ("https://example.test/a?token=private-token&x=1", "private-token"),
            ("curl --api-key 'private key' --password private-pass", "private key"),
            ("curl --api-key 'private key' --password private-pass", "private-pass"),
            ("Authorization: Bearer private-bearer", "private-bearer"),
            ("Cookie: session=private-cookie", "private-cookie"),
        ]
        for value, secret in fixtures:
            with self.subTest(value=value):
                result = AUDIT.redact(value)
                self.assertNotIn(secret, result)
                self.assertIn("[REDACTED]", result)

    def test_argv_is_not_executed_through_shell(self):
        argument = "literal; $(touch must-not-run)"
        result = self.cli("run", *self.fields, "--", sys.executable, "-c",
                          "import sys; print(sys.argv[1])", argument)
        self.assertEqual(result.returncode, 0)
        self.assertEqual(result.stdout.strip(), argument)

    def test_corrupt_journal_prevents_new_begin(self):
        self.log.write_text('{"incomplete":')
        result = self.cli("begin", *self.fields)
        self.assertEqual(result.returncode, 78)
        self.assertEqual(self.log.read_text(), '{"incomplete":')

    def test_symlink_destination_is_rejected(self):
        original = self.log.parent / "original"
        original.write_text("do not change")
        self.log.symlink_to(original)
        result = self.cli("begin", *self.fields)
        self.assertEqual(result.returncode, 78)
        self.assertEqual(original.read_text(), "do not change")

    def test_concurrent_appends_preserve_all_records(self):
        def append(index):
            return AUDIT.record_event(self.log, {
                "id": str(index), "phase": "begin", "operation": "fixture",
            })
        with concurrent.futures.ThreadPoolExecutor(max_workers=4) as executor:
            list(executor.map(append, range(20)))
        self.assertEqual(len(self.events()), 20)
        self.assertEqual({item["id"] for item in self.events()},
                         {str(index) for index in range(20)})


if __name__ == "__main__":
    unittest.main()
