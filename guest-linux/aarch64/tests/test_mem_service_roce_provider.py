import re
import subprocess
import tempfile
import textwrap
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SERVICE_DIR = ROOT / "components" / "mem_service"
PROVIDER_DIR = SERVICE_DIR / "providers"
PROVIDER_HEADER = PROVIDER_DIR / "mem_service_provider_roce.h"
PROVIDER_SOURCE = PROVIDER_DIR / "mem_service_provider_roce.c"
PROVIDER_CLI = PROVIDER_DIR / "mem_service_provider_roce_cli.c"
PROVIDER_LAYOUT = PROVIDER_DIR / "README.md"
CORE_PROVIDER_HEADER = SERVICE_DIR / "mem_service_provider.h"
CORE_PROVIDER_SOURCE = SERVICE_DIR / "mem_service_provider.c"
MEM_SERVICE_MAKEFILE = ROOT / "apps" / "mem_service" / "Makefile"
PACKAGE_MANIFEST = ROOT / "apps" / "mem_service" / "package-manifest.txt"
RELEASE_MANIFEST = ROOT / "apps" / "mem_service" / "release-manifest.txt"
MESH_CONFIG_EXAMPLE = (
    ROOT
    / "apps"
    / "mem_service"
    / "configs"
    / "providers"
    / "roce"
    / "mesh.example.conf"
)


