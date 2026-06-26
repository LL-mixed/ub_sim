import re
import subprocess
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


APP_VALIDATION_COMMANDS = {
    "ub_chat": [
        "scripts/run_ub_dual_node_chat.sh",
        "scripts/run_ub_eight_node_chat_matrix.sh",
    ],
    "ub_rpc": [
        "scripts/run_ub_dual_node_rpc.sh",
        "scripts/run_ub_eight_node_rpc_matrix.sh",
    ],
    "ub_tcp_each_server": [
        "scripts/run_ub_dual_node_tcp_each_server.sh",
        "scripts/run_ub_eight_node_tcp_each_server_matrix.sh",
    ],
    "ub_udma": [
        "scripts/run_ub_dual_node_udma.sh",
        "scripts/run_ub_eight_node_udma_matrix.sh",
    ],
    "ub_obmm_pool": [
        "scripts/run_ub_dual_node_obmm_pool.sh",
        "scripts/run_ub_eight_node_obmm_pool.sh",
    ],
    "obmm_queue": [
        "scripts/run_ub_dual_node_obmm_queue.sh",
        "scripts/run_ub_eight_node_obmm_queue.sh",
    ],
    "obmm_dataplane_microbench": [
        "scripts/run_ub_dual_node_obmm_dataplane_microbench_matrix.sh",
        "scripts/run_ub_eight_node_obmm_dataplane_microbench.sh",
    ],
    "obmm_import_stress": [
        "scripts/run_ub_dual_node_obmm_import_stress.sh",
        "scripts/run_ub_eight_node_obmm_import_stress.sh",
    ],
    "obmm_gsva": [
        "scripts/run_ub_dual_node_obmm_gsva.sh",
        "scripts/run_ub_eight_node_obmm_gsva_matrix.sh",
    ],
    "obmm_coh_test": [
        "scripts/run_ub_dual_node_obmm_coh_test.sh",
        "scripts/run_ub_eight_node_obmm_coh_test.sh",
    ],
    "gva_direct": [
        "scripts/run_ub_dual_node_gva_direct_test.sh",
        "scripts/run_ub_eight_node_gva_direct_test.sh",
    ],
    "gva_manager": [
        "scripts/run_ub_dual_node_gsva_manager_bootstrap.sh",
        "scripts/run_ub_eight_node_gsva_manager_bootstrap.sh",
    ],
    "gsva_query": [
        "scripts/run_ub_dual_node_gsva_query.sh",
        "scripts/run_ub_eight_node_gsva_query_caps.sh",
    ],
    "gsva_coh_test": [
        "scripts/run_ub_two_node_gsva_coh_test.sh",
        "scripts/run_ub_eight_node_gsva_coh_test.sh",
    ],
    "gsva_lifecycle_test": [
        "scripts/run_ub_two_node_gsva_lifecycle_test.sh",
        "scripts/run_ub_eight_node_gsva_lifecycle_test.sh",
    ],
    "npu_test": [
        "scripts/run_ub_two_node_npu_test.sh",
        "scripts/run_ub_eight_node_npu_test.sh",
    ],
    "npu_gsva_test": [
        "scripts/run_ub_two_node_npu_gsva_test.sh",
        "scripts/run_ub_eight_node_npu_gsva_test.sh",
    ],
    "ssd_test": [
        "scripts/run_ub_two_node_ssd_test.sh",
        "scripts/run_ub_eight_node_ssd_test.sh",
    ],
    "ssd_gsva_test": [
        "scripts/run_ub_two_node_ssd_gsva_test.sh",
        "scripts/run_ub_eight_node_ssd_gsva_test.sh",
    ],
    "mem_service": [
        "scripts/run_ub_dual_node_mem_service.sh",
        "scripts/run_ub_eight_node_mem_service.sh",
    ],
    "llm_infer": [
        "scripts/run_ub_dual_node_w4_guest.sh",
        "scripts/run_ub_eight_node_w4_guest_qwen3_0_6b_2step.sh",
    ],
}


def test_apps_readme_lists_reusable_validation_command_for_each_app():
    readme = (ROOT / "apps" / "README.md").read_text()
    app_dirs = sorted(path.name for path in (ROOT / "apps").iterdir() if path.is_dir())

    assert app_dirs == sorted(APP_VALIDATION_COMMANDS)
    assert not any("w4" in app or "w5" in app for app in app_dirs)
    assert "/bin/run_demo" not in readme
    assert "DEMO_" not in readme
    assert "scripts/run_ub_app_build_matrix.sh" in readme
    assert "scripts/run_ub_app_validation_matrix.sh" in readme
    assert "scripts/run_w5_cluster_config.sh" in readme
    assert "components/mem_service" in readme
    for app, commands in APP_VALIDATION_COMMANDS.items():
        assert f"`{app}`" in readme
        assert (ROOT / "apps" / app / "Makefile").exists()
        for command in commands:
            script = command.split()[0]
            assert command in readme
            assert (ROOT / script).exists()


def test_app_validation_matrix_runner_matches_readme_commands():
    runner = (ROOT / "scripts" / "run_ub_app_validation_matrix.sh").read_text()

    assert "W5_ENTRY=\"w5_inference_cluster|scripts/run_w5_cluster_qwen3_0_6b_2step.sh\"" in runner
    assert "--scope 2-node|8-node|all|w5|all-with-w5" in runner
    assert "--dry-run" in runner
    assert "--from APP" in runner
    assert "--resume" in runner
    assert "--reset-status" in runner
    assert "--status-file PATH" in runner
    assert "STATUS_FILE=" in runner
    assert "RESET_STATUS=0" in runner
    assert 'RESET_STATUS" == "1"' in runner
    assert 'RESUME" != "1"' not in runner
    assert "status_has_pass" in runner
    assert "local status=" not in runner
    assert "status_value" in runner
    assert "/bin/run_demo" not in runner
    for app, commands in APP_VALIDATION_COMMANDS.items():
        assert f"\"{app}|{commands[0]}|{commands[1]}\"" in runner


def test_app_build_matrix_runner_matches_app_inventory():
    runner_path = ROOT / "scripts" / "run_ub_app_build_matrix.sh"
    runner = runner_path.read_text()

    assert runner_path.exists()
    assert runner_path.stat().st_mode & 0o111
    assert "--dry-run" in runner
    assert "--from APP" in runner
    assert "--only APP" in runner
    assert "--continue-on-fail" in runner
    assert "--keep-artifacts" in runner
    assert "make -C \"$app_dir\"" in runner
    assert "make -C \"$app_dir\" clean" in runner
    for app in APP_VALIDATION_COMMANDS:
        assert f"  {app}\n" in runner


def test_app_validation_matrix_runner_dry_run_executes_without_qemu():
    runner = ROOT / "scripts" / "run_ub_app_validation_matrix.sh"

    list_result = subprocess.run(
        [str(runner), "--list"],
        cwd=ROOT,
        check=True,
        text=True,
        capture_output=True,
    )
    dry_run_result = subprocess.run(
        [str(runner), "--scope", "all", "--dry-run", "--only", "ub_chat"],
        cwd=ROOT,
        check=True,
        text=True,
        capture_output=True,
    )
    w5_result = subprocess.run(
        [str(runner), "--scope", "w5", "--dry-run"],
        cwd=ROOT,
        check=True,
        text=True,
        capture_output=True,
    )

    assert "ub_chat 2-node=scripts/run_ub_dual_node_chat.sh" in list_result.stdout
    assert "w5_inference_cluster 8-node=scripts/run_w5_cluster_qwen3_0_6b_2step.sh" in list_result.stdout
    assert "cmd=scripts/run_ub_dual_node_chat.sh" in dry_run_result.stdout
    assert "cmd=scripts/run_ub_eight_node_chat_matrix.sh" in dry_run_result.stdout
    assert "cmd=scripts/run_w5_cluster_qwen3_0_6b_2step.sh" in w5_result.stdout
    assert "QEMU" not in dry_run_result.stderr

    with tempfile.TemporaryDirectory() as tmpdir:
        status_file = Path(tmpdir) / "app_matrix.status"
        status_file.write_text(
            "ub_chat|2-node|PASS|0|scripts/run_ub_dual_node_chat.sh\n"
            "ub_chat|8-node|PASS|0|scripts/run_ub_eight_node_chat_matrix.sh\n"
        )
        resume_result = subprocess.run(
            [
                str(runner),
                "--scope",
                "all",
                "--dry-run",
                "--only",
                "ub_chat",
                "--resume",
                "--status-file",
                str(status_file),
            ],
            cwd=ROOT,
            check=True,
            text=True,
            capture_output=True,
        )

    assert "SKIP app=ub_chat scope=2-node status=PASS" in resume_result.stdout
    assert "SKIP app=ub_chat scope=8-node status=PASS" in resume_result.stdout
    assert "RUN app=ub_chat" not in resume_result.stdout


def test_tcp_each_server_matrix_supports_dataplane_benchmark_mode():
    runner = (ROOT / "scripts" / "run_ub_eight_node_tcp_each_server_matrix.sh").read_text()
    app_source = (ROOT / "apps" / "ub_tcp_each_server" / "ub_tcp_each_server.c").read_text()
    report_script = ROOT / "scripts" / "transport_perf_report.py"

    assert "TCP_BENCHMARK=" in runner
    assert "TCP_BENCH_SIZE=" in runner
    assert "TCP_BENCH_ITERATIONS=" in runner
    assert "TCP_BENCH_ONE_WAY=" in runner
    assert "TCP_BENCH_PROGRESS_INTERVAL=" in runner
    assert "benchmark_result=done" in runner
    assert "benchmark_result=done" in app_source
    assert "benchmark_server_accepted" in app_source
    assert "benchmark_client_progress" in app_source
    assert "benchmark_server_progress" in app_source
    assert "tcp_bench_one_way_client_role" in app_source
    assert "run_benchmark_client" in app_source
    assert "run_benchmark_server_child" in app_source
    assert report_script.exists()
    assert report_script.stat().st_mode & 0o111


def test_app_build_matrix_runner_dry_run_executes_without_building():
    runner = ROOT / "scripts" / "run_ub_app_build_matrix.sh"

    list_result = subprocess.run(
        [str(runner), "--list"],
        cwd=ROOT,
        check=True,
        text=True,
        capture_output=True,
    )
    dry_run_result = subprocess.run(
        [str(runner), "--dry-run", "--only", "ub_chat"],
        cwd=ROOT,
        check=True,
        text=True,
        capture_output=True,
    )

    assert "ub_chat makefile=apps/ub_chat/Makefile" in list_result.stdout
    assert "llm_infer makefile=apps/llm_infer/Makefile" in list_result.stdout
    assert "RUN app=ub_chat cmd=make -C apps/ub_chat" in dry_run_result.stdout
    assert "RUN app=ub_chat cmd=make -C apps/ub_chat clean" in dry_run_result.stdout
    assert "PASS" in dry_run_result.stdout
    assert not (ROOT / "apps" / "ub_chat" / "ub_chat").exists()


def test_primary_dual_node_app_wrappers_are_stable_cli_entrypoints():
    wrappers = {
        "run_ub_dual_node_chat.sh": "chat",
        "run_ub_dual_node_rpc.sh": "rpc",
        "run_ub_dual_node_tcp_each_server.sh": "tcp_each_server",
        "run_ub_dual_node_udma.sh": "udma",
    }

    for runner_name, app_name in wrappers.items():
        runner = (ROOT / "scripts" / runner_name).read_text()

        assert 'exec "$SCRIPT_DIR/run_ub_dual_node_apps.sh"' in runner
        assert f"--app {app_name}" in runner
        assert ' "$@"' in runner
        assert "/bin/run_demo" not in runner


def test_ub_chat_is_packaged_from_app_directory():
    build_script = (ROOT / "scripts" / "build_initramfs.sh").read_text()
    dual_runner = (ROOT / "scripts" / "run_ub_dual_node_apps.sh").read_text()

    assert 'CHAT_SRC="$ROOT_DIR/apps/ub_chat/ub_chat.c"' in build_script
    assert "\\\\[ub_chat\\\\] pass" in dual_runner
    assert "\\\\[ub_chat\\\\] fail" in dual_runner
    assert 'USE_QMP="${USE_QMP:-0}"' in dual_runner
    assert "--use-qmp" in dual_runner
    assert "qemu_control_args=(-S -qmp" in dual_runner
    assert "Operation not permitted" not in dual_runner
    assert "\\[init\\] ub chat pass" not in dual_runner
    assert not (ROOT / "ub_chat.c").exists()
    assert (ROOT / "apps" / "ub_chat" / "ub_chat.c").exists()
    assert (ROOT / "apps" / "ub_chat" / "Makefile").exists()


