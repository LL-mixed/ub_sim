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
    init_source = (ROOT / "init.c").read_text()
    run_demo = (ROOT / "initramfs" / "run_demo").read_text()
    dual_runner = (ROOT / "scripts" / "run_ub_dual_node_apps.sh").read_text()
    app_dir = ROOT / "apps" / "ub_rpc"

    assert 'RPC_SRC="$ROOT_DIR/apps/ub_rpc/ub_rpc.c"' in build_script
    assert "linqu_ub_rpc=1" in init_source
    assert "linqu_ub_rpc=1" in run_demo
    assert "linqu_ub_rpc_demo" not in init_source
    assert "linqu_ub_rpc_demo" not in run_demo
    assert "linqu_ub_rpc_demo" not in dual_runner
    assert "ub rpc (app|demo)" not in dual_runner
    assert (app_dir / "ub_rpc.c").exists()
    assert (app_dir / "Makefile").exists()
    assert not (ROOT / "apps" / "ub_rpc_demo").exists()


def test_ub_udma_uses_canonical_app_source():
    build_script = (ROOT / "scripts" / "build_initramfs.sh").read_text()
    init_source = (ROOT / "init.c").read_text()
    run_demo = (ROOT / "initramfs" / "run_demo").read_text()
    dual_runner = (ROOT / "scripts" / "run_ub_dual_node_apps.sh").read_text()
    app_dir = ROOT / "apps" / "ub_udma"

    assert 'UDMA_SRC="$ROOT_DIR/apps/ub_udma/ub_udma.c"' in build_script
    assert 'UDMA_BIN="$OUT_DIR/linqu_ub_udma"' in build_script
    assert "linqu_ub_udma_demo" not in build_script
    assert "linqu_ub_udma=1" in init_source
    assert "linqu_ub_udma=1" in run_demo
    assert "linqu_ub_udma_demo" not in init_source
    assert "linqu_ub_udma_demo" not in run_demo
    assert "linqu_ub_udma_demo" not in dual_runner
    assert "ub udma (app|demo)" not in dual_runner
    assert (app_dir / "ub_udma.c").exists()
    assert (app_dir / "Makefile").exists()
    assert not (ROOT / "apps" / "ub_udma_demo").exists()


