from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def test_ub_chat_is_packaged_from_app_directory():
    build_script = (ROOT / "scripts" / "build_initramfs.sh").read_text()

    assert 'CHAT_SRC="$ROOT_DIR/apps/ub_chat/ub_chat.c"' in build_script
    assert not (ROOT / "ub_chat.c").exists()
    assert (ROOT / "apps" / "ub_chat" / "ub_chat.c").exists()
    assert (ROOT / "apps" / "ub_chat" / "Makefile").exists()


def test_ub_rpc_uses_canonical_app_source():
    build_script = (ROOT / "scripts" / "build_initramfs.sh").read_text()
    run_demo = (ROOT / "initramfs" / "run_demo").read_text()
    app_dir = ROOT / "apps" / "ub_rpc"

    assert 'RPC_SRC="$ROOT_DIR/apps/ub_rpc/ub_rpc.c"' in build_script
    assert "linqu_ub_rpc=1" in run_demo
    assert (app_dir / "ub_rpc.c").exists()
    assert (app_dir / "Makefile").exists()
    assert not (ROOT / "apps" / "ub_rpc_demo").exists()


def test_ub_udma_uses_canonical_app_source():
    build_script = (ROOT / "scripts" / "build_initramfs.sh").read_text()
    run_demo = (ROOT / "initramfs" / "run_demo").read_text()
    app_dir = ROOT / "apps" / "ub_udma"

    assert 'UDMA_SRC="$ROOT_DIR/apps/ub_udma/ub_udma.c"' in build_script
    assert 'UDMA_BIN="$OUT_DIR/linqu_ub_udma"' in build_script
    assert "linqu_ub_udma_demo" not in build_script
    assert "linqu_ub_udma=1" in run_demo
    assert (app_dir / "ub_udma.c").exists()
    assert (app_dir / "Makefile").exists()
    assert not (ROOT / "apps" / "ub_udma_demo").exists()


def test_ub_tcp_each_server_uses_canonical_app_source():
    build_script = (ROOT / "scripts" / "build_initramfs.sh").read_text()
    app_dir = ROOT / "apps" / "ub_tcp_each_server"

    assert (
        'TCP_EACH_SERVER_SRC="$ROOT_DIR/apps/ub_tcp_each_server/ub_tcp_each_server.c"'
        in build_script
    )
    assert (app_dir / "ub_tcp_each_server.c").exists()
    assert not (app_dir / "ub_tcp_each_server_demo.c").exists()
    assert (app_dir / "Makefile").exists()


def test_obmm_dataplane_microbench_has_independent_app_build():
    build_script = (ROOT / "scripts" / "build_initramfs.sh").read_text()
    app_dir = ROOT / "apps" / "obmm_dataplane_microbench"

    assert (
        'OBMM_DATAPLANE_MICROBENCH_SRC="$ROOT_DIR/apps/obmm_dataplane_microbench/obmm_dataplane_microbench.c"'
        in build_script
    )
    assert (app_dir / "obmm_dataplane_microbench.c").exists()
    assert (app_dir / "Makefile").exists()


def test_npu_gsva_test_has_independent_app_build():
    build_script = (ROOT / "scripts" / "build_initramfs.sh").read_text()
    app_dir = ROOT / "apps" / "npu_gsva_test"

    assert 'NPU_GSVA_TEST_SRC="$ROOT_DIR/apps/npu_gsva_test/npu_gsva_test.c"' in build_script
    assert (app_dir / "npu_gsva_test.c").exists()
    assert (app_dir / "Makefile").exists()


def test_ssd_gsva_test_has_independent_app_build():
    build_script = (ROOT / "scripts" / "build_initramfs.sh").read_text()
    app_dir = ROOT / "apps" / "ssd_gsva_test"

    assert 'SSD_GSVA_TEST_SRC="$ROOT_DIR/apps/ssd_gsva_test/ssd_gsva_test.c"' in build_script
    assert (app_dir / "ssd_gsva_test.c").exists()
    assert (app_dir / "Makefile").exists()


def test_obmm_gsva_uses_canonical_app_source():
    build_script = (ROOT / "scripts" / "build_initramfs.sh").read_text()
    run_demo = (ROOT / "initramfs" / "run_demo").read_text()
    app_dir = ROOT / "apps" / "obmm_gsva"

    assert 'OBMM_GSVA_SRC="$ROOT_DIR/apps/obmm_gsva/obmm_gsva.c"' in build_script
    assert 'OBMM_GSVA_BIN="$OUT_DIR/linqu_ub_obmm_gsva"' in build_script
    assert "linqu_ub_obmm_gsva_demo" not in build_script
    assert "linqu_obmm_gsva=1" in run_demo
    assert (app_dir / "obmm_gsva.c").exists()
    assert (app_dir / "Makefile").exists()
    assert not (ROOT / "apps" / "obmm_gsva_demo").exists()


def test_gva_direct_uses_canonical_app_source():
    build_script = (ROOT / "scripts" / "build_initramfs.sh").read_text()
    run_demo = (ROOT / "initramfs" / "run_demo").read_text()
    app_dir = ROOT / "apps" / "gva_direct"

    assert 'GVA_DIRECT_SRC="$ROOT_DIR/apps/gva_direct/gva_direct.c"' in build_script
    assert 'GVA_DIRECT_BIN="$OUT_DIR/linqu_gva_direct"' in build_script
    assert "linqu_gva_direct_demo" not in build_script
    assert "linqu_gva_direct=1" in run_demo
    assert (app_dir / "gva_direct.c").exists()
    assert (app_dir / "Makefile").exists()
    assert not (ROOT / "apps" / "gva_direct_demo").exists()