def test_ub_rpc_uses_canonical_app_source():
    build_script = (ROOT / "scripts" / "build_initramfs.sh").read_text()
    init_source = (ROOT / "init.c").read_text()
    run_app = (ROOT / "initramfs" / "run_app").read_text()
    dual_runner = (ROOT / "scripts" / "run_ub_dual_node_apps.sh").read_text()
    app_dir = ROOT / "apps" / "ub_rpc"

    assert 'RPC_SRC="$ROOT_DIR/apps/ub_rpc/ub_rpc.c"' in build_script
    assert "linqu_ub_rpc=1" in init_source
    assert "linqu_ub_rpc=1" in run_app
    assert "\\\\[ub_rpc\\\\] pass" in dual_runner
    assert "\\\\[ub_rpc\\\\] fail" in dual_runner
    assert "\\[init\\] ub rpc app pass" not in dual_runner
    assert "linqu_ub_rpc_demo" not in init_source
    assert "linqu_ub_rpc_demo" not in run_app
    assert "linqu_ub_rpc_demo" not in dual_runner
    assert "ub rpc (app|demo)" not in dual_runner
    assert (app_dir / "ub_rpc.c").exists()
    assert (app_dir / "Makefile").exists()
    assert not (ROOT / "apps" / "ub_rpc_demo").exists()


def test_ub_udma_uses_canonical_app_source():
    build_script = (ROOT / "scripts" / "build_initramfs.sh").read_text()
    init_source = (ROOT / "init.c").read_text()
    run_app = (ROOT / "initramfs" / "run_app").read_text()
    dual_runner = (ROOT / "scripts" / "run_ub_dual_node_apps.sh").read_text()
    app_dir = ROOT / "apps" / "ub_udma"

    assert 'UDMA_SRC="$ROOT_DIR/apps/ub_udma/ub_udma.c"' in build_script
    assert 'UDMA_BIN="$OUT_DIR/linqu_ub_udma"' in build_script
    assert "linqu_ub_udma_demo" not in build_script
    assert "linqu_ub_udma=1" in init_source
    assert "linqu_ub_udma=1" in run_app
    assert "\\\\[ub_udma\\\\] pass" in dual_runner
    assert "\\\\[ub_udma\\\\] fail" in dual_runner
    assert "\\[init\\] ub udma app pass" not in dual_runner
    assert "linqu_ub_udma_demo" not in init_source
    assert "linqu_ub_udma_demo" not in run_app
    assert "linqu_ub_udma_demo" not in dual_runner
    assert "ub udma (app|demo)" not in dual_runner
    assert (app_dir / "ub_udma.c").exists()
    assert (app_dir / "Makefile").exists()
    assert not (ROOT / "apps" / "ub_udma_demo").exists()


def test_ub_tcp_each_server_uses_canonical_app_source():
    build_script = (ROOT / "scripts" / "build_initramfs.sh").read_text()
    init_source = (ROOT / "init.c").read_text()
    run_app = (ROOT / "initramfs" / "run_app").read_text()
    dual_runner = (ROOT / "scripts" / "run_ub_dual_node_apps.sh").read_text()
    eight_runner = (ROOT / "scripts" / "run_ub_eight_node_tcp_each_server_matrix.sh").read_text()
    app_dir = ROOT / "apps" / "ub_tcp_each_server"
    app_source = (app_dir / "ub_tcp_each_server.c").read_text()

    assert (
        'TCP_EACH_SERVER_SRC="$ROOT_DIR/apps/ub_tcp_each_server/ub_tcp_each_server.c"'
        in build_script
    )
    assert "linqu_ub_tcp_each_server=1" in run_app
    assert "linqu_ub_tcp_each_server=1" in init_source
    assert "\\\\[ub_tcp_each_server\\\\] pass" in dual_runner
    assert "\\\\[ub_tcp_each_server\\\\] fail" in dual_runner
    assert "/bin/linqu_ub_tcp_each_server" in eight_runner
    assert "LINQU_URMA_DP_ROLE" in eight_runner
    assert "LINQU_UB_LOCAL_IP" in eight_runner
    assert "LINQU_UB_PEER_IP" in eight_runner
    assert "nodeA nodeH" in eight_runner
    assert "nodeG nodeH" in eight_runner
    assert "\\\\[ub_tcp_each_server\\\\] start role=${role}" in eight_runner
    assert "\\\\[ub_tcp_each_server\\\\] pass" in eight_runner
    assert "rdinit=/bin/run_demo ub_tcp_each_server " not in eight_runner
    assert "env_or_cmdline_value(\"LINQU_URMA_DP_ROLE\"" in app_source
    assert "env_or_cmdline_value(\"LINQU_UB_LOCAL_IP\"" in app_source
    assert "env_or_cmdline_value(\"LINQU_UB_PEER_IP\"" in app_source
    assert "\\[init\\] ub tcp each server app pass" not in dual_runner
    assert "[init] ub tcp each server app pass" in init_source
    assert "run_ub_tcp_each_server_demo_probe" not in init_source
    assert "linqu_ub_tcp_each_server_demo" not in run_app
    assert "linqu_ub_tcp_each_server_demo" not in init_source
    assert "linqu_ub_tcp_each_server_demo" not in dual_runner
    assert "ub tcp each server demo" not in dual_runner
    assert "ub tcp each server demo" not in init_source
    assert (app_dir / "ub_tcp_each_server.c").exists()
    assert not (app_dir / "ub_tcp_each_server_demo.c").exists()
    assert (app_dir / "Makefile").exists()


def test_app_local_makefiles_use_cross_compile_inputs():
    obmm_queue_makefile = (ROOT / "apps" / "obmm_queue" / "Makefile").read_text()
    gsva_query_makefile = (ROOT / "apps" / "gsva_query" / "Makefile").read_text()

    assert "TARGET_CC ?= aarch64-linux-gnu-gcc" in obmm_queue_makefile
    assert "-I$(ROOT)/kernel_ub/include/uapi" in obmm_queue_makefile
    assert "-I$(ROOT)/common" in gsva_query_makefile


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
    matrix_runner_path = (
        ROOT / "scripts" / "run_ub_dual_node_obmm_dataplane_microbench_matrix.sh"
    )
    matrix_runner = matrix_runner_path.read_text()
    eight_runner = (
        ROOT / "scripts" / "run_ub_eight_node_obmm_dataplane_microbench.sh"
    ).read_text()
    app_source = (
        ROOT / "apps" / "obmm_dataplane_microbench" / "obmm_dataplane_microbench.c"
    ).read_text()

    assert "rdinit=/bin/run_app linqu_obmm_dataplane_microbench=1" in runner
    assert "rdinit=/bin/run_demo obmm_dataplane_microbench " not in runner
    assert "--mode MODE" in runner
    assert "DP_MODE=\"$2\"" in runner
    assert "unsupported --mode" in runner
    assert "--size must be aligned to 2097152 bytes" in runner
    assert "legacy|legacy-pa)" in runner
    assert "generic|generic-gva|gva)" in runner
    assert "gsva)" in runner
    assert matrix_runner_path.stat().st_mode & 0o111
    assert "MODES=(legacy-pa generic-gva gsva)" in matrix_runner
    assert "RUNNER_ARGS=(--iters 4096 --chunk-size 64)" in matrix_runner
    assert '"$BASE_RUNNER" --mode "$mode" "${RUNNER_ARGS[@]}"' in matrix_runner
    assert "run_ub_dual_node_obmm_dataplane_microbench.sh" in matrix_runner
    assert "DP_MODE=" not in matrix_runner
    assert "/bin/linqu_ub_obmm_dataplane_microbench" in eight_runner
    assert "DP_MODES=(${=DP_MODES_OVERRIDE:-legacy-pa generic-gva gsva})" in eight_runner
    assert "--node-count 8 --peer-index" in eight_runner
    assert "\\\\[obmm_dataplane_microbench\\\\] local_idx=${local_idx} peer_idx=${peer_idx} node_count=8" in eight_runner
    assert "\\\\[obmm_dataplane_microbench\\\\] bootstrap lookup ok got_count=8 node_count=8 peer_got=1" in eight_runner
    assert "\\\\[obmm_dataplane_microbench\\\\] result=done mode=${mode} .*verify_failures=0" in eight_runner
    assert "rdinit=/bin/run_demo obmm_dataplane_microbench " not in eight_runner
    assert "--node-count" in app_source
    assert "--peer-index" in app_source
    assert "default_peer_index" in app_source
    assert "remote_metas[peer_idx]" in app_source


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
    eight_runner = (ROOT / "scripts" / "run_ub_eight_node_obmm_import_stress.sh").read_text()
    app_source = (ROOT / "apps" / "obmm_import_stress" / "obmm_import_stress.c").read_text()

    assert "rdinit=/bin/run_app linqu_obmm_import_stress=1" in runner
    assert "rdinit=/bin/run_demo obmm_import_stress " not in runner
    assert "/bin/linqu_ub_obmm_import_stress" in eight_runner
    assert 'OBMM_MEMSEG_SIZE=2097152' in eight_runner
    assert 'STRESS_SIZE="${STRESS_SIZE:-$OBMM_MEMSEG_SIZE}"' in eight_runner
    assert "STRESS_SIZE must be aligned to ${OBMM_MEMSEG_SIZE} bytes" in eight_runner
    assert "--node-count 8 --peer-index" in eight_runner
    assert "\\\\[obmm_import_stress\\\\] local_idx=${local_idx} peer_idx=${peer_idx} node_count=8" in eight_runner
    assert "\\\\[obmm_import_stress\\\\] bootstrap lookup ok got_count=8 node_count=8 peer_got=1" in eight_runner
    assert "\\\\[obmm_import_stress\\\\] result=done " in eight_runner
    assert "rdinit=/bin/run_demo obmm_import_stress " not in eight_runner
    assert "--node-count" in app_source
    assert "--peer-index" in app_source
    assert "stress_default_peer_index" in app_source
    assert "remote_metas[peer_idx]" in app_source


def test_obmm_coh_test_has_independent_dual_node_bootflow():
    script = (ROOT / "scripts" / "run_ub_dual_node_apps.sh").read_text()
    init_source = (ROOT / "init.c").read_text()

    assert "obmm_coh_test" in script
    assert "linqu_obmm_coh_test=1" in script
    assert "obmm_coh_test: PASS" in script
    assert "obmm_coh_test: FAIL" in script
    assert "\\[init\\] ub obmm coh test app pass" not in script
    assert "COH_TEST_MODE" in script
    assert "COH_TEST_ITERS" in script
    assert "should_run_obmm_coh_test" in init_source
    assert "run_obmm_coh_test_probe" in init_source
    assert "nodea_obmm_coh_test_append" in script


def test_obmm_coh_test_runner_uses_app_flag_entrypoint():
    runner = (ROOT / "scripts" / "run_ub_dual_node_obmm_coh_test.sh").read_text()
    four_runner = (ROOT / "scripts" / "run_ub_four_node_obmm_coh_test.sh").read_text()
    eight_runner = (ROOT / "scripts" / "run_ub_eight_node_obmm_coh_test.sh").read_text()

    assert "rdinit=/bin/run_app linqu_obmm_coh_test=1" in runner
    assert "rdinit=/bin/run_demo obmm_coh_test " not in runner
    assert "rdinit=/bin/run_app linqu_obmm_coh_test=1" in four_runner
    assert "rdinit=/bin/run_demo obmm_coh_test " not in four_runner
    assert 'COH_NODE_COUNT="${COH_NODE_COUNT:-8}"' in eight_runner
    assert "ub_topology_eight_node_full_mesh.ini" in eight_runner
    assert 'UB_SIM_PORT_NUM="${UB_SIM_PORT_NUM:-7}"' in eight_runner
    assert 'exec "$SCRIPT_DIR/run_ub_four_node_obmm_coh_test.sh" "$@"' in eight_runner
    assert 'pass_count == COH_NODE_COUNT' in four_runner
    assert 'obmm_coh_test_node_count=${COH_NODE_COUNT}' in four_runner


def test_npu_gsva_test_has_independent_app_build():
    build_script = (ROOT / "scripts" / "build_initramfs.sh").read_text()
    app_dir = ROOT / "apps" / "npu_gsva_test"
    app_source = (app_dir / "npu_gsva_test.c").read_text()

    assert 'NPU_GSVA_TEST_SRC="$ROOT_DIR/apps/npu_gsva_test/npu_gsva_test.c"' in build_script
    assert (app_dir / "npu_gsva_test.c").exists()
    assert (app_dir / "Makefile").exists()
    assert "LINQU_NPU_GSVA_PEER_NODE_IDX" in app_source
    assert "linqu_npu_gsva_peer_node_idx" in app_source


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
    app_source = (app_dir / "ssd_gsva_test.c").read_text()

    assert 'SSD_GSVA_TEST_SRC="$ROOT_DIR/apps/ssd_gsva_test/ssd_gsva_test.c"' in build_script
    assert (app_dir / "ssd_gsva_test.c").exists()
    assert (app_dir / "Makefile").exists()
    assert "LINQU_SSD_GSVA_PEER_NODE_IDX" in app_source
    assert "linqu_ssd_gsva_peer_node_idx" in app_source
    assert "LINQU_SSD_GSVA_SUITE" in app_source
    assert "linqu_ssd_gsva_suite" in app_source