def test_ub_tcp_each_server_uses_canonical_app_source():
    build_script = (ROOT / "scripts" / "build_initramfs.sh").read_text()
    init_source = (ROOT / "init.c").read_text()
    run_demo = (ROOT / "initramfs" / "run_demo").read_text()
    dual_runner = (ROOT / "scripts" / "run_ub_dual_node_apps.sh").read_text()
    app_dir = ROOT / "apps" / "ub_tcp_each_server"

    assert (
        'TCP_EACH_SERVER_SRC="$ROOT_DIR/apps/ub_tcp_each_server/ub_tcp_each_server.c"'
        in build_script
    )
    assert "linqu_ub_tcp_each_server=1" in run_demo
    assert "linqu_ub_tcp_each_server=1" in init_source
    assert "[init] ub tcp each server app pass" in init_source
    assert "run_ub_tcp_each_server_demo_probe" not in init_source
    assert "linqu_ub_tcp_each_server_demo" not in run_demo
    assert "linqu_ub_tcp_each_server_demo" not in init_source
    assert "linqu_ub_tcp_each_server_demo" not in dual_runner
    assert "ub tcp each server demo" not in dual_runner
    assert "ub tcp each server demo" not in init_source
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
    dual_runner = (ROOT / "scripts" / "run_ub_dual_node_obmm_gsva.sh").read_text()
    multi_runner = (ROOT / "scripts" / "run_ub_multi_node_obmm_gsva_matrix.sh").read_text()
    wrapper_runners = "\n".join(
        [
            (ROOT / "scripts" / "run_ub_two_node_gsva_identity_test.sh").read_text(),
            (ROOT / "scripts" / "run_ub_two_node_gsva_arm_mmu_acceptance.sh").read_text(),
            (ROOT / "scripts" / "run_ub_four_node_gsva_identity_test.sh").read_text(),
            (ROOT / "scripts" / "run_ub_four_node_gsva_arm_mmu_acceptance.sh").read_text(),
            (ROOT / "scripts" / "run_ub_eight_node_gsva_identity_test.sh").read_text(),
            (ROOT / "scripts" / "run_ub_eight_node_gsva_arm_mmu_acceptance.sh").read_text(),
        ]
    )
    legacy_dual_runner = (ROOT / "scripts" / "run_ub_dual_node_gsva_demo.sh").read_text()
    legacy_matrix_runner = (ROOT / "scripts" / "run_ub_four_node_gsva_matrix_demo.sh").read_text()
    app_dir = ROOT / "apps" / "obmm_gsva"
    app_source = (app_dir / "obmm_gsva.c").read_text()

    assert 'OBMM_GSVA_SRC="$ROOT_DIR/apps/obmm_gsva/obmm_gsva.c"' in build_script
    assert 'OBMM_GSVA_BIN="$OUT_DIR/linqu_ub_obmm_gsva"' in build_script
    assert "linqu_ub_obmm_gsva_demo" not in build_script
    assert "linqu_obmm_gsva=1" in run_demo
    assert "linqu_obmm_gsva_demo" not in run_demo
    assert "obmm_gsva_demo" not in run_demo
    assert "rdinit=/bin/run_demo obmm_gsva" in dual_runner
    assert "OBMM_GSVA_MODE" in dual_runner
    assert "GSVA_DEMO_" not in dual_runner
    assert "GSVA_DEMO_" not in wrapper_runners
    assert "[obmm-gsva]" in dual_runner
    assert "rdinit=/bin/run_demo obmm_gsva" in multi_runner
    assert "OBMM_GSVA_MATRIX_NODE_COUNT" in multi_runner
    assert "run_ub_dual_node_obmm_gsva.sh" in legacy_dual_runner
    assert "run_ub_multi_node_obmm_gsva_matrix.sh" in legacy_matrix_runner
    assert "enum gsva_app_mode" in app_source
    assert "struct gsva_app_config" in app_source
    assert "GSVA_DEMO" not in app_source
    assert "gsva_demo" not in app_source
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
    assert "linqu_gva_direct_demo" not in run_demo
    assert "gva_direct_demo" not in run_demo
    assert (app_dir / "gva_direct.c").exists()
    assert (app_dir / "Makefile").exists()
    assert not (ROOT / "apps" / "gva_direct_demo").exists()


def test_obmm_queue_uses_canonical_app_source():
    build_script = (ROOT / "scripts" / "build_initramfs.sh").read_text()
    run_demo = (ROOT / "initramfs" / "run_demo").read_text()
    dual_runner = (ROOT / "scripts" / "run_ub_dual_node_obmm_queue.sh").read_text()
    four_runner = (ROOT / "scripts" / "run_ub_four_node_obmm_queue.sh").read_text()
    eight_runner = (ROOT / "scripts" / "run_ub_eight_node_obmm_queue.sh").read_text()
    legacy_four_runner = (ROOT / "scripts" / "run_ub_four_node_obmm_queue_demo.sh").read_text()
    legacy_eight_runner = (ROOT / "scripts" / "run_ub_eight_node_obmm_queue_demo.sh").read_text()
    app_dir = ROOT / "apps" / "obmm_queue"
    app_source = (app_dir / "obmm_queue.c").read_text()

    assert 'OBMM_QUEUE_SRC="$ROOT_DIR/apps/obmm_queue/obmm_queue.c"' in build_script
    assert 'OBMM_QUEUE_BIN="$OUT_DIR/linqu_ub_obmm_queue"' in build_script
    assert "linqu_ub_obmm_queue_demo" not in build_script
    assert "linqu_obmm_queue=1" in run_demo
    assert "linqu_obmm_queue_demo" not in run_demo
    assert "obmm_queue_demo" not in run_demo
    assert "run linqu_ub_obmm_queue" in dual_runner
    assert "OBMM_QUEUE_MODE" in dual_runner
    assert "OBMM_DEMO_MODE" not in dual_runner
    assert "run_queue_app" in four_runner
    assert "[obmm-queue4]" in four_runner
    assert "run_queue_app" in eight_runner
    assert "[obmm-queue8]" in eight_runner
    assert "export OBMM_QUEUE_MODE=" in eight_runner
    assert "OBMM_DEMO_MODE" not in eight_runner
    assert "OBMM_DEMO_MODE" not in run_demo
    assert "run_ub_four_node_obmm_queue.sh" in legacy_four_runner
    assert "run_ub_eight_node_obmm_queue.sh" in legacy_eight_runner
    assert "enum queue_mode" in app_source
    assert "parse_queue_mode" in app_source
    assert "OBMM_QUEUE_MODE" in app_source
    assert "OBMM_DEMO_MODE" not in app_source
    assert "DEMO_MODE_" not in app_source
    assert (app_dir / "obmm_queue.c").exists()
    assert (app_dir / "obmm_pool_helpers.h").exists()
    assert (app_dir / "Makefile").exists()
    assert not (ROOT / "apps" / "obmm_queue_demo").exists()


