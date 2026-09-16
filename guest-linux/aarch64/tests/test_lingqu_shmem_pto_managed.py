import pathlib
import json
import platform
import shutil
import shlex
import subprocess
import sys
import tempfile
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[3]
APP = ROOT / "guest-linux/aarch64/apps/lingqu_shmem_pto_direct"
NATIVE = platform.system() == "Linux" and platform.machine() == "aarch64"
CONFIG = {
    "connect": "unix:/tmp/managed-pto-validation-must-not-connect.sock",
    "key": "managed-pto-test", "node_id": "1", "generation": "135",
    "requester_cna": "0xf002", "artifact_fingerprint": "0x1234",
    "timeout_ms": "1000",
}


@unittest.skipUnless(NATIVE and shutil.which("cc"), "native Linux AArch64 CLI fixture")
class ManagedPtoCliTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.directory.cleanup)
        cls.binary = pathlib.Path(cls.directory.name) / "managed-pto"
        result = subprocess.run([
            "make", "-C", str(APP), "CC=cc", "MANAGED=1", f"TARGET={cls.binary}",
        ], capture_output=True, text=True, timeout=120)
        if result.returncode:
            raise AssertionError(result.stdout + result.stderr)

    def run_config(self, values=None, extra="", raw=None):
        config = pathlib.Path(self.directory.name) / "input.conf"
        if raw is None:
            raw = "".join(f"{key}={value}\n" for key, value in
                          (CONFIG if values is None else values).items()) + extra
        config.write_text(raw)
        return subprocess.run([str(self.binary), "--managed-config", str(config),
                               "--validate-only"], capture_output=True, text=True, timeout=5)

    def test_validate_only_accepts_both_roles_without_device_or_network(self):
        for node in ("0", "1"):
            with self.subTest(node=node):
                result = self.run_config({**CONFIG, "node_id": node})
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertIn(f"status=ok node={node} nodes=2", result.stdout)
                self.assertIn("network_operations=0", result.stdout)
                self.assertNotIn("peer_canary", result.stdout)
        result = self.run_config({**CONFIG, "connect": "tcp:127.0.0.1:9700"})
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_rejects_missing_duplicate_unknown_and_malformed_fields(self):
        for key in CONFIG:
            values = dict(CONFIG)
            values.pop(key)
            with self.subTest(missing=key):
                self.assertEqual(self.run_config(values).returncode, 2)
            with self.subTest(duplicate=key):
                self.assertEqual(self.run_config(extra=f"{key}={CONFIG[key]}\n").returncode, 2)
        bad = {"node_id": ("2", "-1"), "generation": ("0", "18446744073709551615"),
               "requester_cna": ("0", "4294967296"), "artifact_fingerprint": ("0", "bad"),
               "timeout_ms": ("0", "600001", " 50"), "key": ("", "x" * 100, "a b"),
               "connect": ("tcp:", "tcp:127.0.0.1:0", "tcp:127.0.0.1:65536",
                           "tcp:bad:80", "unix:", "unix:relative", "http://example.test")}
        for key, values in bad.items():
            for value in values:
                with self.subTest(key=key, value=value):
                    self.assertEqual(self.run_config({**CONFIG, key: value}).returncode, 2)
        for extra in ("unknown=1\n", "missing_equals\n", "key=hidden\0tail\n", "\rhidden\n"):
            with self.subTest(extra=extra):
                self.assertEqual(self.run_config(extra=extra).returncode, 2)

    def test_managed_binary_has_no_model_arena_dependency(self):
        symbols = subprocess.check_output(["nm", str(self.binary)], text=True)
        self.assertIn("lingqu_shmem_mem_service_open_managed", symbols)
        self.assertIn("mem_service_provider_obmm_mapping_pin_acquire", symbols)
        self.assertNotIn("mem_service_cluster_runtime_current", symbols)
        self.assertNotIn("mem_service_model_kv_state_alloc", symbols)

    def test_optional_submit_timeout_requires_bounded_recovery(self):
        result = self.run_config({**CONFIG, "submit_timeout_ms": "5"})
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("submit_timeout_ms=5 recovery_expected=1", result.stdout)
        for value in ("0", "1000", "1001", "600001", "-1", "5x"):
            with self.subTest(value=value):
                self.assertEqual(self.run_config({**CONFIG, "submit_timeout_ms": value}).returncode, 2)
        self.assertEqual(self.run_config(extra="submit_timeout_ms=5\nsubmit_timeout_ms=6\n").returncode, 2)

    def test_access_threads_is_bounded_and_validate_only_has_no_side_effects(self):
        for count in ("0", "4", "8"):
            result = self.run_config({**CONFIG, "access_threads": count})
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn(f"access_threads={count}", result.stdout)
            self.assertIn("network_operations=0", result.stdout)
        for count in ("9", "-1", "bad", "4294967296"):
            self.assertEqual(self.run_config({**CONFIG, "access_threads": count}).returncode, 2)
        self.assertEqual(self.run_config(extra="access_threads=4\naccess_threads=4\n").returncode, 2)

    def test_concurrent_requests_are_explicit_and_preserve_geometry_and_owner_modes(self):
        self.assertIn("concurrent_requests=1", self.run_config().stdout)
        for home in ("0", "1"):
            for layout in ("cross-page", "nd", "tail"):
                for readonly in ("0", "1"):
                    values = {**CONFIG, "home_node": home, "layout": layout,
                              "readonly_inputs": readonly, "concurrent_requests": "2", "access_threads": "8"}
                    result = self.run_config(values)
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                    self.assertIn("concurrent_requests=2", result.stdout)
                    self.assertIn("network_operations=0", result.stdout)
        for invalid in ("0", "3", "-1", "2x", "18446744073709551615"):
            self.assertEqual(self.run_config({**CONFIG, "concurrent_requests": invalid}).returncode, 2)
        for extra in ({"negative_checks": "1"}, {"submit_timeout_ms": "5"}):
            self.assertEqual(self.run_config({**CONFIG, "concurrent_requests": "2", **extra}).returncode, 2)
        self.assertEqual(self.run_config(extra="concurrent_requests=2\nconcurrent_requests=1\n").returncode, 2)

    def test_negative_checks_are_opt_in_and_separate_from_timeout_recovery(self):
        for enabled in ("0", "1"):
            result = self.run_config({**CONFIG, "negative_checks": enabled})
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn(f"negative_checks={enabled}", result.stdout)
            self.assertIn("network_operations=0", result.stdout)
        for value in ("2", "-1", "true", "4294967296"):
            self.assertEqual(self.run_config({**CONFIG, "negative_checks": value}).returncode, 2)
        self.assertEqual(self.run_config({**CONFIG, "negative_checks": "1",
                                         "submit_timeout_ms": "5"}).returncode, 2)
        self.assertEqual(self.run_config({**CONFIG, "negative_checks": "1",
                                         "generation": str((1 << 64) - 5)}).returncode, 2)
        self.assertEqual(self.run_config({**CONFIG, "negative_checks": "1",
                                         "generation": str((1 << 64) - 9)}).returncode, 0)
        self.assertEqual(self.run_config({**CONFIG, "negative_checks": "1",
                                         "generation": str((1 << 64) - 8)}).returncode, 2)
        self.assertEqual(self.run_config(extra="negative_checks=1\nnegative_checks=1\n").returncode, 2)

    def test_holder_recovery_probe_is_bounded_and_side_effect_free_when_validated(self):
        for node in ("0", "1"):
            result = self.run_config({**CONFIG, "node_id": node,
                                      "holder_recovery_probe": "1"})
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("holder_recovery_probe=1", result.stdout)
            self.assertIn("network_operations=0", result.stdout)
        for invalid in ("2", "-1", "true", "18446744073709551616"):
            result = self.run_config({**CONFIG, "holder_recovery_probe": invalid})
            self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
        for conflict in ({"negative_checks": "1"}, {"readonly_inputs": "1"},
                         {"access_threads": "1"}, {"concurrent_requests": "2"},
                         {"submit_timeout_ms": "5"}):
            result = self.run_config({**CONFIG, **conflict,
                                      "holder_recovery_probe": "1"})
            self.assertEqual(result.returncode, 2, result.stdout + result.stderr)

    def test_readonly_inputs_preserve_negative_and_recovery_options(self):
        for extra in ({}, {"negative_checks": "1"}, {"submit_timeout_ms": "5"},
                      {"access_threads": "4"}):
            for node in ("0", "1"):
                result = self.run_config({**CONFIG, **extra, "node_id": node,
                                          "readonly_inputs": "1"})
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertIn("readonly_inputs=1", result.stdout)
                self.assertIn("network_operations=0", result.stdout)
        for value in ("2", "-1", "true", "18446744073709551616"):
            self.assertEqual(self.run_config({**CONFIG, "readonly_inputs": value}).returncode, 2)
        self.assertEqual(self.run_config(extra="readonly_inputs=1\nreadonly_inputs=0\n").returncode, 2)
        for length, expected in ((88, 0), (89, 2)):
            self.assertEqual(self.run_config({**CONFIG, "readonly_inputs": "1",
                                             "key": "k" * length}).returncode, expected)

    def test_home_selection_preserves_actual_node_and_rejects_invalid_values(self):
        self.assertIn("home_node=0", self.run_config().stdout)
        for home in (0, 1):
            for node in (0, 1):
                result = self.run_config({**CONFIG, "node_id": str(node),
                    "home_node": str(home), "readonly_inputs": "1"})
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertIn(f"status=ok node={node} nodes=2", result.stdout)
                self.assertIn(f"home_node={home}", result.stdout)
                self.assertIn("network_operations=0", result.stdout)
        for invalid in ("2", "-1", "1x", "18446744073709551615", ""):
            self.assertEqual(self.run_config({**CONFIG, "home_node": invalid}).returncode, 2)
        self.assertEqual(self.run_config(extra="home_node=0\nhome_node=1\n").returncode, 2)

    def test_reference_inputs_require_readonly_and_accept_every_geometry_and_home(self):
        self.assertIn("reference_inputs=0", self.run_config().stdout)
        for home in ("0", "1"):
            for node in ("0", "1"):
                for layout in ("cross-page", "nd", "tail"):
                    result = self.run_config({**CONFIG, "home_node": home, "node_id": node,
                        "layout": layout, "readonly_inputs": "1", "reference_inputs": "1"})
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                    self.assertIn("reference_inputs=1", result.stdout)
                    self.assertIn("network_operations=0", result.stdout)
        self.assertEqual(self.run_config({**CONFIG, "reference_inputs": "1"}).returncode, 2)
        for value in ("2", "-1", "true", "18446744073709551616"):
            self.assertEqual(self.run_config({**CONFIG, "readonly_inputs": "1",
                                             "reference_inputs": value}).returncode, 2)
        self.assertEqual(self.run_config({**CONFIG, "readonly_inputs": "1"},
            extra="reference_inputs=1\nreference_inputs=1\n").returncode, 2)

    def test_model_publication_requires_the_managed_model_probe(self):
        values = {**CONFIG, "home_node": "1", "readonly_inputs": "1",
                  "reference_inputs": "1", "layout": "nd", "model_probe": "1",
                  "model_output_bytes": "54680", "model_output_checksum": "0x1234",
                  "model_publish": "1"}
        key = ("w5v1/0000000000000087/0000000000000088/00000000/"
               "0000000000000000")
        for node in ("0", "1"):
            result = self.run_config({**values, "node_id": node})
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("model_publish=1", result.stdout)
            self.assertIn(f"MANAGED_MODEL_KEY status=ok node={node} "
                          f"producer=0 step=0 key={key} network_operations=0",
                          result.stdout)
            self.assertIn("network_operations=0", result.stdout)
        for invalid in ("2", "-1", "1x", "18446744073709551616"):
            self.assertEqual(self.run_config({**values, "model_publish": invalid}).returncode, 2)
        self.assertEqual(self.run_config({**CONFIG, "model_publish": "1"}).returncode, 2)
        self.assertEqual(self.run_config(values, extra="model_publish=1\n").returncode, 2)

    def test_layout_profiles_derive_bounded_geometry_before_any_connection(self):
        profiles = (("cross-page", 64, 65536), ("nd", 16384, 262144),
                    ("tail", 16256, 262144))
        for layout, elements, size in profiles:
            for flags in ({}, {"readonly_inputs": "1", "negative_checks": "1"},
                          {"readonly_inputs": "1", "submit_timeout_ms": "5"}):
                values = {**CONFIG, **flags, "layout": layout}
                for explicit in ({}, {"elements": str(elements), "size_bytes": str(size)}):
                    result = self.run_config({**values, **explicit})
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                    self.assertIn(f"bytes={size} layout={layout} elements={elements}", result.stdout)
                    self.assertIn("network_operations=0", result.stdout)
            for field, value in (("elements", elements), ("size_bytes", size)):
                for invalid in ("0", str(value + 1), "-1", "18446744073709551615", "1x"):
                    with self.subTest(layout=layout, field=field, invalid=invalid):
                        self.assertEqual(self.run_config({**CONFIG, "layout": layout,
                                                          field: invalid}).returncode, 2)
                self.assertEqual(self.run_config({**CONFIG, "layout": layout},
                    extra=f"{field}={value}\n{field}={value}\n").returncode, 2)
        for layout in ("nd-strided", "dn", "nz", "unaligned", "unknown", "ND", "nd "):
            self.assertEqual(self.run_config({**CONFIG, "layout": layout}).returncode, 2)
        self.assertEqual(self.run_config(extra="layout=nd\nlayout=tail\n").returncode, 2)

    def test_model_probe_requires_bounded_output_oracle_and_managed_reference(self):
        values = {**CONFIG, "home_node": "1", "readonly_inputs": "1",
                  "reference_inputs": "1", "layout": "nd", "model_probe": "1",
                  "model_output_bytes": "54680", "model_output_checksum": "0x1234"}
        for node in ("0", "1"):
            result = self.run_config({**values, "node_id": node})
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("callable=4 network_operations=0", result.stdout)
            self.assertIn("model_probe=1", result.stdout)
        invalid = {"model_probe": ("0", "2"), "home_node": ("0",),
                   "readonly_inputs": ("0",), "reference_inputs": ("0",),
                   "layout": ("cross-page", "tail"), "access_threads": ("1",),
                   "concurrent_requests": ("2",), "negative_checks": ("1",),
                   "submit_timeout_ms": ("5",),
                   "model_output_bytes": ("0", "245761", "18446744073709551615")}
        for key, options in invalid.items():
            for value in options:
                with self.subTest(key=key, value=value):
                    self.assertEqual(self.run_config({**values, key: value}).returncode, 2)
        for key in ("model_output_bytes", "model_output_checksum"):
            missing = dict(values)
            missing.pop(key)
            self.assertEqual(self.run_config(missing).returncode, 2)