def test_mem_service_has_component_and_cli_entrypoints():
    build_script = (ROOT / "scripts" / "build_initramfs.sh").read_text()
    run_app = (ROOT / "initramfs" / "run_app").read_text()
    components_readme = (ROOT / "components" / "README.md").read_text()
    component_dir = ROOT / "components" / "mem_service"
    app_dir = ROOT / "apps" / "mem_service"
    readme = (component_dir / "README.md").read_text()
    app_makefile = (app_dir / "Makefile").read_text()
    app_source = (app_dir / "mem_service.c").read_text()
    release_manifest = (app_dir / "release-manifest.txt").read_text()
    wire_schema_manifest = (app_dir / "wire-schema.txt").read_text()
    compat_matrix = (app_dir / "compat-matrix.txt").read_text()
    compat_baseline = (app_dir / "compat-baseline-v1.txt").read_text()
    compat_old_new = (app_dir / "compat-old-new-matrix.txt").read_text()
    config_schema = (app_dir / "configs" / "mem_service.conf.schema").read_text()
    config_example = (app_dir / "configs" / "mem_service.example.conf").read_text()
    deploy_manifest = (app_dir / "deploy" / "linqu_mem_service.service").read_text()
    serving_example = (app_dir / "examples" / "mem_service_serving_example.c").read_text()
    pretraining_example = (
        app_dir / "examples" / "mem_service_pretraining_example.c"
    ).read_text()

    assert 'MEM_SERVICE_SRC="$ROOT_DIR/components/mem_service/mem_service.c"' in build_script
    assert 'MEM_SERVICE_CLUSTER_UTILS_SRC="$ROOT_DIR/components/mem_service/mem_service_cluster_utils.c"' in build_script
    assert 'MEM_SERVICE_CLUSTER_PAYLOAD_SRC="$ROOT_DIR/components/mem_service/mem_service_cluster_payload.c"' in build_script
    assert 'MEM_SERVICE_CLUSTER_READ_SRC="$ROOT_DIR/components/mem_service/mem_service_cluster_read.c"' in build_script
    assert 'MEM_SERVICE_CLUSTER_RUNTIME_SRC="$ROOT_DIR/components/mem_service/mem_service_cluster_runtime.c"' in build_script
    assert 'MEM_SERVICE_CLUSTER_QUEUE_SRC="$ROOT_DIR/components/mem_service/mem_service_cluster_queue.c"' in build_script
    assert 'MEM_SERVICE_CLUSTER_OBSERVE_SRC="$ROOT_DIR/components/mem_service/mem_service_cluster_observe.c"' in build_script
    assert 'MEM_SERVICE_OBMM_OBJECT_FLOW_SRC="$ROOT_DIR/components/mem_service/mem_service_obmm_object_flow.c"' in build_script
    assert 'MEM_SERVICE_METADATA_SRC="$ROOT_DIR/components/mem_service/mem_service_metadata.c"' in build_script
    assert 'MEM_SERVICE_DAEMON_SRC="$ROOT_DIR/components/mem_service/mem_service_daemon.c"' in build_script
    assert 'MEM_SERVICE_CLIENT_SRC="$ROOT_DIR/components/mem_service/mem_service_client.c"' in build_script
    assert 'MEM_SERVICE_WIRE_CLIENT_SRC="$ROOT_DIR/components/mem_service/mem_service_wire_client.c"' in build_script
    assert 'MEM_SERVICE_KEYS_SRC="$ROOT_DIR/components/mem_service/mem_service_keys.c"' in build_script
    assert 'MEM_SERVICE_OBJECT_REFS_SRC="$ROOT_DIR/components/mem_service/mem_service_object_refs.c"' in build_script
    assert 'MEM_SERVICE_OBMM_OBJECTS_SRC="$ROOT_DIR/components/mem_service/mem_service_obmm_objects.c"' in build_script
    assert 'MEM_SERVICE_RECORDS_SRC="$ROOT_DIR/components/mem_service/mem_service_records.c"' in build_script
    assert 'MEM_SERVICE_QWEN3_RECORDS_SRC="$ROOT_DIR/components/mem_service/mem_service_qwen3_records.c"' in build_script
    assert 'MEM_SERVICE_QWEN3_RUNTIME_SRC="$ROOT_DIR/components/mem_service/mem_service_qwen3_runtime.c"' in build_script
    assert 'MEM_SERVICE_QWEN3_DECODE_BARRIER_SRC="$ROOT_DIR/components/mem_service/mem_service_qwen3_decode_barrier.c"' in build_script
    assert 'MEM_SERVICE_QWEN3_KV_STATE_FLOW_SRC="$ROOT_DIR/components/mem_service/mem_service_qwen3_kv_state_flow.c"' in build_script
    assert 'MEM_SERVICE_QWEN3_TERMINAL_TOKEN_FLOW_SRC="$ROOT_DIR/components/mem_service/mem_service_qwen3_terminal_token_flow.c"' in build_script
    assert 'MEM_SERVICE_QWEN3_RUNTIME_RANGE_WAIT_FLOW_SRC="$ROOT_DIR/components/mem_service/mem_service_qwen3_runtime_range_wait_flow.c"' in build_script
    assert 'MEM_SERVICE_QWEN3_RUNTIME_RANGE_PUBLISH_FLOW_SRC="$ROOT_DIR/components/mem_service/mem_service_qwen3_runtime_range_publish_flow.c"' in build_script
    assert 'MEM_SERVICE_QWEN3_ENGRAM_PUBLISH_FLOW_SRC="$ROOT_DIR/components/mem_service/mem_service_qwen3_engram_publish_flow.c"' in build_script
    assert 'MEM_SERVICE_QWEN3_ENGRAM_WAIT_FLOW_SRC="$ROOT_DIR/components/mem_service/mem_service_qwen3_engram_wait_flow.c"' in build_script
    assert 'MEM_SERVICE_QWEN3_SRC="$ROOT_DIR/components/mem_service/mem_service_qwen3.c"' in build_script
    assert 'MEM_SERVICE_CLI_SRC="$ROOT_DIR/apps/mem_service/mem_service.c"' in build_script
    assert 'MEM_SERVICE_CLI_BIN="$OUT_DIR/linqu_mem_service"' in build_script
    assert 'MEM_SERVICE_QWEN3_CLI_BIN="$OUT_DIR/linqu_mem_service_qwen3"' in build_script
    assert '"$LLM_INFER_APP_SRC" "$MEM_SERVICE_SRC" "$MEM_SERVICE_CLUSTER_UTILS_SRC" "$MEM_SERVICE_CLUSTER_PAYLOAD_SRC" "$MEM_SERVICE_CLUSTER_READ_SRC" "$MEM_SERVICE_CLUSTER_RUNTIME_SRC" "$MEM_SERVICE_CLUSTER_QUEUE_SRC" "$MEM_SERVICE_CLUSTER_OBSERVE_SRC" "$MEM_SERVICE_OBMM_OBJECT_FLOW_SRC" "$MEM_SERVICE_METADATA_SRC" "$MEM_SERVICE_KEYS_SRC" "$MEM_SERVICE_OBJECT_REFS_SRC" "$MEM_SERVICE_OBMM_OBJECTS_SRC" "$MEM_SERVICE_RECORDS_SRC" "$MEM_SERVICE_QWEN3_RECORDS_SRC" "$MEM_SERVICE_QWEN3_RUNTIME_SRC" "$MEM_SERVICE_QWEN3_DECODE_BARRIER_SRC" "$MEM_SERVICE_QWEN3_KV_STATE_FLOW_SRC" "$MEM_SERVICE_QWEN3_TERMINAL_TOKEN_FLOW_SRC" "$MEM_SERVICE_QWEN3_RUNTIME_RANGE_WAIT_FLOW_SRC" "$MEM_SERVICE_QWEN3_RUNTIME_RANGE_PUBLISH_FLOW_SRC" "$MEM_SERVICE_QWEN3_ENGRAM_PUBLISH_FLOW_SRC" "$MEM_SERVICE_QWEN3_ENGRAM_WAIT_FLOW_SRC" "$MEM_SERVICE_QWEN3_SRC" "$LLM_INFER_SRC" -lm -o "$LLM_INFER_APP_BIN"' in build_script
    assert '"$MEM_SERVICE_CLI_SRC" "$MEM_SERVICE_DAEMON_SRC" "$MEM_SERVICE_CLIENT_SRC" "$MEM_SERVICE_WIRE_CLIENT_SRC" "$MEM_SERVICE_METADATA_SRC" "$MEM_SERVICE_KEYS_SRC" "$MEM_SERVICE_OBJECT_REFS_SRC" "$MEM_SERVICE_RECORDS_SRC" -lm -o "$MEM_SERVICE_CLI_BIN"' in build_script
    assert "-DMEM_SERVICE_ENABLE_QWEN3_INSPECT" in build_script
    assert '"$MEM_SERVICE_CLI_SRC" "$MEM_SERVICE_SRC" "$MEM_SERVICE_CLUSTER_UTILS_SRC" "$MEM_SERVICE_CLUSTER_PAYLOAD_SRC" "$MEM_SERVICE_CLUSTER_READ_SRC" "$MEM_SERVICE_CLUSTER_RUNTIME_SRC" "$MEM_SERVICE_CLUSTER_QUEUE_SRC" "$MEM_SERVICE_CLUSTER_OBSERVE_SRC" "$MEM_SERVICE_OBMM_OBJECT_FLOW_SRC" "$MEM_SERVICE_DAEMON_SRC" "$MEM_SERVICE_CLIENT_SRC" "$MEM_SERVICE_WIRE_CLIENT_SRC" "$MEM_SERVICE_METADATA_SRC" "$MEM_SERVICE_KEYS_SRC" "$MEM_SERVICE_OBJECT_REFS_SRC" "$MEM_SERVICE_OBMM_OBJECTS_SRC" "$MEM_SERVICE_RECORDS_SRC" "$MEM_SERVICE_QWEN3_RECORDS_SRC" "$MEM_SERVICE_QWEN3_RUNTIME_SRC" "$MEM_SERVICE_QWEN3_DECODE_BARRIER_SRC" "$MEM_SERVICE_QWEN3_KV_STATE_FLOW_SRC" "$MEM_SERVICE_QWEN3_TERMINAL_TOKEN_FLOW_SRC" "$MEM_SERVICE_QWEN3_RUNTIME_RANGE_WAIT_FLOW_SRC" "$MEM_SERVICE_QWEN3_RUNTIME_RANGE_PUBLISH_FLOW_SRC" "$MEM_SERVICE_QWEN3_ENGRAM_PUBLISH_FLOW_SRC" "$MEM_SERVICE_QWEN3_ENGRAM_WAIT_FLOW_SRC" "$MEM_SERVICE_QWEN3_SRC" "$LLM_INFER_SRC" -lm -o "$MEM_SERVICE_QWEN3_CLI_BIN"' in build_script
    assert "Components do not install guest binaries directly" in components_readme
    assert "standalone demo" not in readme
    assert "linqu_mem_service" in build_script
    assert "linqu_mem_service_qwen3" in build_script
    assert "linqu_mem_service" in run_app
    assert "linqu_mem_service=1" in run_app
    assert "linqu_mem_service_qwen3" in run_app
    assert "linqu_mem_service_wire_fixtures" in run_app
    assert "/bin/linqu_mem_service wire-fixtures" in run_app
    assert "linqu_mem_service_wire_schema_fixtures" in run_app
    assert "/bin/linqu_mem_service wire-schema-fixtures" in run_app
    assert "linqu_mem_service_config_fixtures" in run_app
    assert "/bin/linqu_mem_service config-fixtures" in run_app
    assert "linqu_mem_service_metrics_export_fixtures" in run_app
    assert "/bin/linqu_mem_service metrics-export-fixtures" in run_app
    assert "linqu_mem_service_client_retry_fixtures" in run_app
    assert "/bin/linqu_mem_service client-retry-fixtures" in run_app
    assert "linqu_mem_service_compat_fixtures" in run_app
    assert "/bin/linqu_mem_service compat-fixtures" in run_app
    assert "linqu_mem_service_compat_baseline_fixtures" in run_app
    assert "/bin/linqu_mem_service compat-baseline-fixtures" in run_app
    assert "linqu_mem_service_compat_old_new_fixtures" in run_app
    assert "/bin/linqu_mem_service compat-old-new-fixtures" in run_app
    assert "linqu_mem_service_release_fixtures" in run_app
    assert "/bin/linqu_mem_service release-fixtures" in run_app
    assert (app_dir / "mem_service.c").exists()
    assert (app_dir / "Makefile").exists()
    assert (app_dir / "release-manifest.txt").exists()
    assert (app_dir / "wire-schema.txt").exists()
    assert (app_dir / "compat-matrix.txt").exists()
    assert (app_dir / "compat-baseline-v1.txt").exists()
    assert (app_dir / "compat-old-new-matrix.txt").exists()
    assert (app_dir / "configs" / "mem_service.conf.schema").exists()
    assert (app_dir / "configs" / "mem_service.example.conf").exists()
    assert (app_dir / "deploy" / "linqu_mem_service.service").exists()
    assert (app_dir / "examples" / "mem_service_serving_example.c").exists()
    assert (app_dir / "examples" / "mem_service_pretraining_example.c").exists()
    assert "linqu_mem_service_core" in app_makefile
    assert "linqu_mem_service_qwen3" in app_makefile
    assert "-DMEM_SERVICE_ENABLE_QWEN3_INSPECT" in app_makefile
    assert "MEM_SERVICE_RELEASE_MANIFEST := release-manifest.txt" in app_makefile
    assert "MEM_SERVICE_WIRE_SCHEMA_MANIFEST := wire-schema.txt" in app_makefile
    assert "MEM_SERVICE_COMPAT_MATRIX := compat-matrix.txt" in app_makefile
    assert "MEM_SERVICE_COMPAT_BASELINE_V1 := compat-baseline-v1.txt" in app_makefile
    assert "MEM_SERVICE_COMPAT_OLD_NEW_MATRIX := compat-old-new-matrix.txt" in app_makefile
    assert "MEM_SERVICE_CONFIG_SCHEMA := configs/mem_service.conf.schema" in app_makefile
    assert "MEM_SERVICE_CONFIG_EXAMPLE := configs/mem_service.example.conf" in app_makefile
    assert "MEM_SERVICE_DEPLOY_MANIFEST := deploy/linqu_mem_service.service" in app_makefile
    assert "MEM_SERVICE_CLIENT_EXAMPLES :=" in app_makefile
    assert "examples/mem_service_serving_example.c" in app_makefile
    assert "examples/mem_service_pretraining_example.c" in app_makefile
    assert "INSTALL_EXAMPLEDIR := $(INSTALL_DATADIR)/examples" in app_makefile
    assert "INSTALL_CONFIGDIR := $(INSTALL_DATADIR)/config" in app_makefile
    assert "INSTALL_DEPLOYDIR := $(INSTALL_DATADIR)/deploy" in app_makefile
    assert "MEM_SERVICE_PUBLIC_HEADERS :=" in app_makefile
    assert "$(ROOT)/components/mem_service/mem_service_client.h" in app_makefile
    assert "$(ROOT)/components/mem_service/mem_service_wire_schema.h" in app_makefile
    assert "MEM_SERVICE_CLIENT_SDK_SRCS :=" in app_makefile
    assert "$(ROOT)/components/mem_service/mem_service_client.c" in app_makefile
    assert "$(ROOT)/components/mem_service/mem_service_wire_client.c" in app_makefile
    assert "$(MEM_SERVICE_CONFIG_SCHEMA)" in app_makefile
    assert "$(MEM_SERVICE_CONFIG_EXAMPLE)" in app_makefile
    assert "$(MEM_SERVICE_DEPLOY_MANIFEST)" in app_makefile
    assert "^metrics_export_format=prometheus-text$$" in app_makefile
    assert "^client_retry_policy=explicit-max-attempts-backoff$$" in app_makefile
    assert "^compat_matrix=share/lingqu/mem_service/compat-matrix.txt$$" in app_makefile
    assert "^compat_matrix_checksum=0x8b4219c5$$" in app_makefile
    assert "^compat_baseline=share/lingqu/mem_service/compat-baseline-v1.txt$$" in app_makefile
    assert "^compat_baseline_checksum=0xdc6376da$$" in app_makefile
    assert "^compat_old_new_matrix=share/lingqu/mem_service/compat-old-new-matrix.txt$$" in app_makefile
    assert "^compat_old_new_matrix_checksum=0x56f8e4c3$$" in app_makefile
    assert "^deployment_smoke=deployment-fixtures$$" in app_makefile
    assert "^service_manager_lifecycle=serve-config-ready-scrape-sigterm$$" in app_makefile
    assert "^service_manager_shutdown=signal-clean-stop$$" in app_makefile
    assert "^durable_backend=snapshot+journal$$" in app_makefile
    assert "^durable_catalog=storage-root-v1$$" in app_makefile
    assert "^durable_catalog_manifest=catalog/manifest.txt$$" in app_makefile
    assert "^payload_block_backend=sealed-local-block-v1$$" in app_makefile
    assert "^metrics_listen_config=metrics_listen$$" in app_makefile
    assert "^metrics_http_listener=tcp-ipv4$$" in app_makefile
    assert "^metrics_scrape_path=/metrics$$" in app_makefile
    assert "^metrics_listen=tcp:127.0.0.1:9900$$" in app_makefile
    assert "install-smoke: install" in app_makefile
    assert "print-release-manifest" in app_makefile
    assert "print-wire-schema" in app_makefile
    assert "print-compat-matrix" in app_makefile
    assert "print-compat-baseline-v1" in app_makefile
    assert "print-compat-old-new-matrix" in app_makefile
    core_sources = re.search(
        r"MEM_SERVICE_CORE_SRCS :=(?P<body>.*?)MEM_SERVICE_QWEN3_ADAPTER_SRCS :=",
        app_makefile,
        re.S,
    )
    assert core_sources is not None
    assert "LLM_INFER" not in core_sources.group("body")
    assert "MEM_SERVICE_QWEN3" not in core_sources.group("body")
    assert "$(MEM_SERVICE)" not in core_sources.group("body")
    assert "$(MEM_SERVICE_DAEMON)" in core_sources.group("body")
    assert "$(MEM_SERVICE_CLIENT)" in core_sources.group("body")
    assert "$(MEM_SERVICE_WIRE_CLIENT)" in core_sources.group("body")
    assert '#include "components/mem_service/mem_service_daemon.h"' in app_source
    assert '#include "components/mem_service/mem_service_wire_client.h"' in app_source
    assert 'strcmp(argv[1], "wire-fixtures")' in app_source
    assert 'strcmp(argv[1], "wire-schema")' in app_source
    assert 'strcmp(argv[1], "wire-schema-fixtures")' in app_source
    assert 'strcmp(argv[1], "journal-fixtures")' in app_source
    assert 'strcmp(argv[1], "config-fixtures")' in app_source
    assert 'strcmp(argv[1], "metrics-export-fixtures")' in app_source
    assert 'strcmp(argv[1], "deployment-fixtures")' in app_source
    assert 'strcmp(argv[1], "durable-catalog-fixtures")' in app_source
    assert 'strcmp(argv[1], "client-retry-fixtures")' in app_source
    assert 'strcmp(argv[1], "compat-matrix")' in app_source
    assert 'strcmp(argv[1], "compat-fixtures")' in app_source
    assert 'strcmp(argv[1], "compat-baseline-v1")' in app_source
    assert 'strcmp(argv[1], "compat-baseline-fixtures")' in app_source
    assert 'strcmp(argv[1], "compat-old-new-matrix")' in app_source
    assert 'strcmp(argv[1], "compat-old-new-fixtures")' in app_source
    assert 'strcmp(argv[1], "serve")' in app_source
    assert 'option_value(argc, argv, "--config")' in app_source
    assert 'option_value(argc, argv, "--metrics-listen")' in app_source
    assert "mem_service_run_unix_daemon_with_store_metrics_and_catalog" in app_source
    assert "load_mem_service_config" in app_source
    assert "MEM_SERVICE_CONFIG_SCHEMA_VERSION 1U" in app_source
    assert 'strcmp(argv[1], "release-manifest")' in app_source
    assert 'strcmp(argv[1], "release-fixtures")' in app_source
    assert "run_release_manifest" in app_source
    assert "run_release_fixture_check" in app_source
    assert "run_compat_matrix" in app_source
    assert "run_compat_fixture_check" in app_source
    assert "run_compat_baseline_v1" in app_source
    assert "run_compat_baseline_fixture_check" in app_source
    assert "run_compat_old_new_matrix" in app_source
    assert "run_compat_old_new_fixture_check" in app_source
    assert "MEM_SERVICE_DEPLOYMENT_SMOKE_VERSION 1U" in app_source
    assert "render_metrics_http_response" in app_source
    assert "run_deployment_fixture_check" in app_source
    assert "MEM_SERVICE_COMPAT_MATRIX_EXPECTED_LEN 1887U" in app_source
    assert "MEM_SERVICE_COMPAT_MATRIX_EXPECTED_CHECKSUM 0x8b4219c5U" in app_source
    assert "MEM_SERVICE_COMPAT_BASELINE_V1_EXPECTED_LEN 1208U" in app_source
    assert "MEM_SERVICE_COMPAT_BASELINE_V1_EXPECTED_CHECKSUM 0xdc6376daU" in app_source
    assert "MEM_SERVICE_COMPAT_OLD_NEW_MATRIX_EXPECTED_LEN 1590U" in app_source
    assert "MEM_SERVICE_COMPAT_OLD_NEW_MATRIX_EXPECTED_CHECKSUM 0x56f8e4c3U" in app_source
    assert "run_wire_schema_manifest" in app_source
    assert "run_wire_schema_fixture_check" in app_source
    assert "MEM_SERVICE_WIRE_SCHEMA_MANIFEST_EXPECTED_LEN 9220U" in app_source
    assert "MEM_SERVICE_WIRE_SCHEMA_MANIFEST_EXPECTED_CHECKSUM 0xce883650U" in app_source
    assert 'strcmp(argv[1], "health")' in app_source
    assert 'strcmp(argv[1], "ready")' in app_source
    assert 'strcmp(argv[1], "metrics")' in app_source
    assert 'strcmp(argv[1], "audit-log")' in app_source
    assert 'strcmp(argv[1], "metrics-export")' in app_source
    assert "render_metrics_prometheus_text" in app_source
    assert "lingqu_mem_service_request_count" in app_source
    assert "--max-attempts" in app_source
    assert "--retry-backoff-ms" in app_source
    assert "--retry-timeouts" in app_source
    assert 'strcmp(argv[1], "export-snapshot")' in app_source
    assert 'strcmp(argv[1], "export-snapshot-page")' in app_source
    assert 'strcmp(argv[1], "export-snapshot-to")' in app_source
    assert 'strcmp(argv[1], "restore-snapshot")' in app_source
    assert 'strcmp(argv[1], "put-object")' in app_source
    assert 'strcmp(argv[1], "get-object")' in app_source
    assert 'strcmp(argv[1], "inspect-object")' in app_source
    assert 'strcmp(argv[1], "register-prefix")' in app_source
    assert 'strcmp(argv[1], "lookup-prefix")' in app_source
    assert 'strcmp(argv[1], "publish-kv")' in app_source
    assert 'strcmp(argv[1], "resolve-kv")' in app_source
    assert 'strcmp(argv[1], "publish-runtime-handoff")' in app_source
    assert 'strcmp(argv[1], "resolve-runtime-handoff")' in app_source
    assert 'strcmp(argv[1], "register-execution-artifact")' in app_source
    assert 'strcmp(argv[1], "query-execution-artifact")' in app_source
    assert 'strcmp(argv[1], "register-training-artifact")' in app_source
    assert 'strcmp(argv[1], "query-training-artifact")' in app_source
    assert 'strcmp(argv[1], "commit-training-step")' in app_source
    assert 'strcmp(argv[1], "resolve-training-step")' in app_source
    assert "#ifdef MEM_SERVICE_ENABLE_QWEN3_INSPECT" in app_source
    assert "mem_service_release_manifest_version=1" in release_manifest
    assert "core_binary=bin/linqu_mem_service" in release_manifest
    assert "public_header=include/lingqu/mem_service/mem_service_client.h" in release_manifest
    assert "client_source=src/lingqu/mem_service/mem_service_client.c" in release_manifest
    assert (
        "example_source=share/lingqu/mem_service/examples/"
        "mem_service_serving_example.c"
    ) in release_manifest
    assert (
        "example_source=share/lingqu/mem_service/examples/"
        "mem_service_pretraining_example.c"
    ) in release_manifest
    assert "wire_schema_manifest=share/lingqu/mem_service/wire-schema.txt" in release_manifest
    assert "compat_matrix=share/lingqu/mem_service/compat-matrix.txt" in release_manifest
    assert "compat_matrix_checksum=0x8b4219c5" in release_manifest
    assert "compat_baseline=share/lingqu/mem_service/compat-baseline-v1.txt" in release_manifest
    assert "compat_baseline_checksum=0xdc6376da" in release_manifest
    assert "compat_old_new_matrix=share/lingqu/mem_service/compat-old-new-matrix.txt" in release_manifest
    assert "compat_old_new_matrix_checksum=0x56f8e4c3" in release_manifest
    assert "deployment_smoke=deployment-fixtures" in release_manifest
    assert "service_manager_lifecycle=serve-config-ready-scrape-sigterm" in release_manifest
    assert "service_manager_shutdown=signal-clean-stop" in release_manifest
    assert "durable_backend=snapshot+journal" in release_manifest
    assert "durable_catalog=storage-root-v1" in release_manifest
    assert "durable_catalog_manifest=catalog/manifest.txt" in release_manifest
    assert "payload_block_backend=sealed-local-block-v1" in release_manifest
    assert "durable_journal=store-path.journal" in release_manifest
    assert "metrics_listen_config=metrics_listen" in release_manifest
    assert "metrics_http_listener=tcp-ipv4" in release_manifest
    assert "metrics_scrape_path=/metrics" in release_manifest
    assert "metrics_http_content_type=text/plain; version=0.0.4" in release_manifest
    assert "mem_service_compat_matrix_version=1" in compat_matrix
    assert "wire_version_current=1" in compat_matrix
    assert "wire_schema_manifest_checksum=0xce883650" in compat_matrix
    assert "idempotency_conflict_status=version_conflict" in compat_matrix
    assert "idempotency_persistence=store-journal-and-full-snapshot" in compat_matrix
    assert "audit_log_persistence=store-journal-and-full-snapshot" in compat_matrix
    assert "journal_scope=completed-idempotency-and-audit-events" in compat_matrix
    assert "compat_test=journal-fixtures" in compat_matrix
    assert "compat_test=deployment-fixtures" in compat_matrix
    assert "mem_service_compat_baseline_version=1" in compat_baseline
    assert "old_client_new_server=compatible-within-v1" in compat_baseline
    assert "new_client_old_server=not-certified" in compat_baseline
    assert "baseline_payload=register_training_artifact:v1-training-step-compatible" in compat_baseline
    assert "mem_service_old_new_compat_matrix_version=1" in compat_old_new
    assert "certified_pair=current-v1-client->old-v1-schema-profile" in compat_old_new
    assert "not_certified_pair=current-v1-client->old-v1-runtime-binary" in compat_old_new
    assert "certification_limit=old-server-runtime-binary-not-certified" in compat_old_new
    assert "wire_schema_manifest_len=9220" in release_manifest
    assert "wire_schema_manifest_checksum=0xce883650" in release_manifest
    assert "config_schema_version=1" in release_manifest
    assert "config_schema=share/lingqu/mem_service/config/mem_service.conf.schema" in release_manifest
    assert "config_example=share/lingqu/mem_service/config/mem_service.example.conf" in release_manifest
    assert "deployment_manifest=share/lingqu/mem_service/deploy/linqu_mem_service.service" in release_manifest
    assert "metrics_export_format=prometheus-text" in release_manifest
    assert "client_retry_policy=explicit-max-attempts-backoff" in release_manifest
    assert "client_api=pretraining-refs-v1" in release_manifest
    assert "client_api=pretraining-step-commit-v1" in release_manifest
    assert "operation=metrics:5" in release_manifest
    assert "operation=audit_log:10" in release_manifest
    assert "operation=export_snapshot:6" in release_manifest
    assert "operation=export_snapshot_page:7" in release_manifest
    assert "operation=restore_snapshot:8" in release_manifest
    assert "operation=restore_snapshot_page:9" in release_manifest
    assert "operation=inspect_object:18" in release_manifest
    assert "operation=query_training_artifact:97" in release_manifest
    assert "status=internal:10" in release_manifest
    assert "mem_service_wire_schema_manifest_version=1" in wire_schema_manifest
    assert "operation_count=23" in wire_schema_manifest
    assert "operation=metrics:5" in wire_schema_manifest
    assert "operation=audit_log:10" in wire_schema_manifest
    assert "operation=export_snapshot:6" in wire_schema_manifest
    assert "operation=export_snapshot_page:7" in wire_schema_manifest
    assert "operation=restore_snapshot:8" in wire_schema_manifest
    assert "operation=restore_snapshot_page:9" in wire_schema_manifest
    assert "operation=inspect_object:18" in wire_schema_manifest
    assert "field_count=110" in wire_schema_manifest
    assert "oneof_field=resolve_kv_segment.0.block_hash" in wire_schema_manifest
    assert "mem_service_config_schema_version=1" in config_schema
    assert "field=listen type=string" in config_schema
    assert "field=store type=string" in config_schema
    assert "field=storage_root type=string" in config_schema
    assert "field=backend type=enum values=snapshot,snapshot+journal" in config_schema
    assert "field=metrics_listen type=string" in config_schema
    assert "listen=unix:/tmp/linqu_mem_service.sock" in config_example
    assert "store=/tmp/linqu_mem_service.store" in config_example
    assert "backend=snapshot+journal" in config_example
    assert "metrics_listen=tcp:127.0.0.1:9900" in config_example
    assert "ExecStart=/usr/bin/linqu_mem_service serve --config /etc/lingqu/mem_service/mem_service.conf" in deploy_manifest
    assert '#include "mem_service_client.h"' in serving_example
    assert "mem_service_client_register_prefix_entry" in serving_example
    assert "mem_service_client_publish_kv_segment" in serving_example
    assert "mem_service_client_publish_runtime_handoff" in serving_example
    assert "mem_service_client_register_execution_artifact" in serving_example
    assert "mem_service_serving_example=ok" in serving_example
    assert '#include "mem_service_client.h"' in pretraining_example
    assert "mem_service_client_training_ref" in pretraining_example
    assert "mem_service_client_publish_dataset_shard" in pretraining_example
    assert "mem_service_client_resolve_dataset_shard" in pretraining_example
    assert "mem_service_client_publish_sample_batch" in pretraining_example
    assert "mem_service_client_resolve_sample_batch" in pretraining_example
    assert "mem_service_client_publish_checkpoint" in pretraining_example
    assert "mem_service_client_resolve_checkpoint" in pretraining_example
    assert "mem_service_client_publish_gradient_bucket" in pretraining_example
    assert "mem_service_client_resolve_gradient_bucket" in pretraining_example
    assert "mem_service_client_publish_optimizer_state" in pretraining_example
    assert "mem_service_client_resolve_optimizer_state" in pretraining_example
    assert "mem_service_client_register_training_artifact" not in pretraining_example
    assert "dataset-shard" in pretraining_example
    assert "sample-batch" in pretraining_example
    assert "checkpoint" in pretraining_example
    assert "gradient-bucket" in pretraining_example
    assert "optimizer-state" in pretraining_example
    assert "mem_service_pretraining_example=ok" in pretraining_example
    assert not (ROOT / "apps" / "mem_service_demo").exists()
    assert "test_mem_service_record_recycling.py" in readme
    assert (component_dir / "mem_service.c").exists()
    assert (component_dir / "mem_service_cluster_utils.c").exists()
    assert (component_dir / "mem_service_cluster_utils.h").exists()
    assert (component_dir / "mem_service_cluster_payload.c").exists()
    assert (component_dir / "mem_service_cluster_payload.h").exists()
    assert (component_dir / "mem_service_cluster_read.c").exists()
    assert (component_dir / "mem_service_cluster_read.h").exists()
    assert (component_dir / "mem_service_cluster_runtime.c").exists()
    assert (component_dir / "mem_service_cluster_runtime.h").exists()
    assert (component_dir / "mem_service_cluster_queue.c").exists()
    assert (component_dir / "mem_service_cluster_queue.h").exists()
    assert (component_dir / "mem_service_cluster_observe.c").exists()
    assert (component_dir / "mem_service_cluster_observe.h").exists()
    assert (component_dir / "mem_service_obmm_object_flow.c").exists()
    assert (component_dir / "mem_service_obmm_object_flow.h").exists()
    assert (component_dir / "mem_service_metadata.c").exists()
    assert (component_dir / "mem_service_daemon.c").exists()
    assert (component_dir / "mem_service_daemon.h").exists()
    assert (component_dir / "mem_service_client.c").exists()
    assert (component_dir / "mem_service_client.h").exists()
    assert (component_dir / "mem_service_wire.h").exists()
    assert (component_dir / "mem_service_wire_client.c").exists()
    assert (component_dir / "mem_service_wire_client.h").exists()
    assert (component_dir / "mem_service_keys.c").exists()
    assert (component_dir / "mem_service_keys.h").exists()
    assert (component_dir / "mem_service_object_refs.c").exists()
    assert (component_dir / "mem_service_object_refs.h").exists()
    assert (component_dir / "mem_service_obmm_objects.c").exists()
    assert (component_dir / "mem_service_obmm_objects.h").exists()
    assert (component_dir / "mem_service.h").exists()
    assert (component_dir / "mem_service_core.h").exists()
    assert (component_dir / "mem_service_qwen3_records.c").exists()
    assert (component_dir / "mem_service_qwen3_runtime.c").exists()
    assert (component_dir / "mem_service_qwen3_decode_barrier.c").exists()
    assert (component_dir / "mem_service_qwen3_kv_state_flow.c").exists()
    assert (component_dir / "mem_service_qwen3_terminal_token_flow.c").exists()
    assert (component_dir / "mem_service_qwen3_runtime_range_wait_flow.c").exists()
    assert (component_dir / "mem_service_qwen3_runtime_range_publish_flow.c").exists()
    assert (component_dir / "mem_service_qwen3_engram_publish_flow.c").exists()
    assert (component_dir / "mem_service_qwen3_engram_wait_flow.c").exists()
    assert (component_dir / "mem_service_qwen3.c").exists()
    assert (component_dir / "mem_service_qwen3.h").exists()
    assert (component_dir / "lingqu_object_service.h").exists()


