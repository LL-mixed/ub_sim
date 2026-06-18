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
    run_demo = (ROOT / "initramfs" / "run_app").read_text()
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
    run_demo = (ROOT / "initramfs" / "run_app").read_text()
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
    run_demo = (ROOT / "initramfs" / "run_app").read_text()
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


def test_obmm_dataplane_microbench_has_integration_entrypoints():
    script = (ROOT / "scripts" / "run_ub_dual_node_apps.sh").read_text()
    init_source = (ROOT / "init.c").read_text()

    assert "linqu_obmm_dataplane_microbench=1" in script
    assert "obmm_dataplane_microbench" in script
    assert "should_run_obmm_dataplane_microbench" in init_source
    assert "run_obmm_dataplane_microbench_probe" in init_source


def test_obmm_dataplane_microbench_runner_uses_app_flag_entrypoint():
    runner = (ROOT / "scripts" / "run_ub_dual_node_obmm_dataplane_microbench.sh").read_text()

    assert "rdinit=/bin/run_app linqu_obmm_dataplane_microbench=1" in runner
    assert "rdinit=/bin/run_demo obmm_dataplane_microbench " not in runner


def test_obmm_import_stress_has_independent_app_build():
    build_script = (ROOT / "scripts" / "build_initramfs.sh").read_text()
    app_dir = ROOT / "apps" / "obmm_import_stress"

    assert (
        'OBMM_IMPORT_STRESS_SRC="$ROOT_DIR/apps/obmm_import_stress/obmm_import_stress.c"'
        in build_script
    )
    assert (app_dir / "obmm_import_stress.c").exists()
    assert (app_dir / "Makefile").exists()


def test_obmm_import_stress_has_integration_entrypoints():
    script = (ROOT / "scripts" / "run_ub_dual_node_apps.sh").read_text()
    init_source = (ROOT / "init.c").read_text()

    assert "linqu_obmm_import_stress=1" in script
    assert "obmm_import_stress" in script
    assert "should_run_obmm_import_stress" in init_source
    assert "run_obmm_import_stress_probe" in init_source


def test_obmm_import_stress_runner_uses_app_flag_entrypoint():
    runner = (ROOT / "scripts" / "run_ub_dual_node_obmm_import_stress.sh").read_text()

    assert "rdinit=/bin/run_app linqu_obmm_import_stress=1" in runner
    assert "rdinit=/bin/run_demo obmm_import_stress " not in runner


def test_obmm_coh_test_has_independent_dual_node_bootflow():
    script = (ROOT / "scripts" / "run_ub_dual_node_apps.sh").read_text()
    init_source = (ROOT / "init.c").read_text()

    assert "obmm_coh_test" in script
    assert "linqu_obmm_coh_test=1" in script
    assert "COH_TEST_MODE" in script
    assert "COH_TEST_ITERS" in script
    assert "should_run_obmm_coh_test" in init_source
    assert "run_obmm_coh_test_probe" in init_source
    assert "nodea_obmm_coh_test_append" in script


def test_obmm_coh_test_runner_uses_app_flag_entrypoint():
    runner = (ROOT / "scripts" / "run_ub_dual_node_obmm_coh_test.sh").read_text()

    assert "rdinit=/bin/run_app linqu_obmm_coh_test=1" in runner
    assert "rdinit=/bin/run_demo obmm_coh_test " not in runner


def test_npu_gsva_test_has_independent_app_build():
    build_script = (ROOT / "scripts" / "build_initramfs.sh").read_text()
    app_dir = ROOT / "apps" / "npu_gsva_test"

    assert 'NPU_GSVA_TEST_SRC="$ROOT_DIR/apps/npu_gsva_test/npu_gsva_test.c"' in build_script
    assert (app_dir / "npu_gsva_test.c").exists()
    assert (app_dir / "Makefile").exists()


def test_gsva_query_has_independent_app_build():
    build_script = (ROOT / "scripts" / "build_initramfs.sh").read_text()
    app_dir = ROOT / "apps" / "gsva_query"

    assert 'GSVA_QUERY_SRC="$ROOT_DIR/apps/gsva_query/gsva_query.c"' in build_script
    assert (app_dir / "gsva_query.c").exists()
    assert (app_dir / "Makefile").exists()