@unittest.skipUnless(shutil.which("cc"), "native C compiler required for isolated CPU probe")
class ManagedPtoCpuProbeTest(unittest.TestCase):
    def test_probe_uses_real_page_protection_and_restores_signal_handlers(self):
        with tempfile.TemporaryDirectory() as directory:
            binary = pathlib.Path(directory) / "cpu-probe"
            source = ROOT / "guest-linux/aarch64/tests/lingqu_shmem_pto_cpu_probe_golden.c"
            built = subprocess.run(["cc", "-std=gnu11", "-O2", "-Wall", "-Wextra", "-Werror",
                                    "-I", str(APP), str(source), "-o", str(binary)],
                                   capture_output=True, text=True, timeout=30)
            self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
            result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=5)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("pto cpu readonly probe golden: pass", result.stdout)


@unittest.skipUnless(platform.system() == "Linux" and shutil.which("cc"),
                     "Linux linker required for isolated managed owner guard fixture")
class ManagedPtoOwnerGuardTest(unittest.TestCase):
    def test_reference_subview_identity_and_uncertain_mapping_ownership(self):
        with tempfile.TemporaryDirectory() as directory:
            binary = pathlib.Path(directory) / "reference"
            source = ROOT / "guest-linux/aarch64/tests/lingqu_shmem_pto_reference_golden.c"
            built = subprocess.run([
                "cc", "-std=gnu11", "-O2", "-Wall", "-Wextra", "-Werror", "-pthread",
                "-DLINGQU_SHMEM_MANAGED_MAPPING", "-ffunction-sections", "-fdata-sections",
                "-Wl,--gc-sections", "-I", str(ROOT / "mem_service"),
                "-I", str(ROOT / "guest-linux/aarch64"),
                "-I", str(ROOT / "crates/sim-qemu/include"),
                "-I", str(ROOT / "guest-linux/aarch64/libs/lingqu_shmem_pto"),
                str(source), "-o", str(binary),
            ], capture_output=True, text=True, timeout=30)
            self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
            result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=5)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("managed PTO reference orchestration: pass", result.stdout)

    def test_expected_admission_rejection_completes_both_owner_guards(self):
        with tempfile.TemporaryDirectory() as directory:
            binary = pathlib.Path(directory) / "owner-guards"
            source = ROOT / "guest-linux/aarch64/tests/lingqu_shmem_pto_managed_owner_golden.c"
            built = subprocess.run([
                "cc", "-std=gnu11", "-O2", "-Wall", "-Wextra", "-Werror", "-pthread",
                "-DLINGQU_SHMEM_MANAGED_MAPPING", "-ffunction-sections", "-fdata-sections",
                "-Wl,--gc-sections", "-I", str(ROOT / "mem_service"),
                "-I", str(ROOT / "guest-linux/aarch64"),
                "-I", str(ROOT / "crates/sim-qemu/include"),
                "-I", str(ROOT / "guest-linux/aarch64/libs/lingqu_shmem_pto"),
                str(ROOT / "guest-linux/aarch64/libs/lingqu_shmem_pto/w5_managed_model_guest.c"),
                str(source), "-o", str(binary),
            ], capture_output=True, text=True, timeout=30)
            self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
            result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=5)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("managed PTO owner guards: pass", result.stdout)
            self.assertIn("managed PTO shared compute guards: pass", result.stdout)
            self.assertIn("managed model adopted input view: pass", result.stdout)