def test_llm_infer_has_app_local_build_entrypoint():
    app_dir = ROOT / "apps" / "llm_infer"
    makefile = (app_dir / "Makefile").read_text()
    readme = (app_dir / "README.md").read_text()

    assert (app_dir / "llm_infer.c").exists()
    assert (app_dir / "Makefile").exists()
    assert "all: linqu_llm_infer" in makefile
    assert "components/mem_service/mem_service.c" in makefile
    assert "components/mem_service/mem_service_cluster_utils.c" in makefile
    assert "components/mem_service/mem_service_cluster_payload.c" in makefile
    assert "components/mem_service/mem_service_cluster_read.c" in makefile
    assert "components/mem_service/mem_service_cluster_runtime.c" in makefile
    assert "components/mem_service/mem_service_cluster_queue.c" in makefile
    assert "components/mem_service/mem_service_cluster_observe.c" in makefile
    assert "components/mem_service/mem_service_obmm_object_flow.c" in makefile
    assert "components/mem_service/mem_service_metadata.c" in makefile
    assert "components/mem_service/mem_service_keys.c" in makefile
    assert "components/mem_service/mem_service_object_refs.c" in makefile
    assert "components/mem_service/mem_service_obmm_objects.c" in makefile
    assert "components/mem_service/mem_service_records.c" in makefile
    assert "components/mem_service/mem_service_qwen3_records.c" in makefile
    assert "components/mem_service/mem_service_qwen3_runtime.c" in makefile
    assert "components/mem_service/mem_service_qwen3_decode_barrier.c" in makefile
    assert "components/mem_service/mem_service_qwen3_kv_state_flow.c" in makefile
    assert "components/mem_service/mem_service_qwen3_terminal_token_flow.c" in makefile
    assert "components/mem_service/mem_service_qwen3_runtime_range_wait_flow.c" in makefile
    assert "components/mem_service/mem_service_qwen3_runtime_range_publish_flow.c" in makefile
    assert "components/mem_service/mem_service_qwen3_engram_publish_flow.c" in makefile
    assert "components/mem_service/mem_service_qwen3_engram_wait_flow.c" in makefile
    assert "components/llm_infer/llm_infer.c" in makefile
    assert "-I$(ROOT)/libs/obmm_queue" in makefile
    assert "-I$(ROOT)/apps/obmm_queue" in makefile
    assert "$^ -lm -o $@" in makefile
    assert "components/llm_infer/" in readme
    assert "app-local `Makefile`" in readme
    assert "scripts/build_initramfs.sh" in readme