class MemServiceRoceProviderTests(unittest.TestCase):
    def test_provider_is_explicitly_opt_in_and_transport_stays_out_of_core(self):
        makefile = MEM_SERVICE_MAKEFILE.read_text()
        package_manifest = PACKAGE_MANIFEST.read_text()
        release_manifest = RELEASE_MANIFEST.read_text()
        layout = PROVIDER_LAYOUT.read_text()
        core = CORE_PROVIDER_HEADER.read_text() + CORE_PROVIDER_SOURCE.read_text()

        self.assertTrue(PROVIDER_HEADER.exists())
        self.assertTrue(PROVIDER_SOURCE.exists())
        self.assertTrue(PROVIDER_CLI.exists())
        self.assertTrue(MESH_CONFIG_EXAMPLE.exists())
        self.assertIn("linqu_mem_service_provider_<name>", layout)
        self.assertIn("never an automatic fallback", layout)
        self.assertIn(
            "service data-plane\n  readiness requires a completed peer transfer",
            layout,
        )
        self.assertIn("linqu_mem_service_provider_roce:", makefile)
        self.assertIn("-lrdmacm -libverbs", makefile)
        self.assertIn("MEM_SERVICE_PROVIDER_SDK_SRCS :=", makefile)
        self.assertIn(
            "payload_provider_roce_sources="
            "$${sourcedir}/mem_service_provider_roce.c",
            makefile,
        )
        self.assertIn(
            "pkgconfig_payload_provider_roce_libs=-lrdmacm -libverbs",
            package_manifest,
        )
        self.assertIn(
            "provider_source=src/lingqu/mem_service/"
            "mem_service_provider_roce.c",
            release_manifest,
        )
        self.assertIn("mesh-serve --config <path>", layout)
        self.assertIn(
            "model runtime uses the neutral provider SDK in the model\n"
            "  process",
            layout,
        )
        self.assertIn("mem_service_provider_channel_bind", core)
        self.assertIn("mem_service_provider_remote_region_encode", core)
        self.assertIn("mem_service_provider_channel_transfer", core)
        all_rule = re.search(r"^all:([^\n]+)$", makefile, re.MULTILINE)
        self.assertIsNotNone(all_rule)
        self.assertNotIn("provider_roce", all_rule.group(1))
        for forbidden in ("roce", "rdma", "ibverbs", "rdmacm"):
            self.assertNotIn(forbidden, core.lower())

    def test_provider_uses_one_sided_payload_and_fail_closed_readiness(self):
        source = PROVIDER_SOURCE.read_text()

        self.assertIn("<rdma/rdma_cma.h>", source)
        self.assertIn("<infiniband/verbs.h>", source)
        self.assertIn("IBV_WR_RDMA_WRITE", source)
        self.assertIn("IBV_QPT_RC", source)
        self.assertIn("IBV_ACCESS_REMOTE_WRITE", source)
        self.assertIn("MEM_SERVICE_PROVIDER_CAP_PEER_TRANSFER", source)
        self.assertIn("MEM_SERVICE_PROVIDER_STATE_DEGRADED", source)
        self.assertIn("context->transfer_verified = true", source)
        self.assertIn("mem_service_provider_roce_endpoint_listen", source)
        self.assertIn("mem_service_provider_roce_endpoint_accept", source)
        self.assertIn(
            "mem_service_provider_registry_data_plane_ready(&registry)",
            source,
        )
        self.assertRegex(
            source,
            r"checksum\s*!=\s*message\.checksum",
        )
        self.assertRegex(
            source,
            re.compile(
                r"context->transfer_verified = true;\s*/\*.*?"
                r"memset\(payload, 0, \(size_t\)payload_bytes\);\s*"
                r"if \(mem_service_roce_send_control\(",
                re.DOTALL,
            ),
        )
        self.assertNotRegex(
            source,
            r"if \(server\) \{\s*"
            r"memset\(payload, 0, \(size_t\)payload_bytes\);\s*"
            r"if \(mem_service_roce_receive_control\(",
        )
        self.assertNotRegex(source, r"\bsocket\s*\(")
        self.assertNotIn("SOCK_STREAM", source)
        self.assertNotIn("TCP_NODELAY", source)

    def test_cli_argument_contract_with_stub_provider(self):
        stub_source = textwrap.dedent(
            """
            #include "mem_service_provider_roce.h"
            #include "../mem_service_daemon.h"
            #include <stdio.h>
            #include <string.h>

            static int probe(
                void *context,
                enum mem_service_provider_state *state_out)
            {
                if (context == 0 || state_out == 0) return -1;
                *state_out = MEM_SERVICE_PROVIDER_STATE_READY;
                return 0;
            }

            static int submit(
                void *context,
                const struct mem_service_transfer_request *request,
                uint64_t *completion_id_out)
            {
                if (context == 0 || request == 0 ||
                    completion_id_out == 0) return -1;
                *completion_id_out = 1;
                return 0;
            }

            static int poll(
                void *context,
                uint64_t completion_id,
                struct mem_service_transfer_completion *completion_out)
            {
                if (context == 0 || completion_id != 1 ||
                    completion_out == 0) return -1;
                memset(completion_out, 0, sizeof(*completion_out));
                completion_out->id = completion_id;
                return 0;
            }

            static const struct mem_service_provider_ops ops = {
                .probe = probe,
                .submit_transfer = submit,
                .poll_completion = poll,
            };
            static int provider_context;

            int mem_service_provider_roce_probe_device(
                const char *device, char *detail, size_t detail_len)
            {
                snprintf(detail, detail_len,
                         "device=%s port=1 state=active link_layer=ethernet",
                         device);
                return 0;
            }

            static int fill_result(
                const struct mem_service_provider_roce_config *config,
                uint64_t payload_bytes,
                uint32_t iterations,
                struct mem_service_provider_roce_canary_result *result)
            {
                memset(result, 0, sizeof(*result));
                snprintf(result->device, sizeof(result->device), "%s",
                         config->expected_device);
                snprintf(result->local_ipv4, sizeof(result->local_ipv4), "%s",
                         config->local_ipv4);
                snprintf(result->peer_ipv4, sizeof(result->peer_ipv4), "%s",
                         config->peer_ipv4);
                result->payload_bytes = payload_bytes;
                result->iterations = iterations;
                result->checksum = 0x1234;
                result->elapsed_us = 1000;
                result->data_plane_ready = true;
                return 0;
            }

            int mem_service_provider_roce_run_server_canary(
                const struct mem_service_provider_roce_config *config,
                uint64_t payload_bytes,
                uint32_t iterations,
                struct mem_service_provider_roce_canary_result *result)
            {
                return fill_result(config, payload_bytes, iterations, result);
            }

            int mem_service_provider_roce_run_client_canary(
                const struct mem_service_provider_roce_config *config,
                uint64_t payload_bytes,
                uint32_t iterations,
                struct mem_service_provider_roce_canary_result *result)
            {
                return fill_result(config, payload_bytes, iterations, result);
            }

            int mem_service_provider_roce_run_protocol_fixture(void)
            {
                puts("mem_service roce-provider-fixtures: status=ok");
                return 0;
            }

            int mem_service_provider_roce_endpoint_open(
                struct mem_service_provider_roce_endpoint *endpoint,
                const struct mem_service_provider_roce_config *config,
                bool server)
            {
                (void)config;
                (void)server;
                endpoint->implementation = &provider_context;
                return 0;
            }

            int mem_service_provider_roce_endpoint_verify(
                struct mem_service_provider_roce_endpoint *endpoint,
                bool server,
                uint64_t payload_bytes,
                uint32_t iterations,
                struct mem_service_provider_roce_canary_result *result)
            {
                (void)endpoint;
                (void)server;
                memset(result, 0, sizeof(*result));
                snprintf(result->device, sizeof(result->device),
                         "rocep1s0f0");
                snprintf(result->local_ipv4, sizeof(result->local_ipv4),
                         "192.0.2.10");
                snprintf(result->peer_ipv4, sizeof(result->peer_ipv4),
                         "192.0.2.11");
                result->payload_bytes = payload_bytes;
                result->iterations = iterations;
                result->data_plane_ready = true;
                return 0;
            }

            int mem_service_provider_roce_endpoint_registration(
                struct mem_service_provider_roce_endpoint *endpoint,
                struct mem_service_provider_registration *registration_out)
            {
                (void)endpoint;
                memset(registration_out, 0, sizeof(*registration_out));
                registration_out->name = "roce";
                registration_out->instance = "stub";
                registration_out->capabilities =
                    MEM_SERVICE_PROVIDER_CAP_PEER_TRANSFER;
                registration_out->ops = &ops;
                registration_out->context = &provider_context;
                return 0;
            }

            void mem_service_provider_roce_endpoint_close(
                struct mem_service_provider_roce_endpoint *endpoint)
            {
                endpoint->implementation = 0;
            }

            int mem_service_run_unix_daemon_with_runtime(
                const char *listen_spec,
                const char *store_path,
                const char *metrics_listen_spec,
                const char *storage_root,
                const struct mem_service_daemon_runtime *runtime)
            {
                (void)store_path;
                (void)metrics_listen_spec;
                (void)storage_root;
                printf("stub_daemon=ok listen=%s providers=%zu\\n",
                       listen_spec,
                       runtime->providers->count);
                return 0;
            }
            """
        )
        with tempfile.TemporaryDirectory() as temporary:
            temporary_path = Path(temporary)
            stub_path = temporary_path / "roce_stub.c"
            binary = temporary_path / "linqu_mem_service_provider_roce"
            stub_path.write_text(stub_source)
            subprocess.run(
                [
                    "cc",
                    "-std=c11",
                    "-Wall",
                    "-Wextra",
                    "-I",
                    str(PROVIDER_DIR),
                    "-I",
                    str(SERVICE_DIR),
                    str(PROVIDER_CLI),
                    str(stub_path),
                    str(CORE_PROVIDER_SOURCE),
                    "-o",
                    str(binary),
                ],
                check=True,
                capture_output=True,
                text=True,
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
            self.assertIn("status=ok", fixtures.stdout)

            probe = subprocess.run(
                [str(binary), "probe", "--device", "rocep1s0f0"],
                check=True,
                capture_output=True,
                text=True,
            )
            self.assertIn("status=available", probe.stdout)

            invalid = subprocess.run(
                [str(binary), "client-canary", "--local-ip", "192.168.2.1"],
                capture_output=True,
                text=True,
            )
            self.assertEqual(invalid.returncode, 2)

            canary = subprocess.run(
                [
                    str(binary),
                    "client-canary",
                    "--local-ip",
                    "192.168.2.1",
                    "--peer-ip",
                    "192.168.2.2",
                    "--port",
                    "19100",
                    "--device",
                    "rocep1s0f1",
                    "--bytes",
                    "4096",
                    "--iterations",
                    "3",
                ],
                check=True,
                capture_output=True,
                text=True,
            )
            self.assertIn("role=client status=ok", canary.stdout)
            self.assertIn("data_plane_ready=1", canary.stdout)
            self.assertIn("bytes=4096 iterations=3", canary.stdout)

            config = temporary_path / "mesh.conf"
            config.write_text(
                "version=1\n"
                "listen=unix:/tmp/mem-service-roce-test.sock\n"
                "verify_bytes=4096\n"
                "verify_iterations=2\n"
                "timeout_ms=1000\n"
                "endpoint=server,192.0.2.10,192.0.2.11,"
                "19110,rocep1s0f0\n"
            )
            mesh = subprocess.run(
                [
                    str(binary),
                    "mesh-serve",
                    "--config",
                    str(config),
                ],
                check=True,
                capture_output=True,
                text=True,
            )
            self.assertIn("mesh_endpoint=0 status=ready", mesh.stdout)
            self.assertIn("stub_daemon=ok", mesh.stdout)

            config.write_text(
                "version=1\n"
                "listen=unix:/tmp/mem-service-roce-test.sock\n"
                "timeout_ms=1000\n"
                "timeout_ms=2000\n"
                "endpoint=server,192.0.2.10,192.0.2.11,"
                "19110,rocep1s0f0\n"
            )
            duplicate = subprocess.run(
                [
                    str(binary),
                    "mesh-serve",
                    "--config",
                    str(config),
                ],
                capture_output=True,
                text=True,
            )
            self.assertEqual(duplicate.returncode, 2)
            self.assertIn("invalid mesh config", duplicate.stderr)


if __name__ == "__main__":
    unittest.main()
