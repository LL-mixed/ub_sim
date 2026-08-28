from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def test_mem_service_ready_probe_tolerates_loaded_eight_node_guests():
    source = (ROOT / "apps" / "ssd_gsva_test" / "ssd_gsva_test.c").read_text()
    makefile = (ROOT / "apps" / "ssd_gsva_test" / "Makefile").read_text()

    assert '#define MEM_SERVICE_READY_ATTEMPTS 20' in source
    assert '#define MEM_SERVICE_READY_TIMEOUT_MS "1000"' in source
    assert "run_mem_service_argv_quiet(argv)" in source
    assert "(void)run_mem_service_argv(argv);" in source
    assert '(char *)"50"' not in source
    assert "$(OBMM_LDLIBS) -o $@" in makefile
