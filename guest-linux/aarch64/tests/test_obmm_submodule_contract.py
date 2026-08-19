import pathlib
import subprocess
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[3]
GITMODULES = ROOT / ".gitmodules"
LIBOBMM_MK = ROOT / "guest-linux/aarch64/common/libobmm.mk"
SIM_ADAPTOR = ROOT / "guest-linux/aarch64/common/obmm_vendor_adaptor_sim.c"
EXPECTED_PIN = "53011eed10716b422d2ac29199f68b55f7c5bdc5"

OBMM_COMMON_APPS = [
    "gsva_coh_test", "gsva_lifecycle_test", "gsva_query", "gva_direct",
    "gva_manager", "npu_gsva_test", "obmm_coh_test",
    "obmm_dataplane_microbench", "obmm_gsva", "obmm_import_stress",
    "ssd_gsva_test",
]


class ObmmSubmoduleContractTest(unittest.TestCase):
    def test_gitmodules_declares_obmm(self):
        text = GITMODULES.read_text()
        self.assertIn('[submodule "vendor/obmm"]', text)
        self.assertIn("url = https://atomgit.com/openeuler/obmm.git", text)
        self.assertIn("branch = master", text)

    def test_obmm_pinned_to_expected_revision(self):
        out = subprocess.run(
            ["git", "-C", str(ROOT), "ls-tree", "HEAD", "vendor/obmm"],
            check=True, capture_output=True, text=True,
        ).stdout
        self.assertIn(EXPECTED_PIN, out)

    def test_libobmm_mk_wires_submodule(self):
        text = LIBOBMM_MK.read_text()
        self.assertIn("vendor/obmm/src/libobmm/libobmm.c", text)
        self.assertIn("obmm_vendor_adaptor_sim.c", text)
        self.assertIn("kernel_ub/include/uapi", text)

    def test_sim_adaptor_implements_vendor_seam(self):
        text = SIM_ADAPTOR.read_text()
        for symbol in (
            "vendor_adapt_export", "free_vendor_info",
            "vendor_fixup_import_cmd", "vendor_cleanup_import_cmd",
            "vendor_fixup_preimport_cmd", "vendor_cleanup_preimport_cmd",
        ):
            self.assertIn(symbol, text)

    def test_obmm_apps_use_libobmm_mk(self):
        for app in OBMM_COMMON_APPS:
            with self.subTest(app=app):
                makefile = (
                    ROOT / "guest-linux/aarch64/apps" / app / "Makefile"
                ).read_text()
                self.assertIn("libobmm.mk", makefile)
                self.assertIn("$(OBMM_SRCS)", makefile)


if __name__ == "__main__":
    unittest.main()