def test_ub_obmm_pool_uses_canonical_app_source():
    build_script = (ROOT / "scripts" / "build_initramfs.sh").read_text()
    run_demo = (ROOT / "initramfs" / "run_demo").read_text()
    dual_runner = (ROOT / "scripts" / "run_ub_dual_node_obmm_pool.sh").read_text()
    four_runner = (ROOT / "scripts" / "run_ub_four_node_obmm_pool.sh").read_text()
    eight_runner = (ROOT / "scripts" / "run_ub_eight_node_obmm_pool.sh").read_text()
    app_dir = ROOT / "apps" / "ub_obmm_pool"
    app_source = (app_dir / "ub_obmm_pool.c").read_text()

    assert 'OBMM_POOL_SRC="$ROOT_DIR/apps/ub_obmm_pool/ub_obmm_pool.c"' in build_script
    assert 'OBMM_POOL_BIN="$OUT_DIR/linqu_ub_obmm_pool"' in build_script
    assert "linqu_ub_obmm_demo" not in build_script
    assert "linqu_obmm_pool=1" in run_demo
    assert "linqu_obmm_demo" not in run_demo
    assert "obmm|obmm_pool|obmm_demo" not in run_demo
    assert "obmm_demo" not in run_demo
    assert "linqu_obmm_demo=1" not in dual_runner
    assert "run linqu_ub_obmm_pool" in dual_runner
    assert "run_pool_app" in four_runner
    assert "[obmm-pool4]" in four_runner
    assert "run_pool_app" in eight_runner
    assert "[obmm-pool8]" in eight_runner
    assert "struct obmm_pool_meta" in app_source
    assert "obmm_demo_meta" not in app_source
    assert (app_dir / "ub_obmm_pool.c").exists()
    assert (app_dir / "Makefile").exists()
    assert not (ROOT / "apps" / "ub_obmm_pool_demo").exists()


def test_entity_runtime_inject_uses_canonical_cli_entrypoint():
    script = (ROOT / "scripts" / "run_ub_entity_runtime_inject.sh").read_text()
    legacy_script = (ROOT / "scripts" / "run_ub_entity_runtime_inject_demo.sh").read_text()

    assert "entity runtime injection guide" in script
    assert "演示" not in script
    assert "run_ub_entity_runtime_inject.sh" in legacy_script


def test_dual_node_apps_uses_canonical_cli_entrypoint():
    script = (ROOT / "scripts" / "run_ub_dual_node_apps.sh").read_text()
    legacy_script = (ROOT / "scripts" / "run_ub_dual_node_demo.sh").read_text()
    w4_runner = (ROOT / "scripts" / "run_ub_dual_node_w4_guest.sh").read_text()

    assert 'REPORT_FILE="${REPORT_FILE:-$OUT_DIR/apps_report.latest.txt}"' in script
    assert "scenario=dual-node-apps" in script
    assert "dual-node apps validation passed" in script
    assert "ub_nodeA.apps." in script
    assert "--app NAME" in script
    assert "APP_SELECTION" in script
    assert 'flag="linqu_ub_tcp_each_server=1"' in script
    assert "run_ub_dual_node_apps.sh" in legacy_script
    assert "run_ub_dual_node_apps.sh" in w4_runner
    assert "run_ub_dual_node_demo.sh" not in w4_runner