def test_npu_test_has_independent_app_build():
    build_script = (ROOT / "scripts" / "build_initramfs.sh").read_text()
    app_dir = ROOT / "apps" / "npu_test"

    assert 'NPU_TEST_SRC="$ROOT_DIR/apps/npu_test/npu_test.c"' in build_script
    assert 'NPU_TEST_BIN="$OUT_DIR/npu_test"' in build_script
    assert (app_dir / "npu_test.c").exists()
    assert (app_dir / "Makefile").exists()


def test_ssd_gsva_test_has_independent_app_build():
    build_script = (ROOT / "scripts" / "build_initramfs.sh").read_text()
    app_dir = ROOT / "apps" / "ssd_gsva_test"

    assert 'SSD_GSVA_TEST_SRC="$ROOT_DIR/apps/ssd_gsva_test/ssd_gsva_test.c"' in build_script
    assert (app_dir / "ssd_gsva_test.c").exists()
    assert (app_dir / "Makefile").exists()


def test_w5_mem_service_is_link_time_component():
    build_script = (ROOT / "scripts" / "build_initramfs.sh").read_text()
    component_dir = ROOT / "components" / "w5_mem_service"
    readme = (component_dir / "README.md").read_text()

    assert 'W4_DB_SERVICE_SRC="$ROOT_DIR/components/w5_mem_service/w4_kvcache_db_service.c"' in build_script
    assert '"$W4_GUEST_SRC" "$W4_DB_SERVICE_SRC" -lm -o "$W4_GUEST_BIN"' in build_script
    assert "not a standalone app" in readme
    assert "standalone demo" not in readme
    assert "test_w4_db_record_recycling.py" in readme
    assert (component_dir / "w4_kvcache_db_service.c").exists()
    assert (component_dir / "w4_kvcache_db_service.h").exists()
    assert (component_dir / "w4_lingqu_object_service.h").exists()


def test_obmm_gsva_uses_canonical_app_source():
    build_script = (ROOT / "scripts" / "build_initramfs.sh").read_text()
    run_demo = (ROOT / "initramfs" / "run_app").read_text()
    dual_runner = (ROOT / "scripts" / "run_ub_dual_node_obmm_gsva.sh").read_text()
    dual_apps_runner = (ROOT / "scripts" / "run_ub_dual_node_apps.sh").read_text()
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
    app_dir = ROOT / "apps" / "obmm_gsva"
    app_source = (app_dir / "obmm_gsva.c").read_text()

    assert 'OBMM_GSVA_SRC="$ROOT_DIR/apps/obmm_gsva/obmm_gsva.c"' in build_script
    assert 'OBMM_GSVA_BIN="$OUT_DIR/linqu_ub_obmm_gsva"' in build_script
    assert "linqu_ub_obmm_gsva_demo" not in build_script
    assert "linqu_obmm_gsva=1" in run_demo
    assert "linqu_obmm_gsva_demo" not in run_demo
    assert "obmm_gsva_demo" not in run_demo
    assert "linqu_obmm_gsva=1" in dual_apps_runner
    assert "obmm_gsva" in dual_apps_runner
    assert "rdinit=/bin/run_app linqu_obmm_gsva=1" in dual_runner
    assert "OBMM_GSVA_MODE" in dual_runner
    assert "rdinit=/bin/run_demo obmm_gsva " not in dual_runner
    assert "GSVA_DEMO_" not in dual_runner
    assert "GSVA_DEMO_" not in wrapper_runners
    assert "[obmm-gsva]" in dual_runner
    assert "rdinit=/bin/run_demo linqu_obmm_gsva=1" in multi_runner
    assert "rdinit=/bin/run_demo obmm_gsva " not in multi_runner
    assert "OBMM_GSVA_MATRIX_NODE_COUNT" in multi_runner
    assert "enum gsva_app_mode" in app_source
    assert "struct gsva_app_config" in app_source
    assert "GSVA_DEMO" not in app_source
    assert "gsva_demo" not in app_source
    assert (app_dir / "obmm_gsva.c").exists()
    assert (app_dir / "Makefile").exists()
    assert not (ROOT / "apps" / "obmm_gsva_demo").exists()
    assert not (ROOT / "scripts" / "run_ub_dual_node_gsva_demo.sh").exists()
    assert not (ROOT / "scripts" / "run_ub_four_node_gsva_matrix_demo.sh").exists()


