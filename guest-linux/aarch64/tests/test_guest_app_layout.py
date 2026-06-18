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
        "scripts/run_ub_dual_node_obmm_dataplane_microbench.sh",
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
        "scripts/run_ub_gsva_query_caps_test.sh",
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
    "w4_guest": [
        "scripts/run_ub_dual_node_w4_guest.sh",
        "scripts/run_ub_eight_node_w4_guest.sh",
    ],
}


def test_apps_readme_lists_reusable_validation_command_for_each_app():
    readme = (ROOT / "apps" / "README.md").read_text()
    app_dirs = sorted(path.name for path in (ROOT / "apps").iterdir() if path.is_dir())

    assert app_dirs == sorted(APP_VALIDATION_COMMANDS)
    assert "/bin/run_demo" not in readme
    assert "DEMO_" not in readme
    assert "scripts/run_ub_app_build_matrix.sh" in readme
    assert "scripts/run_ub_app_validation_matrix.sh" in readme
    assert "scripts/run_w5_cluster_config.sh" in readme
    assert "components/w5_mem_service" in readme
    for app, commands in APP_VALIDATION_COMMANDS.items():
        assert f"`{app}`" in readme
        assert (ROOT / "apps" / app / "Makefile").exists()
        for command in commands:
            script = command.split()[0]
            assert command in readme
            assert (ROOT / script).exists()


def test_app_validation_matrix_runner_matches_readme_commands():
    runner = (ROOT / "scripts" / "run_ub_app_validation_matrix.sh").read_text()

    assert "W5_ENTRY=\"w5_inference_cluster|scripts/run_w5_cluster_config.sh\"" in runner
    assert "--scope 2-node|8-node|all|w5|all-with-w5" in runner
    assert "--dry-run" in runner
    assert "--from APP" in runner
    assert "--resume" in runner
    assert "--status-file PATH" in runner
    assert "STATUS_FILE=" in runner
    assert "status_has_pass" in runner
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
    assert "w5_inference_cluster 8-node=scripts/run_w5_cluster_config.sh" in list_result.stdout
    assert "cmd=scripts/run_ub_dual_node_chat.sh" in dry_run_result.stdout
    assert "cmd=scripts/run_ub_eight_node_chat_matrix.sh" in dry_run_result.stdout
    assert "cmd=scripts/run_w5_cluster_config.sh" in w5_result.stdout
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
    assert "w4_guest makefile=apps/w4_guest/Makefile" in list_result.stdout
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


def test_w5_mem_service_is_link_time_component():
    build_script = (ROOT / "scripts" / "build_initramfs.sh").read_text()
    run_app = (ROOT / "initramfs" / "run_app").read_text()
    components_readme = (ROOT / "components" / "README.md").read_text()
    component_dir = ROOT / "components" / "w5_mem_service"
    readme = (component_dir / "README.md").read_text()

    assert 'W4_DB_SERVICE_SRC="$ROOT_DIR/components/w5_mem_service/w4_kvcache_db_service.c"' in build_script
    assert '"$W4_GUEST_SRC" "$W4_DB_SERVICE_SRC" -lm -o "$W4_GUEST_BIN"' in build_script
    assert "Components do not install guest binaries directly" in components_readme
    assert "not a standalone app" in readme
    assert "standalone demo" not in readme
    assert 'W5_MEM_SERVICE_BIN=' not in build_script
    assert "linqu_w5_mem_service" not in build_script
    assert "linqu_w5_mem_service" not in run_app
    assert "linqu_w5_mem_service=1" not in run_app
    assert not (ROOT / "apps" / "w5_mem_service").exists()
    assert not (ROOT / "apps" / "w5_mem_service_demo").exists()
    assert "test_w4_db_record_recycling.py" in readme
    assert (component_dir / "w4_kvcache_db_service.c").exists()
    assert (component_dir / "w4_kvcache_db_service.h").exists()
    assert (component_dir / "w4_lingqu_object_service.h").exists()


def test_w4_guest_has_app_local_build_entrypoint():
    app_dir = ROOT / "apps" / "w4_guest"
    makefile = (app_dir / "Makefile").read_text()
    readme = (app_dir / "README.md").read_text()

    assert (app_dir / "w4_guest.c").exists()
    assert (app_dir / "Makefile").exists()
    assert "all: linqu_w4_guest" in makefile
    assert "components/w5_mem_service/w4_kvcache_db_service.c" in makefile
    assert "-I$(ROOT)/libs/obmm_queue" in makefile
    assert "-I$(ROOT)/apps/obmm_queue" in makefile
    assert "$^ -lm -o $@" in makefile
    assert "app-local `Makefile`" in readme
    assert "scripts/build_initramfs.sh" in readme


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
    runner = (ROOT / "scripts" / "run_ub_gsva_query_caps_test.sh").read_text()
    eight_runner = (ROOT / "scripts" / "run_ub_eight_node_gsva_query_caps.sh").read_text()

    assert "rdinit=/bin/run_app linqu_gsva_query=1" in runner
    assert "rdinit=/bin/run_demo gsva_query " not in runner
    assert "/bin/linqu_ub_gsva_query --caps" in eight_runner
    assert "\\\\[gsva_query\\\\] GSVA_QUERY_CAPS" in eight_runner
    assert "caps:.*STRICT_ADDRESS_IDENTITY" in eight_runner
    assert "verdict=PASS" in eight_runner
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
    assert "validate_ssd_test_log" in script
    assert "validate_ssd_gsva_test_log" in script
    assert "\\\\[ssd_test\\\\] verdict=PASS" in script
    assert "\\\\[ssd_gsva_test\\\\]verdict=PASS" in script
    assert "\\\\[ssd_gsva_test\\\\]SSD GSVA data test suite" in script
    assert "linqu_node_idx=0 linqu_node_count=2" in script
    assert "linqu_node_idx=1 linqu_node_count=2" in script
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
    assert 'RDINIT="${RDINIT:-/bin/run_app}"' in launcher_scripts
    assert "/bin/run_demo bootstrap" not in launcher_scripts
    assert 'RDINIT="${RDINIT:-/bin/run_demo}"' not in launcher_scripts
    assert "should_enter_app_boot_flow" in init_source
    assert "should_enter_demo_boot_flow" not in init_source
    assert "no demo flags matched" not in run_app
    assert "run_ub_dual_node_apps.sh" in w4_runner
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
    assert [
        path
        for path in tracked_apps
        if Path(path).name != "Makefile"
        and Path(path).suffix not in {".c", ".h", ".md"}
    ] == []