def test_llm_infer_is_guest_component_consumed_by_llm_infer_app():
    build_script = (ROOT / "scripts" / "build_initramfs.sh").read_text()
    llm_infer_app_source = (ROOT / "apps" / "llm_infer" / "llm_infer.c").read_text()
    component_dir = ROOT / "components" / "llm_infer"
    component_source = (component_dir / "llm_infer.c").read_text()
    component_header = (component_dir / "llm_infer.h").read_text()
    component_readme = (component_dir / "README.md").read_text()

    assert (component_dir / "llm_infer.c").exists()
    assert (component_dir / "llm_infer.h").exists()
    assert (component_dir / "README.md").exists()
    assert "LLM_INFER_SRC=" in build_script
    assert '"$LLM_INFER_SRC" -lm -o "$LLM_INFER_APP_BIN"' in build_script
    assert "write_signature_line \"llm_infer_src\"" in build_script
    assert '#include "components/llm_infer/llm_infer.h"' in llm_infer_app_source
    assert "static uint64_t qwen3_pipeline_nodes" not in llm_infer_app_source
    assert "static uint64_t qwen3_total_layers" not in llm_infer_app_source
    assert "static uint64_t qwen3_vocab_size" not in llm_infer_app_source
    assert "static const char *qwen3_model_id" not in llm_infer_app_source
    assert "static bool is_qwen3_profile_name" not in llm_infer_app_source
    assert "llm_infer_qwen3_pipeline_nodes" in llm_infer_app_source
    assert "llm_infer_qwen3_total_layers" in component_header
    assert "llm_infer_qwen3_vocab_size" in component_source
    assert "current model option is" in component_readme
    assert "Qwen3" in component_readme
    assert not (ROOT / "apps" / "w4_guest").exists()