def test_obmm_gsva_has_independent_dual_node_bootflow():
    script = (ROOT / "scripts" / "run_ub_dual_node_apps.sh").read_text()
    init_source = (ROOT / "init.c").read_text()

    assert "linqu_obmm_gsva=1" in script
    assert "obmm_gsva_mode=${OBMM_GSVA_MODE}" in script
    assert "obmm_gsva_node_count=${OBMM_GSVA_NODE_COUNT}" in script
    assert "should_run_obmm_gsva" in init_source
    assert "run_obmm_gsva_probe" in init_source
    assert "append_cmdline_if_missing \"obmm_gsva_mode=${OBMM_GSVA_MODE}\"" in script


def test_gva_direct_has_independent_dual_node_bootflow():
    script = (ROOT / "scripts" / "run_ub_dual_node_apps.sh").read_text()
    init_source = (ROOT / "init.c").read_text()

    assert "linqu_gva_direct=1" in script
    assert "gva_direct_mode=${GVA_DIRECT_MODE}" in script
    assert "gva_direct_size=${GVA_DIRECT_SIZE}" in script
    assert "gva_direct_local_va=${GVA_DIRECT_LOCAL_VA}" in script
    assert "gva_direct_home_va=${GVA_DIRECT_HOME_VA}" in script
    assert "should_run_gva_direct" in init_source
    assert "run_gva_direct_probe" in init_source
    assert "gva_direct_enabled" in script
    assert "validate_gva_direct_log" in script


def test_gsva_query_has_independent_dual_node_bootflow():
    script = (ROOT / "scripts" / "run_ub_dual_node_apps.sh").read_text()
    init_source = (ROOT / "init.c").read_text()

    assert "linqu_gsva_query=1" in script
    assert "should_run_gsva_query" in init_source
    assert "run_gsva_query_probe" in init_source
    assert "gsva_query_enabled" in script
    assert "validate_gsva_query_log" in script


def test_npu_test_has_independent_dual_node_bootflow():
    script = (ROOT / "scripts" / "run_ub_dual_node_apps.sh").read_text()
    init_source = (ROOT / "init.c").read_text()

    assert "linqu_npu_test=1" in script
    assert "should_run_npu_test" in init_source
    assert "run_npu_test_probe" in init_source
    assert "npu_test_enabled" in script
    assert "validate_npu_test_log" in script


def test_gsva_query_runner_uses_app_flag_entrypoint():
    runner = (ROOT / "scripts" / "run_ub_gsva_query_caps_test.sh").read_text()

    assert "rdinit=/bin/run_app linqu_gsva_query=1" in runner
    assert "rdinit=/bin/run_demo gsva_query " not in runner


def test_gsva_query_uses_canonical_app_source():
    run_demo = (ROOT / "initramfs" / "run_app").read_text()

    assert "linqu_gsva_query=1" in run_demo
    assert "run_gsva_query" in run_demo
    assert "gsva_query_demo" not in run_demo


def test_gsva_coh_and_lifecycle_runner_uses_app_flag_entrypoint():
    two_node_coh_runner = (ROOT / "scripts" / "run_ub_two_node_gsva_coh_test.sh").read_text()
    two_node_lifecycle_runner = (ROOT / "scripts" / "run_ub_two_node_gsva_lifecycle_test.sh").read_text()
    runners = {
        (ROOT / "scripts" / "run_ub_four_node_gsva_coh_test.sh").read_text():
            "linqu_gsva_coh_test=1",
        (ROOT / "scripts" / "run_ub_eight_node_gsva_coh_test.sh").read_text():
            "linqu_gsva_coh_test=1",
        (ROOT / "scripts" / "run_ub_eight_node_gsva_lifecycle_test.sh").read_text():
            "linqu_gsva_lifecycle_test=1",
    }
    four_node_lifecycle_runner = (
        ROOT / "scripts" / "run_ub_four_node_gsva_lifecycle_test.sh"
    ).read_text()

    assert "rdinit=/bin/run_app linqu_gsva_coh_test=1" in two_node_coh_runner
    assert "rdinit=/bin/run_demo gsva_coh_test " not in two_node_coh_runner
    assert "rdinit=/bin/run_app linqu_gsva_lifecycle_test=1" in two_node_lifecycle_runner
    assert "rdinit=/bin/run_demo gsva_lifecycle_test " not in two_node_lifecycle_runner
    assert "rdinit=/bin/run_app linqu_gsva_lifecycle_test=1" in four_node_lifecycle_runner
    assert "rdinit=/bin/run_demo gsva_lifecycle_test " not in four_node_lifecycle_runner

    for runner, token in runners.items():
        assert f"rdinit=/bin/run_demo {token}" in runner