class ManagedPtoContractTest(unittest.TestCase):
    @unittest.skipUnless(shutil.which("cc"), "C compiler required for boot parameter boundary")
    def test_home_option_does_not_consume_kernel_init_environment_slot(self):
        runner = (ROOT / "guest-linux/aarch64/scripts/run_ub_dual_node_apps.sh").read_text()
        option = next(line.split('"')[1].split("=", 1)[0] for line in runner.splitlines()
                      if "append_cmdline_if_missing" in line and "PTO_HOME_NODE" in line)
        self.assertEqual(option, "lingqu_shmem_pto.home_node")
        guest = (ROOT / "guest-linux/aarch64/initramfs/run_app").read_text()
        helper = "cmdline_value() {" + guest.split("cmdline_value() {", 1)[1].split("\n}", 1)[0] + "\n}\n"
        for value in ("0", "1"):
            script = "read_cmdline() { :; }\n" + helper + (
                f"cmdline='other=1 {option}={value} trailing=2'\ncmdline_value {option} 0\n")
            result = subprocess.run(["sh", "-c", script], capture_output=True, text=True, timeout=5)
            self.assertEqual((result.returncode, result.stdout.strip()), (0, value))
        kernel = (ROOT / "guest-linux/kernel_ub/init/main.c").read_text()
        function = "static int __init unknown_bootoption" + kernel.split(
            "static int __init unknown_bootoption", 1)[1].split("\n}\n", 1)[0] + "\n}\n"
        # Compile the actual kernel admission function with isolated dependencies.
        fixture = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#define __init