def test_obmm_gsva_uses_canonical_app_source():
    build_script = (ROOT / "scripts" / "build_initramfs.sh").read_text()
    run_app = (ROOT / "initramfs" / "run_app").read_text()
    dual_runner = (ROOT / "scripts" / "run_ub_dual_node_obmm_gsva.sh").read_text()
    dual_apps_runner = (ROOT / "scripts" / "run_ub_dual_node_apps.sh").read_text()
    multi_runner = (ROOT / "scripts" / "run_ub_multi_node_obmm_gsva_matrix.sh").read_text()
    eight_node_wrapper = (
        ROOT / "scripts" / "run_ub_eight_node_obmm_gsva_matrix.sh"
    ).read_text()
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
    assert "linqu_obmm_gsva=1" in run_app
    assert "linqu_obmm_gsva_demo" not in run_app
    assert "obmm_gsva_demo" not in run_app
    assert "linqu_obmm_gsva=1" in dual_apps_runner
    assert "obmm_gsva" in dual_apps_runner
    assert "rdinit=/bin/run_app linqu_obmm_gsva=1" in dual_runner
    assert "OBMM_GSVA_MODE" in dual_runner
    assert "rdinit=/bin/run_demo obmm_gsva " not in dual_runner
    assert "GSVA_DEMO_" not in dual_runner
    assert "GSVA_DEMO_" not in wrapper_runners
    assert "[obmm-gsva]" in dual_runner
    assert "rdinit=/bin/run_app linqu_obmm_gsva=1" in multi_runner
    assert "rdinit=/bin/run_demo obmm_gsva " not in multi_runner
    assert "-qmp unix:" not in multi_runner
    assert "OBMM_GSVA_MATRIX_NODE_COUNT" in multi_runner
    assert "export OBMM_GSVA_MATRIX_NODE_COUNT=8" in eight_node_wrapper
    assert 'exec "$SCRIPT_DIR/run_ub_multi_node_obmm_gsva_matrix.sh" "$@"' in eight_node_wrapper
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
    assert "\\\\[obmm_gsva\\\\] result=done" in script
    assert "\\[init\\] ub obmm gsva app pass" not in script
    assert "obmm_gsva_mode=${OBMM_GSVA_MODE}" in script
    assert "obmm_gsva_node_count=${OBMM_GSVA_NODE_COUNT}" in script
    assert "should_run_obmm_gsva" in init_source
    assert "run_obmm_gsva_probe" in init_source
    assert "append_cmdline_if_missing \"obmm_gsva_mode=${OBMM_GSVA_MODE}\"" in script


def test_gva_direct_has_independent_dual_node_bootflow():
    script = (ROOT / "scripts" / "run_ub_dual_node_apps.sh").read_text()
    init_source = (ROOT / "init.c").read_text()
    eight_node_runner = (ROOT / "scripts" / "run_ub_eight_node_gva_direct_test.sh").read_text()

    assert "linqu_gva_direct=1" in script
    assert "gva_direct_mode=${GVA_DIRECT_MODE}" in script
    assert "gva_direct_size=${GVA_DIRECT_SIZE}" in script
    assert "gva_direct_local_va=${GVA_DIRECT_LOCAL_VA}" in script
    assert "gva_direct_home_va=${GVA_DIRECT_HOME_VA}" in script
    assert "/bin/linqu_gva_direct --mode ${GVA_DIRECT_MODE}" in eight_node_runner
    assert "--node-count ${GVA_DIRECT_NODE_COUNT}" in eight_node_runner
    assert "--node-idx ${node_idx}" in eight_node_runner
    assert "GVA_DIRECT_NODE_COUNT=8" in eight_node_runner
    assert "launch_ub_eight_node_headless.sh" in eight_node_runner
    assert "GVA_S3_MAP" in eight_node_runner
    assert "GVA_PATH" in eight_node_runner
    assert "\\\\[gva_direct\\\\] result=done" in script
    assert "\\[init\\] ub gva direct app pass" not in script
    assert "should_run_gva_direct" in init_source
    assert "run_gva_direct_probe" in init_source
    assert "gva_direct_node_count" in init_source
    assert "gva_direct_enabled" in script
    assert "validate_gva_direct_log" in script


def test_gsva_query_has_independent_dual_node_bootflow():
    script = (ROOT / "scripts" / "run_ub_dual_node_apps.sh").read_text()
    init_source = (ROOT / "init.c").read_text()

    assert "linqu_gsva_query=1" in script
    assert "verdict=PASS" in script
    assert "verdict=FAIL" in script
    assert "\\[init\\] ub gsva query app pass" not in script
    assert "should_run_gsva_query" in init_source
    assert "run_gsva_query_probe" in init_source
    assert "gsva_query_enabled" in script
    assert "validate_gsva_query_log" in script


def test_npu_test_has_independent_dual_node_bootflow():
    script = (ROOT / "scripts" / "run_ub_dual_node_apps.sh").read_text()
    init_source = (ROOT / "init.c").read_text()

    assert "linqu_npu_test=1" in script
    assert "\\\\[npu_test\\\\] verdict=(PASS|SKIP)" in script
    assert "\\\\[npu_test\\\\] verdict=FAIL" in script
    assert "\\[init\\] ub npu test app pass" not in script
    assert "should_run_npu_test" in init_source
    assert "run_npu_test_probe" in init_source
    assert "npu_test_enabled" in script
    assert "validate_npu_test_log" in script


def test_gsva_query_runner_uses_app_flag_entrypoint():
    runner = (ROOT / "scripts" / "run_ub_dual_node_gsva_query.sh").read_text()
    caps_runner = (ROOT / "scripts" / "run_ub_gsva_query_caps_test.sh").read_text()
    eight_runner = (ROOT / "scripts" / "run_ub_eight_node_gsva_query_caps.sh").read_text()
    app_source = (ROOT / "apps" / "gsva_query" / "gsva_query.c").read_text()

    assert "start_node nodeA nodeA" in runner
    assert "start_node nodeB nodeB" in runner
    assert "linqu_gsva_query=1 gsva_query_mode=caps" in runner
    assert "-qmp" not in runner
    assert "rdinit=/bin/run_app linqu_gsva_query=1" in caps_runner
    assert "rdinit=/bin/run_demo gsva_query " not in runner
    assert "rdinit=/bin/run_demo gsva_query " not in caps_runner
    assert "/bin/linqu_ub_gsva_query --caps" in eight_runner
    assert "\\\\[gsva_query\\\\] GSVA_QUERY_CAPS" in eight_runner
    assert "caps:.*STRICT_ADDRESS_IDENTITY" in eight_runner
    assert "verdict=PASS" in eight_runner
    assert "struct gsva_query_resp" in app_source
    assert "struct gsva_caps_resp *caps = (struct gsva_caps_resp *)resp->data" in app_source
    assert "NODE_IDS=(nodeA nodeB nodeC nodeD nodeE nodeF nodeG nodeH)" in eight_runner
    assert "launch_ub_eight_node_headless.sh" in eight_runner
    assert "rdinit=/bin/run_demo gsva_query " not in eight_runner


def test_gsva_query_uses_canonical_app_source():
    run_app = (ROOT / "initramfs" / "run_app").read_text()

    assert "linqu_gsva_query=1" in run_app
    assert "run_gsva_query" in run_app
    assert "gsva_query_demo" not in run_app


def test_gsva_coh_all_mode_keeps_test_blocks_disjoint():
    source = (ROOT / "apps" / "gsva_coh_test" / "gsva_coh_test.c").read_text()

    assert "uint64_t base = GSVA_BASE + 0x800000ULL;" not in source
    assert "uint64_t base = GSVA_BASE + 0x2000000ULL;" in source


def test_gsva_coh_and_lifecycle_runner_uses_app_flag_entrypoint():
    two_node_coh_runner = (ROOT / "scripts" / "run_ub_two_node_gsva_coh_test.sh").read_text()
    two_node_lifecycle_runner = (ROOT / "scripts" / "run_ub_two_node_gsva_lifecycle_test.sh").read_text()
    four_node_coh_runner = (ROOT / "scripts" / "run_ub_four_node_gsva_coh_test.sh").read_text()
    eight_node_coh_runner = (ROOT / "scripts" / "run_ub_eight_node_gsva_coh_test.sh").read_text()
    four_node_lifecycle_runner = (
        ROOT / "scripts" / "run_ub_four_node_gsva_lifecycle_test.sh"
    ).read_text()
    eight_node_lifecycle_runner = (
        ROOT / "scripts" / "run_ub_eight_node_gsva_lifecycle_test.sh"
    ).read_text()

    assert "rdinit=/bin/run_app linqu_gsva_coh_test=1" in two_node_coh_runner
    assert "rdinit=/bin/run_demo gsva_coh_test " not in two_node_coh_runner
    assert "rdinit=/bin/run_app linqu_gsva_coh_test=1" in four_node_coh_runner
    assert "rdinit=/bin/run_demo gsva_coh_test " not in four_node_coh_runner
    assert "rdinit=/bin/run_app linqu_gsva_coh_test=1" in eight_node_coh_runner
    assert "rdinit=/bin/run_demo gsva_coh_test " not in eight_node_coh_runner
    assert "rdinit=/bin/run_app linqu_gsva_lifecycle_test=1" in two_node_lifecycle_runner
    assert "rdinit=/bin/run_demo gsva_lifecycle_test " not in two_node_lifecycle_runner
    assert 'GSVA_MODE="${GSVA_MODE:-arm_mmu}"' in two_node_lifecycle_runner
    assert "GSVA_MODE=\"$GSVA_MODE\"" in two_node_lifecycle_runner
    assert "GSVA_TLB: lookup" in two_node_lifecycle_runner
    assert "GVA_TCG_TRANSLATE" in two_node_lifecycle_runner
    assert "rdinit=/bin/run_app linqu_gsva_lifecycle_test=1" in four_node_lifecycle_runner
    assert "rdinit=/bin/run_demo gsva_lifecycle_test " not in four_node_lifecycle_runner
    assert "rdinit=/bin/run_app linqu_gsva_lifecycle_test=1" in eight_node_lifecycle_runner
    assert "rdinit=/bin/run_demo gsva_lifecycle_test " not in eight_node_lifecycle_runner