def test_npu_ssd_gsva_runner_uses_app_flag_entrypoint():
    two_node_npu_runner = (ROOT / "scripts" / "run_ub_two_node_npu_test.sh").read_text()
    two_node_npu_gsva_runner = (ROOT / "scripts" / "run_ub_two_node_npu_gsva_test.sh").read_text()
    two_node_ssd_runner = (ROOT / "scripts" / "run_ub_two_node_ssd_test.sh").read_text()
    two_node_ssd_gsva_runner = (ROOT / "scripts" / "run_ub_two_node_ssd_gsva_test.sh").read_text()

    assert "rdinit=/bin/run_app linqu_npu_test=1" in two_node_npu_runner
    assert "rdinit=/bin/run_demo npu_test " not in two_node_npu_runner
    assert "rdinit=/bin/run_app linqu_npu_gsva_test=1" in two_node_npu_gsva_runner
    assert "rdinit=/bin/run_demo npu_gsva_test " not in two_node_npu_gsva_runner
    assert "rdinit=/bin/run_app linqu_ssd_test=1" in two_node_ssd_runner
    assert "rdinit=/bin/run_demo ssd_test " not in two_node_ssd_runner
    assert "rdinit=/bin/run_app linqu_ssd_gsva_test=1" in two_node_ssd_gsva_runner
    assert "rdinit=/bin/run_demo ssd_gsva_test " not in two_node_ssd_gsva_runner

    for path, token in {
        ROOT / "scripts" / "run_ub_four_node_npu_gsva_test.sh": "linqu_npu_gsva_test=1",
        ROOT / "scripts" / "run_ub_eight_node_npu_gsva_test.sh": "linqu_npu_gsva_test=1",
        ROOT / "scripts" / "run_ub_four_node_ssd_gsva_test.sh": "linqu_ssd_gsva_test=1",
        ROOT / "scripts" / "run_ub_eight_node_ssd_gsva_test.sh": "linqu_ssd_gsva_test=1",
    }.items():
        runner = path.read_text()
        assert f"rdinit=/bin/run_demo {token}" in runner


def test_gva_direct_uses_canonical_app_source():
    build_script = (ROOT / "scripts" / "build_initramfs.sh").read_text()
    run_demo = (ROOT / "initramfs" / "run_app").read_text()
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


def test_gva_direct_runner_uses_app_flag_entrypoint():
    runner = (ROOT / "scripts" / "run_ub_dual_node_gva_direct_test.sh").read_text()
    assert "rdinit=/bin/run_app linqu_gva_direct=1" in runner
    assert "linqu_gva_direct=1" in runner
    assert "rdinit=/bin/run_demo gva_direct " not in runner


def test_gva_manager_bootstrap_runner_uses_unified_app_entrypoint():
    run_demo = (ROOT / "initramfs" / "run_app").read_text()
    dual_runner = (ROOT / "scripts" / "run_ub_dual_node_gsva_manager_bootstrap.sh").read_text()
    four_runner = (ROOT / "scripts" / "run_ub_four_node_gsva_manager_bootstrap.sh").read_text()

    assert "linqu_gva_manager=1" in dual_runner
    assert "gva_manager_mode=bootstrap" in dual_runner
    assert "rdinit=/bin/run_app linqu_gva_manager=1" in dual_runner
    assert "rdinit=/bin/run_demo gva_manager " not in dual_runner
    assert "gva_manager_bootstrap" not in dual_runner
    assert "gva_manager=" in dual_runner
    assert "run_gva_manager" in run_demo
    assert "gva_manager_bootstrap)" not in run_demo
    assert "gva_manager_dump_routes)" not in run_demo
    assert "gva_manager_segment_cli)" not in run_demo

    assert "linqu_gva_manager=1" in four_runner
    assert "gva_manager_mode=bootstrap" in four_runner
    assert "gva_manager_bootstrap" not in four_runner


