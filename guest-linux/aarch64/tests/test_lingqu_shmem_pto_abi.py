import pathlib
import subprocess
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[3]
HEADER = ROOT / "crates" / "sim-qemu" / "include" / "linqu_shmem_pto_abi.h"
BRIDGE_HEADER = ROOT / "crates" / "sim-qemu" / "include" / "linqu_ub_bridge.h"
QEMU_UBC_HEADER = ROOT / "vendor" / "qemu_8.2.0_ub" / "include" / "hw" / "ub" / "ub_ubc.h"
QEMU_UBC_SOURCE = ROOT / "vendor" / "qemu_8.2.0_ub" / "hw" / "ub" / "ub_ubc.c"
QEMU_ABI_HEADER = (
    ROOT
    / "vendor"
    / "qemu_8.2.0_ub"
    / "include"
    / "hw"
    / "ub"
    / "linqu_shmem_pto_abi.h"
)


def _compile_header(header, compiler, language, standard):
    subprocess.run(
        [
            compiler,
            f"-std={standard}",
            "-Werror",
            "-fsyntax-only",
            "-x",
            language,
            "-include",
            str(header),
            "/dev/null",
        ],
        check=True,
    )


class LingquShmemPtoAbiTest(unittest.TestCase):
    def test_compiles_as_c_and_cpp(self):
        for header in (HEADER, BRIDGE_HEADER):
            _compile_header(header, "cc", "c", "c11")
            _compile_header(header, "c++", "c++", "c++17")

    def test_uses_the_default_ub_gm_contract(self):
        source = HEADER.read_text()
        self.assertIn("LingquPtoDispatchSlotV2", source)
        self.assertIn("LINGQU_PTO_DISPATCH_SLOT_TAG_V2 10u", source)
        self.assertIn("LINGQU_PTO_DISPATCH_SLOT_OP_ID_OFFSET 1u", source)
        self.assertIn("LINGQU_PTO_DISPATCH_SLOT_CONTROL_IOVA_OFFSET 9u", source)
        self.assertIn("LingquPtoDispatchControlV2", source)
        self.assertIn("LingquShmemMemrefV1", source)
        self.assertIn("PtoSimUbGmAccessOpsV1", source)
        self.assertIn("PtoSimUbGmAuthorizedMemrefV1", source)
        self.assertNotIn("EXTERNAL_GM", source)
        self.assertNotIn("external_memref", source)
        self.assertNotIn("NPU_OP_", source)

    def test_qemu_bridge_exposes_authorized_v2_submission(self):
        source = BRIDGE_HEADER.read_text()
        self.assertIn("linqu_ub_bridge_query_ub_gm_callable_v1", source)
        self.assertIn("linqu_ub_bridge_submit_ub_gm_v2", source)
        self.assertIn("PtoSimUbGmAuthorizedMemrefV1", source)

    def test_qemu_ubc_reserves_an_explicit_default_disabled_pto_cna(self):
        header = QEMU_UBC_HEADER.read_text()
        source = QEMU_UBC_SOURCE.read_text()
        self.assertIn("uint32_t pto_device_cna;", header)
        self.assertIn(
            'DEFINE_PROP_UINT32("pto-device-cna", BusControllerDev, pto_device_cna, 0)',
            source,
        )

    def test_qemu_mirror_header_compiles_and_matches_wire_contract(self):
        _compile_header(QEMU_ABI_HEADER, "cc", "c", "c11")
        public = HEADER.read_text()
        qemu = QEMU_ABI_HEADER.read_text()
        for contract in (
            "#define LINGQU_PTO_DISPATCH_ABI_V2 2u",
            "#define LINGQU_SHMEM_MEMREF_ABI_V1 1u",
            "#define LINGQU_PTO_SCALAR_ABI_V1 1u",
            "#define LINGQU_PTO_DISPATCH_SLOT_TAG_V2 10u",
            "#define LINGQU_PTO_DISPATCH_SLOT_OP_ID_OFFSET 1u",
            "#define LINGQU_PTO_DISPATCH_SLOT_CONTROL_IOVA_OFFSET 9u",
            "#define LINGQU_PTO_DISPATCH_SLOT_RESERVED_OFFSET 17u",
            "sizeof(LingquPtoDispatchControlV2) == 64",
            "sizeof(LingquShmemMemrefV1) == 80",
            "sizeof(LingquPtoScalarV1) == 24",
            "sizeof(PtoSimUbGmBindingV1) == 64",
            "sizeof(PtoSimUbGmAuthorizedMemrefV1) == 192",
            "sizeof(PtoSimUbGmAccessOpsV1) == 32",
        ):
            self.assertIn(contract, public)
            self.assertIn(contract, qemu)

    def test_qemu_v2_ingress_is_fail_closed_before_bridge_submission(self):
        source = QEMU_UBC_SOURCE.read_text()
        submit_start = source.index("static int linqu_uapi_submit_ub_gm_v2")
        submit_end = source.index(
            "static const char *linqu_uapi_ub_gm_error_code", submit_start
        )
        submit = source[submit_start:submit_end]
        ordered_checks = (
            "linqu_uapi_wire_is_zero",
            "linqu_uapi_decode_control",
            "control.requester_cna != ubc_dev->pto_device_cna",
            "linqu_ub_bridge_query_ub_gm_callable_v1",
            "linqu_uapi_decode_memref",
            "linqu_uapi_validate_contiguous_memref",
            "ubc_obmm_resolve_async_map",
            "resolved.map_generation != memref->opaque_mapping_ref",
            "linqu_ub_gm_register_dispatch",
            "linqu_ub_bridge_submit_ub_gm_v2",
        )
        positions = [submit.index(check) for check in ordered_checks]
        self.assertEqual(positions, sorted(positions))
        self.assertIn("memset(control_crc_wire + 56, 0", submit)
        self.assertIn("crc ^ 0xffffffffu", submit)
        self.assertIn("SIM_DEC_GVA_ACCESS_READ_ONLY", submit)
        self.assertIn('"bridge_submit_failed"', submit)

    def test_qemu_callbacks_revalidate_and_unbind_request_scoped_mappings(self):
        source = QEMU_UBC_SOURCE.read_text()
        callback_contracts = {
            "linqu_ub_gm_read": "ubc_sim_dec_remote_read",
            "linqu_ub_gm_write": "ubc_sim_dec_remote_write",
            "linqu_ub_gm_fence": "linqu_ub_gm_account_access",
        }
        for name, backend_call in callback_contracts.items():
            start = source.index(f"static int {name}")
            end = source.index("\n}\n", start) + 3
            callback = source[start:end]
            self.assertIn("linqu_ub_gm_binding_snapshot", callback)
            self.assertIn("linqu_ub_gm_mapping_still_authorized", callback)
            self.assertIn(backend_call, callback)

        mapping_check_start = source.index(
            "static bool linqu_ub_gm_mapping_still_authorized"
        )
        mapping_check_end = source.index("\n}\n", mapping_check_start) + 3
        mapping_check = source[mapping_check_start:mapping_check_end]
        self.assertIn("ubc_obmm_resolve_async_map", mapping_check)
        self.assertIn("resolved->map_generation == binding->mapping_ref", mapping_check)
        self.assertIn("binding->remote_base > UINT64_MAX - offset", mapping_check)

        resolver_start = source.index("bool ubc_obmm_resolve_async_map")
        resolver_end = source.index("\n}\n", resolver_start) + 3
        resolver = source[resolver_start:resolver_end]
        self.assertIn("entry->remote_uba > UINT64_MAX - offset", resolver)
        self.assertIn("entry->remote_uba + offset > UINT64_MAX - length", resolver)

        self.assertIn('"completion_success"', source)
        self.assertIn('"completion_failure"', source)
        self.assertIn('"doorbell_failed"', source)
        for code in (
            "pto_ub_gm_unsupported_callable",
            "pto_ub_gm_bad_control_table",
            "pto_ub_gm_bad_memref",
            "pto_ub_gm_unbound",
            "pto_ub_gm_access_denied",
            "pto_ub_gm_authorization_timeout",
            "pto_ub_gm_callback_failed",
            "pto_ub_gm_execution_failed",
        ):
            self.assertIn(code, source)


if __name__ == "__main__":
    unittest.main()
