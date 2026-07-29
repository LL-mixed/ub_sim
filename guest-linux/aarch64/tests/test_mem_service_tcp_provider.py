import socket
import subprocess
import tempfile
import time
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SERVICE_DIR = ROOT / "components" / "mem_service"
PROVIDER_DIR = SERVICE_DIR / "providers"
PROVIDER_HEADER = PROVIDER_DIR / "mem_service_provider_tcp.h"
PROVIDER_SOURCE = PROVIDER_DIR / "mem_service_provider_tcp.c"
PROVIDER_CLI = PROVIDER_DIR / "mem_service_provider_tcp_cli.c"
CORE_PROVIDER_SOURCE = SERVICE_DIR / "mem_service_provider.c"
CONFORMANCE_SOURCE = (
    ROOT / "tests" / "mem_service_tcp_provider_conformance.c"
)
MEM_SERVICE_MAKEFILE = ROOT / "apps" / "mem_service" / "Makefile"
PACKAGE_MANIFEST = ROOT / "apps" / "mem_service" / "package-manifest.txt"
RELEASE_MANIFEST = ROOT / "apps" / "mem_service" / "release-manifest.txt"


def unused_loopback_port():
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as listener:
        listener.bind(("127.0.0.1", 0))
        return listener.getsockname()[1]


def compile_binary(output, sources):
    subprocess.run(
        [
            "cc",
            "-std=c11",
            "-D_DEFAULT_SOURCE",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-pthread",
            "-I",
            str(SERVICE_DIR),
            "-I",
            str(PROVIDER_DIR),
            *(str(source) for source in sources),
            "-o",
            str(output),
        ],
        check=True,
        capture_output=True,
        text=True,
    )


class MemServiceTcpProviderTests(unittest.TestCase):
    def test_provider_is_transport_neutral_and_explicitly_buildable(self):
        source = PROVIDER_SOURCE.read_text()
        makefile = MEM_SERVICE_MAKEFILE.read_text()
        package_manifest = PACKAGE_MANIFEST.read_text()
        release_manifest = RELEASE_MANIFEST.read_text()
        core = CORE_PROVIDER_SOURCE.read_text()

        self.assertTrue(PROVIDER_HEADER.exists())
        self.assertTrue(PROVIDER_CLI.exists())
        self.assertTrue(CONFORMANCE_SOURCE.exists())
        self.assertIn("linqu_mem_service_provider_tcp:", makefile)
        self.assertIn("tcp-provider-smoke:", makefile)
        self.assertIn("mem_service_provider_tcp.h", makefile)
        self.assertIn("mem_service_provider_tcp.c", makefile)
        self.assertIn(
            "payload_provider_tcp_sources="
            "$${sourcedir}/mem_service_provider_tcp.c",
            makefile,
        )
        self.assertIn("payload_provider_tcp_libs=-pthread", makefile)
        self.assertIn(
            "pkgconfig_payload_provider_tcp_sources="
            "${sourcedir}/mem_service_provider_tcp.c",
            package_manifest,
        )
        self.assertIn(
            "public_header=include/lingqu/mem_service/"
            "mem_service_provider_tcp.h",
            release_manifest,
        )
        self.assertIn(
            "provider_source=src/lingqu/mem_service/"
            "mem_service_provider_tcp.c",
            release_manifest,
        )
        self.assertIn("SOCK_STREAM", source)
        self.assertIn("TCP_NODELAY", source)
        self.assertIn("MEM_SERVICE_PROVIDER_CAP_PEER_TRANSFER", source)
        self.assertIn("MEM_SERVICE_PROVIDER_CAP_RECEIVE_FENCE", source)
        self.assertIn('registration_out->name = "tcp"', source)
        self.assertIn("mem_service_tcp_receiver_main", source)
        self.assertIn("MEM_SERVICE_PROVIDER_STATE_UNAVAILABLE", source)
        self.assertNotIn("TCP_NODELAY", core)
        self.assertNotIn("SOCK_STREAM", core)

    def test_cli_runs_real_loopback_canary(self):
        with tempfile.TemporaryDirectory() as temporary:
            binary = Path(temporary) / "linqu_mem_service_provider_tcp"
            compile_binary(
                binary,
                [PROVIDER_CLI, PROVIDER_SOURCE, CORE_PROVIDER_SOURCE],
            )
            missing = subprocess.run(
                [str(binary)],
                capture_output=True,
                text=True,
            )
            self.assertEqual(missing.returncode, 2)
            self.assertIn("Usage:", missing.stderr)

            fixtures = subprocess.run(
                [str(binary), "protocol-fixtures"],
                check=True,
                capture_output=True,
                text=True,
            )
            self.assertIn("corruption=fail-closed", fixtures.stdout)

            port = unused_loopback_port()
            common = [
                "--local-ip",
                "127.0.0.1",
                "--peer-ip",
                "127.0.0.1",
                "--port",
                str(port),
                "--bytes",
                "65536",
                "--iterations",
                "4",
                "--timeout-ms",
                "10000",
            ]
            server = subprocess.Popen(
                [str(binary), "server-canary", *common],
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
            )
            try:
                time.sleep(0.1)
                client = subprocess.run(
                    [str(binary), "client-canary", *common],
                    capture_output=True,
                    text=True,
                    timeout=15,
                )
                server_stdout, server_stderr = server.communicate(timeout=15)
            finally:
                if server.poll() is None:
                    server.kill()
                    server.wait()
            self.assertEqual(server.returncode, 0, server_stderr)
            self.assertEqual(client.returncode, 0, client.stderr)
            self.assertIn("role=server status=ok", server_stdout)
            self.assertIn("role=client status=ok", client.stdout)
            self.assertIn("data_plane_ready=1", client.stdout)
            self.assertIn("bytes=65536 iterations=4", client.stdout)

    def test_neutral_provider_conformance(self):
        with tempfile.TemporaryDirectory() as temporary:
            binary = Path(temporary) / "tcp_provider_conformance"
            compile_binary(
                binary,
                [
                    CONFORMANCE_SOURCE,
                    PROVIDER_SOURCE,
                    CORE_PROVIDER_SOURCE,
                ],
            )
            result = subprocess.run(
                [str(binary), str(unused_loopback_port())],
                check=True,
                capture_output=True,
                text=True,
                timeout=20,
            )
            self.assertIn("registration=verified", result.stdout)
            self.assertIn("bounds=fail-closed", result.stdout)
            self.assertIn("completion=split+out-of-order", result.stdout)
            self.assertIn("receive_fence=demuxed", result.stdout)
            self.assertIn("receive_mode=wait", result.stdout)
            self.assertIn("checksum=fail-closed", result.stdout)


if __name__ == "__main__":
    unittest.main()