def test_gva_manager_segment_cli_runner_uses_unified_app_entrypoint():
    run_demo = (ROOT / "initramfs" / "run_app").read_text()
    segment_cli_runner = (ROOT / "scripts" / "run_ub_two_node_gva_manager_segment_cli_test.sh").read_text()

    assert "linqu_gva_manager=1" in segment_cli_runner
    assert "gva_manager_mode=segment_cli" in segment_cli_runner
    assert "rdinit=/bin/run_app linqu_gva_manager=1" in segment_cli_runner
    assert "rdinit=/bin/run_demo gva_manager " not in segment_cli_runner
    assert "result=done action=gsva-segment-alloc" in segment_cli_runner
    assert "result=done action=gsva-segment-query" in segment_cli_runner
    assert "result=done action=gsva-segment-retire" in segment_cli_runner
    assert "gva_manager_segment_cli" not in segment_cli_runner
    assert "run_gva_manager_segment_cli" in run_demo
    assert "run linqu_gva_manager segment_cli" in run_demo
    assert "linqu_gva_manager segment_cli done" in run_demo


def test_obmm_queue_uses_canonical_app_source():
    build_script = (ROOT / "scripts" / "build_initramfs.sh").read_text()
    run_demo = (ROOT / "initramfs" / "run_app").read_text()
    dual_runner = (ROOT / "scripts" / "run_ub_dual_node_obmm_queue.sh").read_text()
    four_runner = (ROOT / "scripts" / "run_ub_four_node_obmm_queue.sh").read_text()
    eight_runner = (ROOT / "scripts" / "run_ub_eight_node_obmm_queue.sh").read_text()
    app_dir = ROOT / "apps" / "obmm_queue"
    app_source = (app_dir / "obmm_queue.c").read_text()

    assert 'OBMM_QUEUE_SRC="$ROOT_DIR/apps/obmm_queue/obmm_queue.c"' in build_script
    assert 'OBMM_QUEUE_BIN="$OUT_DIR/linqu_ub_obmm_queue"' in build_script
    assert "linqu_ub_obmm_queue_demo" not in build_script
    assert "linqu_obmm_queue=1" in run_demo
    assert "linqu_obmm_queue_demo" not in run_demo
    assert "obmm_queue_demo" not in run_demo
    assert "rdinit=/bin/run_app linqu_obmm_queue=1" in dual_runner
    assert "rdinit=/bin/run_demo obmm_queue " not in dual_runner
    assert "OBMM_QUEUE_MODE" in dual_runner
    assert "OBMM_DEMO_MODE" not in dual_runner
    assert "run_queue_app" in four_runner
    assert "[obmm-queue4]" in four_runner
    assert "run_queue_app" in eight_runner
    assert "[obmm-queue8]" in eight_runner
    assert "export OBMM_QUEUE_MODE=" in eight_runner
    assert "OBMM_DEMO_MODE" not in eight_runner
    assert "OBMM_DEMO_MODE" not in run_demo
    assert "enum queue_mode" in app_source
    assert "parse_queue_mode" in app_source
    assert "OBMM_QUEUE_MODE" in app_source
    assert "OBMM_DEMO_MODE" not in app_source
    assert "DEMO_MODE_" not in app_source
    assert (app_dir / "obmm_queue.c").exists()
    assert (app_dir / "obmm_pool_helpers.h").exists()
    assert (app_dir / "Makefile").exists()
    assert not (ROOT / "apps" / "obmm_queue_demo").exists()
    assert not (ROOT / "scripts" / "run_ub_four_node_obmm_queue_demo.sh").exists()
    assert not (ROOT / "scripts" / "run_ub_eight_node_obmm_queue_demo.sh").exists()


def test_ub_obmm_pool_uses_canonical_app_source():
    build_script = (ROOT / "scripts" / "build_initramfs.sh").read_text()
    init_source = (ROOT / "init.c").read_text()
    run_demo = (ROOT / "initramfs" / "run_app").read_text()
    dual_runner = (ROOT / "scripts" / "run_ub_dual_node_obmm_pool.sh").read_text()
    four_runner = (ROOT / "scripts" / "run_ub_four_node_obmm_pool.sh").read_text()
    eight_runner = (ROOT / "scripts" / "run_ub_eight_node_obmm_pool.sh").read_text()
    app_dir = ROOT / "apps" / "ub_obmm_pool"
    app_source = (app_dir / "ub_obmm_pool.c").read_text()

    assert 'OBMM_POOL_SRC="$ROOT_DIR/apps/ub_obmm_pool/ub_obmm_pool.c"' in build_script
    assert 'OBMM_POOL_BIN="$OUT_DIR/linqu_ub_obmm_pool"' in build_script
    assert "linqu_ub_obmm_demo" not in build_script
    assert "linqu_obmm_pool=1" in init_source
    assert "linqu_obmm_pool=1" in run_demo
    assert "linqu_obmm_demo" not in init_source
    assert "linqu_obmm_demo" not in run_demo
    assert "obmm|obmm_pool|obmm_demo" not in run_demo
    assert "obmm_demo" not in run_demo
    assert "linqu_obmm_demo=1" not in dual_runner
    assert "rdinit=/bin/run_app linqu_obmm_pool=1" in dual_runner
    assert "rdinit=/bin/run_demo obmm_pool " not in dual_runner
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

    assert "entity runtime injection guide" in script
    assert "演示" not in script
    assert not (ROOT / "scripts" / "run_ub_entity_runtime_inject_demo.sh").exists()