#define MAX_INIT_ENVS 32
#define MAX_INIT_ARGS 32
static const char *envp_init[34], *argv_init[34], *panic_later, *panic_param;
static int sysctl_is_alias(const char *p) { (void)p; return 0; }
static void repair_env_string(char *p, char *v) { (void)p; (void)v; }
static int obsolete_checksetup(char *p) { (void)p; return 0; }
static int strstarts(const char *s, const char *p) { return !strncmp(s, p, strlen(p)); }
static const char *strnchr(const char *s, size_t n, int c) { return memchr(s, c, n); }
''' + function + r'''
int main(int argc, char **argv) {
    char fields[32][64];
    assert(argc >= 2);
    envp_init[0] = "HOME=/"; envp_init[1] = "TERM=linux";
    for (int i = 0; i < 30; ++i) {
        snprintf(fields[i], sizeof(fields[i]), "field%d", i);
        unknown_bootoption(fields[i], "1", NULL, NULL);
    }
    for (int i = 1; i < argc; ++i)
        unknown_bootoption(argv[i], "1", NULL, NULL);
    snprintf(fields[30], sizeof(fields[30]), "linqu_node_idx");
    unknown_bootoption(fields[30], "0", NULL, NULL);
    if (panic_later) {
        assert(!strcmp(panic_later, "env") && !strcmp(panic_param, "linqu_node_idx"));
        return 1;
    }
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as directory:
            source, binary = pathlib.Path(directory) / "boot.c", pathlib.Path(directory) / "boot"
            source.write_text(fixture)
            # The kernel callback ABI includes intentionally unused arguments.
            built = subprocess.run(["cc", "-std=gnu11", "-O2", "-Wall", "-Wextra", "-Werror",
                                    "-Wno-unused-parameter", str(source), "-o", str(binary)],
                                   capture_output=True, text=True, timeout=30)
            self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
            for key, expected in ((option, 0), ("lingqu_shmem_pto_home_node", 1),
                                  ("lingqu_shmem_pto.concurrent_requests", 0),
                                  ("lingqu_shmem_pto.reference_inputs", 0),
                                  ("lingqu_shmem_pto.model_probe", 0),
                                  ("lingqu_shmem_pto.model_output_bytes", 0),
                                  ("lingqu_shmem_pto.model_output_checksum", 0)):
                checked = subprocess.run([str(binary), key], capture_output=True, text=True, timeout=5)
                self.assertEqual(checked.returncode, expected, checked.stdout + checked.stderr)

            # Exercise the entire emitted object-share group with the real
            # kernel callback at its init environment capacity boundary.
            fields = ("backend", "access", "size", "fault", "iterations", "seed",
                      "port", "incarnation", "aperture_size", "address", "descriptor_hex")
            keys = [f"mem_service.object_share_{field}" for field in fields]
            for field, key in zip(fields, keys):
                self.assertIn(f'"{key}=${{MEM_SERVICE_OBJECT_SHARE_{field.upper()}}}"', runner)
                self.assertNotIn(f'"mem_service_object_share_{field}=', runner)
            checked = subprocess.run([str(binary), *keys], capture_output=True, text=True, timeout=5)
            self.assertEqual(checked.returncode, 0, checked.stdout + checked.stderr)
            checked = subprocess.run([str(binary), "mem_service_object_share_aperture_size"],
                                     capture_output=True, text=True, timeout=5)
            self.assertEqual(checked.returncode, 1, checked.stdout + checked.stderr)

    def test_object_share_boot_options_preserve_legacy_and_prefer_new_keys(self):
        guest = (ROOT / "guest-linux/aarch64/initramfs/run_app").read_text()
        helper = "cmdline_value() {" + guest.split("cmdline_value() {", 1)[1].split(
            "\n}", 1)[0] + "\n}\n"
        assignments = [line.strip() for line in guest.splitlines()
                       if '="$(cmdline_value mem_service.object_share_' in line]
        self.assertEqual(len(assignments), 11)
        for assignment in assignments:
            variable = assignment.split("=", 1)[0]
            key = assignment.split("cmdline_value ", 1)[1].split(" ", 1)[0]
            old_key = key.replace("mem_service.", "mem_service_")
            for cmdline, expected in ((f"{old_key}=old", "old"), (f"{key}=new", "new"),
                                      (f"{old_key}=old {key}=new", "new"),
                                      (f"{key}=new {old_key}=old", "new")):
                script = ("read_cmdline() { :; }\n" + helper + f"cmdline='{cmdline}'\n"
                          + assignment + f'\nprintf "%s" "${{{variable}}}"\n')
                with self.subTest(key=key, cmdline=cmdline):
                    checked = subprocess.run(["sh", "-c", script], capture_output=True,
                                             text=True, timeout=5)
                    self.assertEqual((checked.returncode, checked.stdout), (0, expected),
                                     checked.stderr)

    def test_guest_preflight_accepts_exact_large_profiles_before_opening_service(self):
        source = (ROOT / "guest-linux/aarch64/initramfs/run_app").read_text()
        body = source.split("run_mem_service_object_share() {", 1)[1].split(
            "  if [ ! -x /bin/linqu_mem_service ]; then", 1)[0]
        preflight = "preflight() {" + body + "\n}\npreflight\n"
        defaults = {"mem_service_object_share_backend": "obmm", "mem_service_object_share_access": "cpu-pto"}
        for layout, elements, size in (("cross-page", "64", "65536"),
                                      ("nd", "16384", "262144"), ("tail", "16256", "262144")):
            values = {**defaults, "lingqu_shmem_pto_layout": layout,
                      "lingqu_shmem_pto_elements": elements, "mem_service_object_share_size": size}
            for change, expected in (({}, 0), ({"mem_service_object_share_size": "4096"}, 2),
                    ({"lingqu_shmem_pto.home_node": "1"}, 0),
                    ({"lingqu_shmem_pto.home_node": "2"}, 2),
                    ({"lingqu_shmem_pto.home_node": "-1"}, 2),
                    ({"lingqu_shmem_pto_elements": "65"}, 2), ({"lingqu_shmem_pto_layout": "dn"}, 2),
                    ({"mem_service_object_share_backend": "loopback"}, 2),
                    ({"mem_service_object_share_iterations": "2"}, 2),
                    ({"mem_service_object_share_fault": "capacity"}, 2)):
                cases = "\n".join(f"{key}) printf '%s' '{value}';;" for key, value in
                                  {**values, **change}.items())
                script = "cmdline_value() { case \"$1\" in\n" + cases + (
                    '\n*) printf "%s" "$2";; esac; }\nlog() { printf "%s\\n" "$*"; }\n') + preflight
                with self.subTest(layout=layout, change=change):
                    result = subprocess.run(["sh", "-c", script], capture_output=True,
                                            text=True, timeout=5)
                    self.assertEqual(result.returncode, expected, result.stdout + result.stderr)

    def test_runner_requires_exact_request_scoped_fault_manifest(self):
        runner = (ROOT / "guest-linux/aarch64/scripts/run_ub_dual_node_apps.sh").read_text()
        function = runner.split("validate_pto_layout_manifest() {", 1)[1].split("\nPY", 1)[0]
        script = function.split("<<'PY'\n", 1)[1]
        manifest = {"ub_gm_layout": {"profile": "cross-page", "global_rows": 1,
            "global_cols": 64, "tile_rows": 1, "tile_cols": 64, "logical_elements": 64},
            "host_vector_program": "quadratic", "ub_gm_access_fault": "none"}
        marker = {"tstore_request": 142, "tload_request": 143}
        cases = [(manifest, "0", 0), (manifest, "1", 1),
                 ({**manifest, "managed_access_fault": marker}, "1", 0),
                 ({**manifest, "managed_access_fault": marker}, "0", 1),
                 ({**manifest, "managed_access_fault": {**marker, "tstore_request": 141}}, "1", 1),
                 ({**manifest, "managed_access_fault": {**marker, "tload_request": 142}}, "1", 1),
                 ({**manifest, "managed_access_fault": marker, "ub_gm_access_fault": "tstore-on-read"}, "1", 1),
                 ({**manifest, "managed_access_fault": marker, "host_vector_program": "pipeline_double"}, "1", 1)]
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / "manifest.json"
            for contents, enabled, expected in cases:
                path.write_text(json.dumps(contents))
                with self.subTest(contents=contents, enabled=enabled):
                    result = subprocess.run([sys.executable, "-c", script, str(path),
                        "cross-page", "64", enabled, "135"], capture_output=True, text=True, timeout=5)
                    self.assertEqual(result.returncode, expected, result.stdout + result.stderr)

    @unittest.skipUnless(shutil.which("zsh"), "zsh runner parser required")
    def test_model_runner_preflight_uses_oracle_without_vector_manifest(self):
        runner = (ROOT / "guest-linux/aarch64/scripts/run_ub_dual_node_apps.sh").read_text()
        defaults = "\n".join(line for line in runner.splitlines() if
                             line.startswith(("LINGQU_SHMEM_PTO_", "SIMPLER_HOST_VECTOR_MANIFEST="))
                             and '="${' in line and line.endswith('}"'))
        functions = "\n".join(name + "() {" + runner.split(name + "() {", 1)[1].split("\n}", 1)[0] + "\n}"
                              for name in ("require_pto_unsigned_value", "append_cmdline_if_missing",
                                           "managed_pto_selected", "validate_lingqu_shmem_pto_config"))
        script = "set -eu\n" + defaults + "\n" + functions + (
            '\nsource "$1"\nvalidate_lingqu_shmem_pto_config\nprintf "%s\\n" "$APPEND_EXTRA"\n')
        values = {"APPEND_EXTRA": "linqu_mem_service_object_share=1",
                  "MEM_SERVICE_OBJECT_SHARE_ACCESS": "cpu-pto",
                  "SIM_UAPI_W4_CHIPBACKEND_PROFILE": "qwen3_dense", "SIM_QWEN3_DENSE_TP_NODES": "2",
                  "SIM_UAPI_SCENARIO_CONFIG": str(ROOT / "scenarios/mvp_2host_single_domain.yaml"),
                  "SIMPLER_HOST_VECTOR_MANIFEST": "/nonexistent/model-probe-must-not-load-vector.json",
                  "LINGQU_SHMEM_PTO_MODEL_PROBE": "1", "LINGQU_SHMEM_PTO_MODEL_OUTPUT_BYTES": "54680",
                  "LINGQU_SHMEM_PTO_MODEL_OUTPUT_CHECKSUM": "0x1234", "LINGQU_SHMEM_PTO_HOME_NODE": "1",
                  "LINGQU_SHMEM_PTO_REFERENCE_INPUTS": "1", "LINGQU_SHMEM_PTO_READONLY_INPUTS": "1",
                  "LINGQU_SHMEM_PTO_ARTIFACT_FINGERPRINT": "0x1234"}
        changes = ({}, {"LINGQU_SHMEM_PTO_MODEL_PUBLISH": "1"},
                   {"LINGQU_SHMEM_PTO_MODEL_PUBLISH": "2"},
                   {"LINGQU_SHMEM_PTO_MODEL_PROBE": "2"},
                   {"LINGQU_SHMEM_PTO_MODEL_OUTPUT_BYTES": "0"},
                   {"LINGQU_SHMEM_PTO_MODEL_OUTPUT_BYTES": "245761"},
                   {"LINGQU_SHMEM_PTO_HOME_NODE": "0"}, {"SIM_QWEN3_DENSE_TP_NODES": "3"},
                   {"LINGQU_SHMEM_PTO_REFERENCE_INPUTS": "0"},
                   {"LINGQU_SHMEM_PTO_ACCESS_THREADS": "1"},
                   {"LINGQU_SHMEM_PTO_CONCURRENT_REQUESTS": "2"},
                   {"LINGQU_SHMEM_PTO_NEGATIVE_CHECKS": "1"},
                   {"LINGQU_SHMEM_PTO_MODEL_OUTPUT_CHECKSUM": "bad"})
        with tempfile.TemporaryDirectory() as directory:
            config = pathlib.Path(directory) / "probe.env"
            for change in changes:
                config.write_text("".join(f"{key}={shlex.quote(value)}\n" for key, value in
                                          {**values, **change}.items()))
                result = subprocess.run(["zsh", "-c", script, "probe-test", str(config)],
                                        capture_output=True, text=True, timeout=5)
                with self.subTest(change=change):
                    valid = not change or change == {"LINGQU_SHMEM_PTO_MODEL_PUBLISH": "1"}
                    self.assertEqual(result.returncode, 0 if valid else 2, result.stdout + result.stderr)
                    if valid:
                        self.assertIn("lingqu_shmem_pto.model_probe=1", result.stdout)
                        self.assertIn("lingqu_shmem_pto.model_output_bytes=54680", result.stdout)
                        self.assertIn("lingqu_shmem_pto.model_output_checksum=0x1234", result.stdout)
                        self.assertIn("lingqu_shmem_pto.model_publish=" +
                                      change.get("LINGQU_SHMEM_PTO_MODEL_PUBLISH", "0"), result.stdout)

    def test_same_mapping_two_dispatches_and_fail_closed_lifetime(self):
        source = (APP / "lingqu_shmem_pto_managed.c").read_text()
        self.assertIn("mem_service_client_map_managed_allocation", source)
        self.assertIn("lingqu_shmem_mem_service_acquire_owned", source)
        self.assertIn("mem_service_mapping_owner_adopt", source)
        self.assertIn("lingqu_shmem_mem_service_lease_begin", source)
        self.assertIn("lingqu_shmem_mem_service_lease_complete", source)
        self.assertIn("pthread_join", source)
        self.assertIn("probe_owner_close(state, true)", source)
        self.assertIn("mem_service_mapping_lease_apply(state->cpu_pin", source)
        self.assertIn("unsigned vectors[3] = {round == 1 || state->config.concurrent_requests == 2 ? 0 : 2,", source)
        self.assertIn("1, round + 1};", source)
        self.assertIn("pthread_create(&batch->threads[i], NULL, compute_caller", source)
        self.assertIn("batch->error = probe_batch(state)", source)
        batch = source.split("static int dispatch_batch(", 1)[1].split("static int readonly_cpu_probe", 1)[0]
        self.assertIn("request->compute = state->compute", batch)
        self.assertIn("allocate_metadata(request)", batch)
        self.assertNotIn("open_compute(request)", batch)
        self.assertNotIn("lingqu_shmem_mem_service_close", batch)
        self.assertLess(batch.index("pthread_join"), batch.index("free(request)"))
        self.assertIn("dispatch_vector(state, 1)", source)
        self.assertIn("dispatch_vector(state, 2)", source)
        self.assertNotIn("mem_service_client_put_object", source)
        self.assertNotIn("obmm_do_import", source)
        self.assertNotIn("obmm_async_map_register", source)
        self.assertIn("state->completion_unknown = true", source)
        self.assertIn("if (state->completion_unknown) return -EBUSY", source)
        self.assertIn("resources_retained=1", source)
        self.assertIn("for (;;) pause()", source)
        self.assertIn("lingqu_shmem_pto_endpoint_wait(state->endpoint, request.op_id", source)
        self.assertIn("lingqu_shmem_pto_endpoint_close_checked(state->endpoint) != -EBUSY", source)
        self.assertIn("state->config.recovery_expected && !recovered", source)
        self.assertIn("mem_service_client_acquire_object_at_node", source)
        self.assertIn("wait_holder_recovery_retired", source)
        self.assertIn("lingqu_shmem_pto_probe_revoked_read", source)
        self.assertIn("MANAGED_PTO_HOLDER_RECOVERY", source)
        self.assertIn("old_cpu_access=%s", source)
        self.assertIn("old_pto_access=%s", source)
        holder_dispatch = source.split(
            "static int dispatch_vector_checked(", 1
        )[1].split("static int dispatch_vector(", 1)[0]
        self.assertLess(holder_dispatch.index("state=submit-start"),
                        holder_dispatch.index("lingqu_shmem_pto_endpoint_submit"))
        self.assertLess(holder_dispatch.index("state=rejected"),
                        holder_dispatch.index("state=cpu-probe-start"))
        self.assertLess(holder_dispatch.index("state=cpu-probe-start"),
                        holder_dispatch.index("lingqu_shmem_pto_probe_revoked_read"))

    def test_initramfs_and_dual_node_runner_expose_managed_mode(self):
        build = (ROOT / "guest-linux/aarch64/scripts/build_initramfs.sh").read_text()
        run_app = (ROOT / "guest-linux/aarch64/initramfs/run_app").read_text()
        runner = (ROOT / "guest-linux/aarch64/scripts/run_ub_dual_node_apps.sh").read_text()
        self.assertIn("-DLINGQU_SHMEM_MANAGED_MAPPING", build)
        self.assertIn('"$LINGQU_SHMEM_PTO_MANAGED_SRC"', build)
        self.assertIn('"$MEM_SERVICE_MAPPING_OWNER_SRC"', build)
        self.assertIn('"$MEM_SERVICE_PROVIDER_SRC" "$MEM_SERVICE_PROVIDER_OBMM_SRC"', build)
        self.assertIn("--managed-config /tmp/managed-share.pto.conf", run_app)
        self.assertIn('access="${8:-cpu}"', run_app)
        self.assertIn("mem_service_object_share_access cpu", run_app)
        self.assertIn("managed_pto_selected()", runner)
        self.assertIn("holder_recovery_pto_selected()", runner)
        self.assertIn("pto_device_selected()", runner)
        self.assertIn('"${SCRIPT_DIR}/validate_managed_pto.py"', runner)
        self.assertIn("MANAGED_PTO_RESULT .*status=fail", runner)
        self.assertIn("lingqu_shmem_pto_submit_timeout_ms", run_app)
        self.assertIn("lingqu_shmem_pto_access_threads", run_app)
        self.assertIn('--access-threads "$LINGQU_SHMEM_PTO_ACCESS_THREADS"', runner)
        self.assertIn('--concurrent-requests "$LINGQU_SHMEM_PTO_CONCURRENT_REQUESTS"', runner)
        self.assertIn('lingqu_shmem_pto.concurrent_requests=$LINGQU_SHMEM_PTO_CONCURRENT_REQUESTS', runner)
        self.assertIn('echo "concurrent_requests=$(cmdline_value lingqu_shmem_pto.concurrent_requests 1)"', run_app)
        self.assertIn("--require-timeout-recovery", runner)
        self.assertIn("lingqu_shmem_pto_negative_checks", run_app)
        self.assertIn("--require-negative-checks", runner)
        self.assertIn("lingqu_shmem_pto_readonly_inputs", run_app)
        self.assertIn("--require-readonly-inputs", runner)
        self.assertIn("--require-reference-inputs", runner)
        self.assertIn('lingqu_shmem_pto.reference_inputs=$LINGQU_SHMEM_PTO_REFERENCE_INPUTS', runner)
        self.assertIn('echo "reference_inputs=$(cmdline_value lingqu_shmem_pto.reference_inputs 0)"', run_app)
        self.assertIn('--layout "$LINGQU_SHMEM_PTO_LAYOUT"', runner)
        self.assertIn('echo "size_bytes=$size"', run_app)
        self.assertIn('echo "home_node=$home_node"', run_app)
        self.assertIn('echo "allocation_home_provider=guest-node-$home_node"', run_app)
        self.assertIn('--home-node "$LINGQU_SHMEM_PTO_HOME_NODE"', runner)
        self.assertIn('--qemu-a "$nodea_qemu_log" --qemu-b "$nodeb_qemu_log"', runner)
        self.assertIn("export UBC_TRACE_DATA_PATH=1", runner)

    @unittest.skipUnless(shutil.which("zsh"), "zsh runner required")
    def test_runner_rejects_unsupported_access_before_guest_launch(self):
        runner = ROOT / "guest-linux/aarch64/scripts/run_ub_dual_node_apps.sh"
        with tempfile.TemporaryDirectory() as directory:
            config = pathlib.Path(directory) / "runner.env"
            for access, expected in (("unknown", "unsupported Memory Service access mode"),
                                     ("cpu-pto", "cpu-pto requires OBMM")):
                with self.subTest(access=access):
                    config.write_text(f"MEM_SERVICE_OBJECT_SHARE_ACCESS={access}\n"
                                      "MEM_SERVICE_OBJECT_SHARE_BACKEND=loopback\n")
                    result = subprocess.run(["zsh", str(runner), "--config", str(config),
                                             "--app", "mem_service_object_share"],
                                            capture_output=True, text=True, timeout=5)
                    self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
                    self.assertIn(expected, result.stderr)


if __name__ == "__main__":
    unittest.main()