def test_npu_ssd_gsva_runner_uses_app_flag_entrypoint():
    two_node_npu_runner = (ROOT / "scripts" / "run_ub_two_node_npu_test.sh").read_text()
    two_node_npu_gsva_runner = (ROOT / "scripts" / "run_ub_two_node_npu_gsva_test.sh").read_text()
    eight_node_npu_runner = (ROOT / "scripts" / "run_ub_eight_node_npu_test.sh").read_text()
    two_node_ssd_runner = (ROOT / "scripts" / "run_ub_two_node_ssd_test.sh").read_text()
    two_node_ssd_gsva_runner = (ROOT / "scripts" / "run_ub_two_node_ssd_gsva_test.sh").read_text()
    eight_node_ssd_runner = (ROOT / "scripts" / "run_ub_eight_node_ssd_test.sh").read_text()
    four_node_npu_gsva_runner = (ROOT / "scripts" / "run_ub_four_node_npu_gsva_test.sh").read_text()
    four_node_ssd_gsva_runner = (ROOT / "scripts" / "run_ub_four_node_ssd_gsva_test.sh").read_text()
    eight_node_npu_gsva_runner = (ROOT / "scripts" / "run_ub_eight_node_npu_gsva_test.sh").read_text()
    eight_node_ssd_gsva_runner = (ROOT / "scripts" / "run_ub_eight_node_ssd_gsva_test.sh").read_text()

    assert "rdinit=/bin/run_app linqu_npu_test=1" in two_node_npu_runner
    assert "rdinit=/bin/run_demo npu_test " not in two_node_npu_runner
    assert "/bin/npu_test" in eight_node_npu_runner
    assert "NODE_IDS=(nodeA nodeB nodeC nodeD nodeE nodeF nodeG nodeH)" in eight_node_npu_runner
    assert "launch_ub_eight_node_headless.sh" in eight_node_npu_runner
    assert "UB_NPU: created" in eight_node_npu_runner
    assert "\\\\[npu_test\\\\] verdict=PASS" in eight_node_npu_runner
    assert "rdinit=/bin/run_demo npu_test " not in eight_node_npu_runner
    assert "rdinit=/bin/run_app linqu_npu_gsva_test=1" in two_node_npu_gsva_runner
    assert "rdinit=/bin/run_demo npu_gsva_test " not in two_node_npu_gsva_runner
    assert "rdinit=/bin/run_app linqu_ssd_test=1" in two_node_ssd_runner
    assert "rdinit=/bin/run_demo ssd_test " not in two_node_ssd_runner
    assert "/bin/ssd_test" in eight_node_ssd_runner
    assert "NODE_IDS=(nodeA nodeB nodeC nodeD nodeE nodeF nodeG nodeH)" in eight_node_ssd_runner
    assert "launch_ub_eight_node_headless.sh" in eight_node_ssd_runner
    assert "UB_SSD: created" in eight_node_ssd_runner
    assert "\\\\[ssd_test\\\\] verdict=PASS" in eight_node_ssd_runner
    assert "rdinit=/bin/run_demo ssd_test " not in eight_node_ssd_runner
    assert "rdinit=/bin/run_app linqu_ssd_gsva_test=1" in two_node_ssd_gsva_runner
    assert "rdinit=/bin/run_demo ssd_gsva_test " not in two_node_ssd_gsva_runner
    assert "rdinit=/bin/run_app linqu_npu_gsva_test=1" in four_node_npu_gsva_runner
    assert "rdinit=/bin/run_demo npu_gsva_test " not in four_node_npu_gsva_runner
    assert "rdinit=/bin/run_app linqu_ssd_gsva_test=1" in four_node_ssd_gsva_runner
    assert "rdinit=/bin/run_demo ssd_gsva_test " not in four_node_ssd_gsva_runner
    assert "rdinit=/bin/run_app linqu_npu_gsva_test=1" in eight_node_npu_gsva_runner
    assert "linqu_npu_gsva_peer_node_idx=${peer_node_idx}" in eight_node_npu_gsva_runner
    assert "validate_ub_gsva_peer_matrix" not in eight_node_npu_gsva_runner
    assert "Testing peer 1/1 node_idx=${peer_node_idx}" in eight_node_npu_gsva_runner
    assert "rdinit=/bin/run_demo npu_gsva_test " not in eight_node_npu_gsva_runner
    assert "rdinit=/bin/run_app linqu_ssd_gsva_test=1" in eight_node_ssd_gsva_runner
    assert "linqu_ssd_gsva_peer_node_idx=${peer_node_idx}" in eight_node_ssd_gsva_runner
    assert "linqu_ssd_gsva_suite=matrix" in eight_node_ssd_gsva_runner
    assert "suite=matrix" in eight_node_ssd_gsva_runner
    assert "validate_ub_gsva_peer_matrix" not in eight_node_ssd_gsva_runner
    assert "Testing peer 1/1 node_idx=${peer_node_idx}" in eight_node_ssd_gsva_runner
    assert "rdinit=/bin/run_demo ssd_gsva_test " not in eight_node_ssd_gsva_runner


def test_gva_direct_uses_canonical_app_source():
    build_script = (ROOT / "scripts" / "build_initramfs.sh").read_text()
    run_app = (ROOT / "initramfs" / "run_app").read_text()
    app_dir = ROOT / "apps" / "gva_direct"
    app_source = (app_dir / "gva_direct.c").read_text()

    assert 'GVA_DIRECT_SRC="$ROOT_DIR/apps/gva_direct/gva_direct.c"' in build_script
    assert 'GVA_DIRECT_BIN="$OUT_DIR/linqu_gva_direct"' in build_script
    assert "linqu_gva_direct_demo" not in build_script
    assert "linqu_gva_direct=1" in run_app
    assert "linqu_gva_direct_demo" not in run_app
    assert "gva_direct_demo" not in run_app
    assert "--node-count" in app_source
    assert "--node-idx" in app_source
    assert "run_home_multi_peer" in app_source
    assert "run_peer_multi_peer" in app_source
    assert "GVA_DIRECT_SLOT_STRIDE" in app_source
    assert (app_dir / "gva_direct.c").exists()
    assert (app_dir / "Makefile").exists()
    assert not (ROOT / "apps" / "gva_direct_demo").exists()


def test_gva_direct_runner_uses_app_flag_entrypoint():
    runner = (ROOT / "scripts" / "run_ub_dual_node_gva_direct_test.sh").read_text()
    assert "rdinit=/bin/run_app linqu_gva_direct=1" in runner
    assert "linqu_gva_direct=1" in runner
    assert "rdinit=/bin/run_demo gva_direct " not in runner


def test_gva_manager_bootstrap_runner_uses_unified_app_entrypoint():
    run_app = (ROOT / "initramfs" / "run_app").read_text()
    dual_runner = (ROOT / "scripts" / "run_ub_dual_node_gsva_manager_bootstrap.sh").read_text()
    four_runner = (ROOT / "scripts" / "run_ub_four_node_gsva_manager_bootstrap.sh").read_text()
    eight_runner = (ROOT / "scripts" / "run_ub_eight_node_gsva_manager_bootstrap.sh").read_text()

    assert "linqu_gva_manager=1" in dual_runner
    assert "gva_manager_mode=bootstrap" in dual_runner
    assert "rdinit=/bin/run_app linqu_gva_manager=1" in dual_runner
    assert "rdinit=/bin/run_demo gva_manager " not in dual_runner
    assert "gva_manager_bootstrap" not in dual_runner
    assert "gva_manager=" in dual_runner
    assert "run_gva_manager" in run_app
    assert "gva_manager_bootstrap)" not in run_app
    assert "gva_manager_dump_routes)" not in run_app
    assert "gva_manager_segment_cli)" not in run_app

    assert "linqu_gva_manager=1" in four_runner
    assert "gva_manager_mode=bootstrap" in four_runner
    assert "rdinit=/bin/run_app linqu_gva_manager=1" in four_runner
    assert "rdinit=/bin/run_demo gva_manager " not in four_runner
    assert "SHARED_DIR=\"${UB_FM_SHARED_DIR:-$ROOT_DIR/out/gsva_manager${GVA_MANAGER_NODE_COUNT}_links_${RANDOM}}\"" in four_runner
    assert "nodeA nodeB nodeC nodeD nodeE nodeF nodeG nodeH" in four_runner
    assert "gva_manager_node_count=${GVA_MANAGER_NODE_COUNT}" in four_runner
    assert "bootstrap hello -> ok peers=$((GVA_MANAGER_NODE_COUNT - 1))" in four_runner
    assert "-qmp unix:" not in four_runner
    assert "gva_manager_bootstrap" not in four_runner
    assert 'GVA_MANAGER_NODE_COUNT="${GVA_MANAGER_NODE_COUNT:-8}"' in eight_runner
    assert "ub_topology_eight_node_full_mesh.ini" in eight_runner
    assert 'UB_SIM_PORT_NUM="${UB_SIM_PORT_NUM:-7}"' in eight_runner
    assert 'exec "$SCRIPT_DIR/run_ub_four_node_gsva_manager_bootstrap.sh" "$@"' in eight_runner
    assert "rdinit=/bin/run_demo gva_manager " not in eight_runner


def test_gva_manager_segment_cli_runner_uses_unified_app_entrypoint():
    run_app = (ROOT / "initramfs" / "run_app").read_text()
    segment_cli_runner = (ROOT / "scripts" / "run_ub_two_node_gva_manager_segment_cli_test.sh").read_text()

    assert "linqu_gva_manager=1" in segment_cli_runner
    assert "gva_manager_mode=segment_cli" in segment_cli_runner
    assert "rdinit=/bin/run_app linqu_gva_manager=1" in segment_cli_runner
    assert "rdinit=/bin/run_demo gva_manager " not in segment_cli_runner
    assert "result=done action=gsva-segment-alloc" in segment_cli_runner
    assert "result=done action=gsva-segment-query" in segment_cli_runner
    assert "result=done action=gsva-segment-retire" in segment_cli_runner
    assert "gva_manager_segment_cli" not in segment_cli_runner
    assert "run_gva_manager_segment_cli" in run_app
    assert "run linqu_gva_manager segment_cli" in run_app
    assert "linqu_gva_manager segment_cli done" in run_app


def test_obmm_queue_uses_canonical_app_source():
    build_script = (ROOT / "scripts" / "build_initramfs.sh").read_text()
    run_app = (ROOT / "initramfs" / "run_app").read_text()
    dual_runner = (ROOT / "scripts" / "run_ub_dual_node_obmm_queue.sh").read_text()
    four_runner = (ROOT / "scripts" / "run_ub_four_node_obmm_queue.sh").read_text()
    eight_runner = (ROOT / "scripts" / "run_ub_eight_node_obmm_queue.sh").read_text()
    app_dir = ROOT / "apps" / "obmm_queue"
    app_source = (app_dir / "obmm_queue.c").read_text()

    assert 'OBMM_QUEUE_SRC="$ROOT_DIR/apps/obmm_queue/obmm_queue.c"' in build_script
    assert 'OBMM_QUEUE_BIN="$OUT_DIR/linqu_ub_obmm_queue"' in build_script
    assert "linqu_ub_obmm_queue_demo" not in build_script
    assert "linqu_obmm_queue=1" in run_app
    assert "linqu_obmm_queue_demo" not in run_app
    assert "obmm_queue_demo" not in run_app
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
    assert "OBMM_DEMO_MODE" not in run_app
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
    run_app = (ROOT / "initramfs" / "run_app").read_text()
    app_matrix_runner = (ROOT / "scripts" / "run_ub_dual_node_apps.sh").read_text()
    dual_runner = (ROOT / "scripts" / "run_ub_dual_node_obmm_pool.sh").read_text()
    four_runner = (ROOT / "scripts" / "run_ub_four_node_obmm_pool.sh").read_text()
    eight_runner = (ROOT / "scripts" / "run_ub_eight_node_obmm_pool.sh").read_text()
    app_dir = ROOT / "apps" / "ub_obmm_pool"
    app_source = (app_dir / "ub_obmm_pool.c").read_text()

    assert 'OBMM_POOL_SRC="$ROOT_DIR/apps/ub_obmm_pool/ub_obmm_pool.c"' in build_script
    assert 'OBMM_POOL_BIN="$OUT_DIR/linqu_ub_obmm_pool"' in build_script
    assert "linqu_ub_obmm_demo" not in build_script
    assert "linqu_obmm_pool=1" in init_source
    assert "linqu_obmm_pool=1" in run_app
    assert "\\\\[ub_obmm_pool\\\\] pass" in app_matrix_runner
    assert "\\\\[ub_obmm_pool\\\\] fail" in app_matrix_runner
    assert "\\[init\\] ub obmm pool app pass" not in app_matrix_runner
    assert "linqu_obmm_demo" not in init_source
    assert "linqu_obmm_demo" not in run_app
    assert "obmm|obmm_pool|obmm_demo" not in run_app
    assert "obmm_demo" not in run_app
    assert "linqu_obmm_demo=1" not in dual_runner
    assert "rdinit=/bin/run_app linqu_obmm_pool=1" in dual_runner
    assert "rdinit=/bin/run_demo obmm_pool " not in dual_runner
    assert "\\[init\\] ub obmm pool app pass" not in dual_runner
    assert "\\[init\\] ub obmm pool app fail" not in dual_runner
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