def test_dual_node_apps_uses_canonical_cli_entrypoint():
    script = (ROOT / "scripts" / "run_ub_dual_node_apps.sh").read_text()
    init_source = (ROOT / "init.c").read_text()
    run_demo = (ROOT / "initramfs" / "run_demo").read_text()
    run_app = (ROOT / "initramfs" / "run_app").read_text()
    build_script = (ROOT / "scripts" / "build_initramfs.sh").read_text()
    w4_runner = (ROOT / "scripts" / "run_ub_dual_node_w4_guest.sh").read_text()
    w4_eight_runner = (ROOT / "scripts" / "run_ub_eight_node_w4_guest.sh").read_text()

    assert 'REPORT_FILE="${REPORT_FILE:-$OUT_DIR/apps_report.latest.txt}"' in script
    assert "scenario=dual-node-apps" in script
    assert "obmm_dataplane_microbench" in script
    assert "dual-node apps validation passed" in script
    assert "ub_nodeA.apps." in script
    assert "--app NAME" in script
    assert "APP_SELECTION" in script
    assert 'RDINIT="${RDINIT:-/bin/run_app}"' in script
    assert 'flag="linqu_ub_tcp_each_server=1"' in script
    assert 'RUN_APP_SRC="$ROOT_DIR/initramfs/run_app"' in build_script
    assert 'RUN_APP_BIN="$INITRAMFS_DIR/bin/run_app"' in build_script
    assert "write_signature_line \"run_app_src\"" in build_script
    assert "run_app|run_demo)" in (ROOT / "initramfs" / "init").read_text()
    assert "exec /bin/run_app" in run_demo
    assert "[run_app]" in run_app
    assert "UB_RUN_APP_FROM_INIT" in init_source
    assert "UB_RUN_APP_FROM_INIT" in run_app
    assert "UB_RUN_APP_FROM_INIT" in w4_eight_runner
    assert "boot flow completed, dropping to shell" not in run_app
    assert "run_default_actions" in run_app
    assert "linqu_obmm_dataplane_microbench=1" in run_app
    assert "linqu_obmm_import_stress=1" in run_app
    assert "linqu_obmm_coh_test=1" in run_app
    assert "linqu_gsva_coh_test=1" in run_app
    assert "linqu_gsva_lifecycle_test=1" in run_app
    assert "linqu_npu_gsva_test=1" in run_app
    assert "linqu_ssd_gsva_test=1" in run_app
    assert "switching to /bin/run_app app boot flow" in init_source
    assert "execv(\"/bin/run_app\"" in init_source
    assert "bool defer_app_boot_flow = should_enter_app_boot_flow()" in init_source
    assert "!defer_app_boot_flow && should_run_obmm_gsva()" in init_source
    assert "!defer_app_boot_flow && should_run_gva_direct()" in init_source
    assert "!defer_app_boot_flow && should_run_obmm_queue()" in init_source
    assert "UB_RUN_DEMO_FROM_INIT" not in init_source
    assert "UB_RUN_DEMO_FROM_INIT" not in run_demo
    assert "UB_RUN_DEMO_FROM_INIT" not in w4_eight_runner
    assert "should_enter_app_boot_flow" in init_source
    assert "should_enter_demo_boot_flow" not in init_source
    assert "no demo flags matched" not in run_app
    assert "run_ub_dual_node_apps.sh" in w4_runner
    assert "linqu_w4_demo" not in w4_runner
    assert "run_ub_dual_node_demo.sh" not in w4_runner
    assert not (ROOT / "scripts" / "run_ub_dual_node_demo.sh").exists()