def test_openeuler_super_node_uses_app_mode_cli():
    script = (ROOT / "scripts" / "run-openEuler-simulated-super-node.sh").read_text()
    readme = (ROOT.parents[1] / "README.md").read_text()

    assert "APP_MODE" in script
    assert "DEMO_MODE" not in script
    assert "--app-mode MODE" in script
    assert "--demo MODE" not in script
    assert "--demo)" not in script
    assert "Deprecated alias for --app-mode" not in script
    assert "--app-mode gsva_identity" in script
    assert "app_mode=$APP_MODE" in script
    assert "--app-mode gsva_identity" in readme
    assert "`--app-mode MODE`" in readme


def test_dual_node_apps_uses_canonical_cli_entrypoint():
    readme = (ROOT / "README.md").read_text()
    script = (ROOT / "scripts" / "run_ub_dual_node_apps.sh").read_text()
    init_source = (ROOT / "init.c").read_text()
    run_app = (ROOT / "initramfs" / "run_app").read_text()
    build_script = (ROOT / "scripts" / "build_initramfs.sh").read_text()
    w4_runner = (ROOT / "scripts" / "run_ub_dual_node_w4_guest.sh").read_text()
    w4_four_runner = (ROOT / "scripts" / "run_ub_four_node_w4_guest.sh").read_text()
    w4_eight_runner = (ROOT / "scripts" / "run_ub_eight_node_w4_guest.sh").read_text()
    app_matrix = (ROOT / "scripts" / "run_ub_app_validation_matrix.sh").read_text()
    four_node_smoke = (ROOT / "scripts" / "run_ub_four_node_smoke.sh").read_text()
    launcher_scripts = "\n".join(
        [
            (ROOT / "scripts" / "launch_ub_dual_node_tmux.sh").read_text(),
            (ROOT / "scripts" / "launch_ub_four_node_tmux.sh").read_text(),
            (ROOT / "scripts" / "launch_ub_four_node_headless.sh").read_text(),
            (ROOT / "scripts" / "launch_ub_eight_node_headless.sh").read_text(),
            (ROOT / "scripts" / "run_ub_dual_node_urma_dataplane_workload_test.sh").read_text(),
        ]
    )

    assert 'REPORT_FILE="${REPORT_FILE:-$OUT_DIR/apps_report.latest.txt}"' in script
    assert "scenario=dual-node-apps" in script
    assert "obmm_dataplane_microbench" in script
    assert "dual-node apps validation passed" in script
    assert "\\\\[obmm_dataplane_microbench\\\\] result=done" in script
    assert "\\[init\\] ub obmm dataplane microbench app pass" not in script
    assert "ub_nodeA.apps." in script
    assert "--app NAME" in script
    assert "APP_SELECTION" in script
    assert 'RDINIT="${RDINIT:-/bin/run_app}"' in script
    assert 'flag="linqu_ub_tcp_each_server=1"' in script
    assert 'flag="linqu_ssd_test=1"' in script
    assert 'flag="linqu_ssd_gsva_test=1"' in script
    assert 'flag="linqu_llm_infer=1"' in script
    assert "validate_ssd_test_log" in script
    assert "validate_ssd_gsva_test_log" in script
    assert "validate_w4_guest_log" in script
    assert "\\\\[ssd_test\\\\] verdict=PASS" in script
    assert "\\\\[ssd_gsva_test\\\\]verdict=PASS" in script
    assert "\\\\[w4_guest\\\\] pass" in script
    assert "\\\\[ssd_gsva_test\\\\]SSD GSVA data test suite" in script
    assert "linqu_node_idx=0 linqu_node_count=2" in script
    assert "linqu_node_idx=1 linqu_node_count=2" in script
    assert "linqu_w4_node_count=2" in script
    assert "sim_uapi_w4_chipbackend_profile=${SIM_UAPI_W4_CHIPBACKEND_PROFILE}" in script
    assert "SIMPLER_HOST_MATMUL_MANIFEST" in script
    assert "SIM_UAPI_SCENARIO_CONFIG" in script
    assert "ensure_simpler_host_manifest" in script
    assert 'append_cmdline_if_missing "pmd_mapping=100%"' in script
    assert 'append_cmdline_if_missing "mem_service_region_size_mb=512"' in script
    assert 'append_cmdline_if_missing "obmm.mempool_size=512M"' in script
    assert 'RUN_APP_SRC="$ROOT_DIR/initramfs/run_app"' in build_script
    assert 'RUN_APP_BIN="$INITRAMFS_DIR/bin/run_app"' in build_script
    assert "write_signature_line \"run_app_src\"" in build_script
    assert 'RUN_DEMO_SRC="$ROOT_DIR/initramfs/run_demo"' not in build_script
    assert 'RUN_DEMO_BIN="$INITRAMFS_DIR/bin/run_demo"' not in build_script
    assert "write_signature_line \"run_demo_src\"" not in build_script
    assert not (ROOT / "initramfs" / "run_demo").exists()
    assert "run_app|run_demo)" not in (ROOT / "initramfs" / "init").read_text()
    assert "run_demo)" not in (ROOT / "initramfs" / "init").read_text()
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
    assert "linqu_llm_infer=1" in run_app
    assert "run_llm_infer" in run_app
    assert "LINQU_MEM_SERVICE_CLUSTER=1" in run_app
    assert "SIM_UAPI_W4_CHIPBACKEND_PROFILE" in run_app
    assert "switching to /bin/run_app app boot flow" in init_source
    assert "execv(\"/bin/run_app\"" in init_source
    assert "bool defer_app_boot_flow = should_enter_app_boot_flow()" in init_source
    assert "!defer_app_boot_flow && should_run_obmm_gsva()" in init_source
    assert "!defer_app_boot_flow && should_run_gva_direct()" in init_source
    assert "!defer_app_boot_flow && should_run_obmm_queue()" in init_source
    assert "UB_RUN_DEMO_FROM_INIT" not in init_source
    assert "UB_RUN_DEMO_FROM_INIT" not in w4_eight_runner
    assert "`run_app` is copied to `/bin/run_app`" in readme
    assert "`run_demo` is copied only as a compatibility wrapper" not in readme
    assert "/bin/run_demo" not in readme
    assert 'local runner="$RUN_INITRAMFS_DIR/bin/run_app"' in w4_eight_runner
    assert 'RDINIT="/bin/run_app"' in w4_eight_runner
    assert 'exec "$SCRIPT_DIR/run_ub_dual_node_apps.sh" --app llm_infer "$@"' in w4_runner
    assert 'RUN_SECS="${RUN_SECS:-300}"' in w4_runner
    assert "mem_service_region_size_mb=512" in w4_runner
    assert "obmm.mempool_size=512M" in w4_runner
    assert "linqu_llm_infer=1" in w4_runner or "--app llm_infer" in w4_runner
    assert 'RDINIT="${RDINIT:-/bin/run_app}"' in launcher_scripts
    assert "/bin/run_demo bootstrap" not in launcher_scripts
    assert 'RDINIT="${RDINIT:-/bin/run_demo}"' not in launcher_scripts
    assert "should_enter_app_boot_flow" in init_source
    assert "should_enter_demo_boot_flow" not in init_source
    assert "no demo flags matched" not in run_app
    assert "run_ub_dual_node_apps.sh" in w4_runner
    assert "run_ub_eight_node_w4_guest_qwen3_0_6b_2step.sh" in app_matrix
    assert "run_w4_app" in w4_four_runner
    assert "run_w4_demo" not in w4_four_runner
    assert "run_w4_app" in w4_eight_runner
    assert "run_w4_demo" not in w4_eight_runner
    assert 'APP_WAIT_SECS="${APP_WAIT_SECS:-300}"' in w4_four_runner
    assert 'APP_WAIT_SECS="${APP_WAIT_SECS:-600}"' in w4_eight_runner
    assert "DEMO_WAIT_SECS" not in w4_four_runner
    assert "DEMO_WAIT_SECS" not in w4_eight_runner
    assert "$APP_WAIT_SECS" in w4_four_runner
    assert "APP_WAIT_SECS * SIM_QWEN3_GUEST_DECODE_STEPS" in w4_eight_runner
    assert "switching to /bin/run_app app boot flow" in four_node_smoke
    assert "switching to /bin/run_demo app boot flow" not in four_node_smoke
    assert "linqu_w4_demo" not in w4_runner
    assert "run_ub_dual_node_demo.sh" not in w4_runner
    assert not (ROOT / "scripts" / "run_ub_dual_node_demo.sh").exists()


def test_guest_scripts_wait_for_run_app_ready_marker():
    scripts_dir = ROOT / "scripts"
    scripts = "\n".join(path.read_text() for path in sorted(scripts_dir.glob("*.sh")))

    assert "[run_demo] boot flow completed, dropping to shell" not in scripts
    assert "\\[run_demo\\] boot flow completed, dropping to shell" not in scripts
    assert "/bin/run_demo" not in scripts
    assert "run_demo_src" not in scripts
    assert "[run_app] entering interactive shell" in scripts
    assert "DEMO_WAIT_SECS" not in scripts


def test_eight_node_matrix_runners_use_headless_serial_sockets():
    runner_names = [
        "run_ub_eight_node_chat_matrix.sh",
        "run_ub_eight_node_rpc_matrix.sh",
        "run_ub_eight_node_udma_matrix.sh",
        "run_ub_eight_node_tcp_each_server_matrix.sh",
        "run_ub_eight_node_obmm_dataplane_microbench.sh",
        "run_ub_eight_node_obmm_pool.sh",
        "run_ub_eight_node_obmm_queue.sh",
        "run_ub_eight_node_obmm_import_stress.sh",
        "run_ub_eight_node_gsva_query_caps.sh",
        "run_ub_eight_node_gva_direct_test.sh",
        "run_ub_eight_node_npu_test.sh",
        "run_ub_eight_node_ssd_test.sh",
    ]

    for runner_name in runner_names:
        runner = (ROOT / "scripts" / runner_name).read_text()

        assert "node_serial_endpoint()" in runner
        assert "NODEA_SERIAL_SOCKET" in runner
        assert "socket.AF_UNIX" in runner
        assert "connect_arg = endpoint" in runner
        assert 's.connect(("127.0.0.1"' not in runner


def test_shared_obmm_helpers_use_app_language():
    common = (ROOT / "common" / "obmm_common.h").read_text()
    queue = (ROOT / "libs" / "obmm_queue" / "obmm_spsc_queue.h").read_text()
    uburma_compat = (ROOT / "uburma_cmd_user_compat.h").read_text()

    combined = common + "\n" + queue + "\n" + uburma_compat
    assert "shared across demos" not in combined
    assert "demo-specific" not in combined
    assert "user-space demo" not in combined


def test_source_tree_does_not_track_app_build_outputs_or_demo_ignores():
    repo_root = ROOT.parents[1]
    gitignore = (repo_root / ".gitignore").read_text()
    docs_files = [
        str(path.relative_to(repo_root))
        for path in sorted((repo_root / "docs").rglob("*"))
        if path.is_file()
    ]
    tracked_apps = subprocess.run(
        ["git", "ls-files", "guest-linux/aarch64/apps"],
        cwd=repo_root,
        check=True,
        text=True,
        capture_output=True,
    ).stdout.splitlines()
    tracked_runtime_source = subprocess.run(
        [
            "git",
            "ls-files",
            ".gitignore",
            "guest-linux/aarch64/apps",
            "guest-linux/aarch64/common",
            "guest-linux/aarch64/components",
            "guest-linux/aarch64/initramfs",
            "guest-linux/aarch64/libs",
            "guest-linux/aarch64/scripts",
        ],
        cwd=repo_root,
        check=True,
        text=True,
        capture_output=True,
    ).stdout.splitlines()

    assert "obmm_queue_demo" not in gitignore
    assert [path for path in docs_files if "demo" in path.lower()] == []
    assert "guest-linux/aarch64/apps/obmm_coh_test/obmm_coh_test" not in tracked_apps
    assert [path for path in tracked_runtime_source if "demo" in path.lower()] == []
    allowed_mem_service_release_artifacts = {
        "guest-linux/aarch64/apps/mem_service/configs/mem_service.conf.schema",
        "guest-linux/aarch64/apps/mem_service/configs/mem_service.example.conf",
        "guest-linux/aarch64/apps/mem_service/deploy/linqu_mem_service.service",
        "guest-linux/aarch64/apps/mem_service/compat-baseline-v1.txt",
        "guest-linux/aarch64/apps/mem_service/compat-matrix.txt",
        "guest-linux/aarch64/apps/mem_service/compat-old-new-matrix.txt",
        "guest-linux/aarch64/apps/mem_service/release-manifest.txt",
        "guest-linux/aarch64/apps/mem_service/wire-schema.txt",
    }
    assert [
        path
        for path in tracked_apps
        if path not in allowed_mem_service_release_artifacts
        and Path(path).name != "Makefile"
        and Path(path).suffix not in {".c", ".h", ".md"}
    ] == []
