import os
import re
import subprocess
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
# mem_service sources, CLI app, and release scripts live in the root submodule.
MEM_SERVICE_ROOT = Path(
    os.environ.get("MEM_SERVICE_ROOT", Path(__file__).resolve().parents[3] / "mem_service")
)
# Apps whose Makefile lives outside this repository.
EXTERNAL_APP_DIRS = {"mem_service": MEM_SERVICE_ROOT / "apps" / "mem_service"}


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
    "serving_control": [
        "scripts/run_w5_cluster_config.sh",
    ],
    "pretraining_client": [
        "scripts/run_ub_dual_node_apps.sh --app pretraining_client_mem_service",
    ],
}


def test_apps_readme_lists_reusable_validation_command_for_each_app():
    readme = (ROOT / "apps" / "README.md").read_text()
    app_dirs = sorted(path.name for path in (ROOT / "apps").iterdir() if path.is_dir())

    assert app_dirs == sorted(set(APP_VALIDATION_COMMANDS) - set(EXTERNAL_APP_DIRS))
    assert not any("w4" in app or "w5" in app for app in app_dirs)
    assert "/bin/run_demo" not in readme
    assert "DEMO_" not in readme
    assert "scripts/run_ub_app_build_matrix.sh" in readme
    assert "scripts/run_ub_app_validation_matrix.sh" in readme
    assert "scripts/run_w5_cluster_config.sh" in readme
    assert "components/mem_service" in readme
    for app, commands in APP_VALIDATION_COMMANDS.items():
        assert f"`{app}`" in readme
        makefile_dir = EXTERNAL_APP_DIRS.get(app, ROOT / "apps" / app)
        assert (makefile_dir / "Makefile").exists()
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
        if len(commands) == 1:
            assert f"\"{app}|{commands[0]}|\"" in runner
            assert "status=N/A" in runner
        else:
            assert f"\"{app}|{commands[0]}|{commands[1]}\"" in runner


def test_w5_container_dependency_helper_is_documented_and_dry_runnable():
    helper = ROOT / "scripts" / "prepare_w5_container_deps.sh"
    container_entry = ROOT / "scripts" / "run_w5_in_container.sh"
    manual_doc = (ROOT.parents[1] / "docs" / "w5_manual_serving_run.md").read_text()
    script_inventory = (ROOT.parents[1] / "docs" / "w5_script_inventory.md").read_text()
    macos_env = ROOT.parents[1] / "w5.macos.env"
    flash_env = ROOT.parents[1] / "w5.deepseek-v4-flash.env"
    flash_simpler_env = ROOT.parents[1] / "w5.deepseek-v4-flash-simpler.env"
    flash_official_env = ROOT.parents[1] / "w5.deepseek-v4-flash-official.env"

    assert helper.exists()
    assert helper.stat().st_mode & 0o111
    assert container_entry.exists()
    assert container_entry.stat().st_mode & 0o111
    container_entry_text = container_entry.read_text()
    assert "prepare_w5_container_deps.sh" in container_entry_text
    assert 'export UB_SYNC_ARTIFACTS="${UB_SYNC_ARTIFACTS:-0}"' in container_entry_text
    assert "/Volumes/repos/qwen3_mlx_run:/Volumes/repos/qwen3_mlx_run:ro" in container_entry_text
    assert macos_env.exists()
    assert "SIM_QWEN3_DENSE_WEIGHTS_PATH=/Volumes/repos/qwen3_mlx_run/Qwen3-0.6B" in macos_env.read_text()
    assert flash_env.exists()
    flash_env_text = flash_env.read_text()
    assert "SIM_UAPI_W5_PROFILE=deepseek_v4_flash_decode" in flash_env_text
    assert "SIM_UAPI_W4_CHIPBACKEND_PROFILE=deepseek-v4-flash" in flash_env_text
    assert "SIM_QWEN3_DENSE_WEIGHTS_PATH" not in flash_env_text
    assert flash_simpler_env.exists()
    flash_simpler_env_text = flash_simpler_env.read_text()
    assert "SIM_UAPI_W5_PROFILE=deepseek_v4_flash_decode" in flash_simpler_env_text
    assert (
        "SIM_UAPI_W4_CHIPBACKEND_PROFILE=deepseek-v4-flash-simpler"
        in flash_simpler_env_text
    )
    assert "SIM_QWEN3_DENSE_WEIGHTS_PATH" not in flash_simpler_env_text
    assert flash_official_env.exists()
    flash_official_env_text = flash_official_env.read_text()
    assert (
        "SIM_UAPI_W4_CHIPBACKEND_PROFILE=deepseek-v4-flash-official"
        in flash_official_env_text
    )
    assert "SIM_LLM_INFER_PROMPT_TOKEN_IDS=1" in flash_official_env_text
    assert ".gguf" not in flash_official_env_text.lower()
    assert "run_w5_in_container.sh" in manual_doc
    assert "run_w5_in_container.sh" in script_inventory

    result = subprocess.run(
        [str(helper), "--dry-run"],
        check=True,
        capture_output=True,
        text=True,
    )

    assert "python3 -m pip install distlib" in result.stdout
    assert "cpio" in result.stdout
    assert "liburing" in result.stdout
    assert (
        "dnf install -y" in result.stdout
        or "yum install -y" in result.stdout
        or "apt-get install -y" in result.stdout
    )
    assert "[prepare_w5_container_deps] ready" in result.stdout

    entry_result = subprocess.run(
        [str(container_entry), "--dry-run", "w5.env"],
        check=True,
        capture_output=True,
        text=True,
    )

    if subprocess.check_output(["uname", "-s"], text=True).strip() == "Darwin":
        assert "docker run" not in entry_result.stdout
        assert "run_w5_cluster_config.sh" in entry_result.stdout
    else:
        assert "docker run" in entry_result.stdout
        assert "--privileged" in entry_result.stdout
        assert "--network host" in entry_result.stdout
        assert "openeuler-2403:v0.0.4" in entry_result.stdout
        assert "prepare_w5_container_deps.sh" in entry_result.stdout
        assert "build_qemu_binary.sh" in entry_result.stdout
    assert "run_w5_cluster_config.sh" in entry_result.stdout
    assert "w5.env" in entry_result.stdout


def test_qemu_common_delegates_qemu_freshness_to_build_helper():
    common = (ROOT / "scripts" / "qemu_ub_common.sh").read_text()
    launcher = (ROOT / "scripts" / "launch_ub_eight_node_headless.sh").read_text()
    w5_runner = (ROOT / "scripts" / "run_llm_infer_eight_node_guest.sh").read_text()

    build = 'QEMU_BUILD_JOBS="$jobs" ./scripts/build_qemu_binary.sh >/dev/null'
    assert build in common
    assert 'if [[ -x "$bin" ]] && qemu_ub_supports_required_opts "$bin"; then' not in common
    assert 'if ! QEMU_BIN="$(ensure_qemu_ub_binary "$WORKSPACE_ROOT")"; then' in launcher
    assert 'log "qemu preflight failed"' in launcher
    assert "launch_rc=$?" in w5_runner
    assert 'trace "FAIL: headless launch/preflight failed rc=$launch_rc"' in w5_runner


def test_qemu_build_helper_uses_recorded_macos_qemu_configure_profile():
    builder = (ROOT / "scripts" / "build_qemu_binary.sh").read_text()
    macos_notes = (ROOT.parents[1] / "vendor" / "qemu_8.2.0_ub_macos_build_notes.md").read_text()
    config_status = (
        ROOT.parents[1] / "vendor" / "qemu_8.2.0_ub" / "build" / "config.status"
    )

    reuse_guard = "qemu_build_stamp_matches &&"
    configure = '"$SRC_DIR/configure" --target-list="$TARGET_LIST" ${=CONFIGURE_ARGS}'

    assert reuse_guard in builder
    assert "check_qemu_build_host_deps" not in builder
    assert "missing native QEMU build dependencies" not in builder
    assert 'missing+=("pkg-config")' not in builder
    assert "brew install pkgconf" not in builder
    assert "write_macos_pkg_config_shim" in builder
    assert "using in-tree pkg-config shim for Homebrew .pc files" in builder
    assert "liburing" not in builder
    assert "--enable-fdt=system" not in builder
    assert "--disable-linux-io-uring" not in builder
    assert "setup_macos_build_env" not in builder
    assert "--disable-docs" in builder
    assert "--disable-zstd" in builder
    assert "--extra-ldflags=$SIM_QEMU_STATICLIB" in builder
    assert builder.index(reuse_guard) < builder.index(configure)
    if config_status.exists():
        config_status_text = config_status.read_text()
        for flag in [
            "--target-list=aarch64-softmmu",
            "--disable-werror",
            "--disable-docs",
            "--disable-zstd",
            "--extra-ldflags=/Volumes/repos/ub_sim/target/release/libsim_qemu.a",
        ]:
            assert flag in config_status_text
    for flag in [
        "--disable-vmnet",
        "--disable-coreaudio",
        "--disable-cocoa",
        "--disable-sdl",
        "--disable-gtk",
        "--disable-opengl",
        "--disable-vnc",
        "--disable-tools",
        "--disable-slirp",
        "--disable-linux-user",
        "--disable-bsd-user",
        "--disable-docs",
    ]:
        assert flag in macos_notes


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


def test_initramfs_mem_service_build_includes_ub_ssd_gsva_backend_sources():
    builder = (ROOT / "scripts" / "build_initramfs.sh").read_text()

    assert 'MEM_SERVICE_PROVIDER_SRC="$MEM_SERVICE_ROOT/components/mem_service/mem_service_provider.c"' in builder
    assert 'write_signature_line "mem_service_provider_src" "$MEM_SERVICE_PROVIDER_SRC"' in builder
    assert builder.count('"$MEM_SERVICE_PROVIDER_SRC"') >= 5
    assert 'MEM_SERVICE_GSVA_ACCESS_HDR="$MEM_SERVICE_ROOT/components/mem_service/mem_service_gsva_access.h"' in builder
    assert 'MEM_SERVICE_UB_SSD_GSVA_BACKEND_SRC="$MEM_SERVICE_ROOT/components/mem_service/mem_service_ub_ssd_gsva_backend.c"' in builder
    assert 'MEM_SERVICE_UB_SSD_GSVA_IO_SRC="$MEM_SERVICE_ROOT/components/mem_service/mem_service_ub_ssd_gsva_io.c"' in builder
    assert 'write_signature_line "mem_service_gsva_access_hdr" "$MEM_SERVICE_GSVA_ACCESS_HDR"' in builder
    assert 'write_signature_line "mem_service_ub_ssd_gsva_backend_src" "$MEM_SERVICE_UB_SSD_GSVA_BACKEND_SRC"' in builder
    assert 'write_signature_line "mem_service_ub_ssd_gsva_io_src" "$MEM_SERVICE_UB_SSD_GSVA_IO_SRC"' in builder
    assert builder.count('"$MEM_SERVICE_UB_SSD_GSVA_BACKEND_SRC"') >= 5
    assert builder.count('"$MEM_SERVICE_UB_SSD_GSVA_IO_SRC"') >= 5


def test_serving_control_app_links_ub_ssd_gsva_backend_sources():
    makefile = (ROOT / "apps" / "serving_control" / "Makefile").read_text()

    assert (
        "MEM_SERVICE_PROVIDER := "
        "$(MEM_SERVICE_ROOT)/components/mem_service/mem_service_provider.c"
    ) in makefile
    assert "$(MEM_SERVICE_PROVIDER)" in makefile
    assert (
        "MEM_SERVICE_UB_SSD_GSVA_BACKEND := "
        "$(MEM_SERVICE_ROOT)/components/mem_service/mem_service_ub_ssd_gsva_backend.c"
    ) in makefile
    assert (
        "MEM_SERVICE_UB_SSD_GSVA_IO := "
        "$(MEM_SERVICE_ROOT)/components/mem_service/mem_service_ub_ssd_gsva_io.c"
    ) in makefile
    assert "$(MEM_SERVICE_UB_SSD_GSVA_BACKEND)" in makefile
    assert "$(MEM_SERVICE_UB_SSD_GSVA_IO)" in makefile


def test_serving_control_internal_symbols_are_not_w5_named():
    source = (ROOT / "apps" / "serving_control" / "serving_control.c").read_text()
    object_contract = (
        MEM_SERVICE_ROOT / "components" / "mem_service" / "mem_service_object_contract.h"
    ).read_text()
    obmm_objects = (
        MEM_SERVICE_ROOT / "components" / "mem_service" / "mem_service_obmm_objects.c"
    ).read_text()

    assert "W5_SERVING_CONTROL" not in source
    assert "w5_serving_control_slot" not in source
    assert "w5_serving_checksum" not in source
    assert "MEM_SERVICE_OBMM_KIND_W5_SERVING_REQUEST" not in object_contract
    assert "MEM_SERVICE_OBMM_KIND_W5_SERVING_REQUEST" not in obmm_objects
    assert "MEM_SERVICE_OBMM_KIND_SERVING_REQUEST" in object_contract
    assert "MEM_SERVICE_OBMM_KIND_SERVING_REQUEST" in source
    assert "MEM_SERVICE_OBMM_KIND_SERVING_REQUEST" in obmm_objects


def test_mem_service_gsva_access_is_not_owned_by_ub_ssd_backend():
    gsva_access = (MEM_SERVICE_ROOT / "components/mem_service/mem_service_gsva_access.h").read_text()
    ub_ssd_backend = (
        MEM_SERVICE_ROOT / "components/mem_service/mem_service_ub_ssd_gsva_backend.h"
    ).read_text()
    cluster_runtime = (
        MEM_SERVICE_ROOT / "components/mem_service/mem_service_cluster_runtime.h"
    ).read_text()

    assert "struct mem_service_gsva_desc_source" in gsva_access
    assert "struct mem_service_gsva_buffer_desc" in gsva_access
    assert "mem_service_make_gsva_buffer_desc_from_source" in gsva_access
    assert "uint64_t segment_id;" in gsva_access
    assert "uint64_t home_va;" in gsva_access
    assert "uint64_t region_bytes;" in gsva_access
    assert "uint32_t home_cna;" in gsva_access
    assert "export_mem_id" not in gsva_access
    assert "remote_uba" not in gsva_access
    assert "export_cna" not in gsva_access
    assert "struct mem_service_ub_ssd_gsva_block_ref" in ub_ssd_backend
    assert "struct mem_service_ub_ssd_gsva_desc_source" not in ub_ssd_backend
    assert "struct mem_service_ub_ssd_gsva_buffer_desc" not in ub_ssd_backend
    assert "mem_service_cluster_runtime_make_gsva_buffer_desc" in cluster_runtime
    assert "mem_service_cluster_runtime_make_ub_ssd_gsva_buffer_desc" not in cluster_runtime


def test_mem_service_linux_ops_ci_runner_is_reusable_and_dry_runnable():
    runner_path = MEM_SERVICE_ROOT / "scripts" / "run_mem_service_linux_ops_ci.sh"
    runner = runner_path.read_text()

    assert runner_path.exists()
    assert runner_path.stat().st_mode & 0o111
    assert "--rollback-rpm PATH" in runner
    assert "--preflight" in runner
    assert "PREFLIGHT FAIL" in runner
    assert "OPS_CERTIFICATION_ROLLBACK_RPM=$ROLLBACK_RPM" in runner
    assert "linux-ops-deployment-smoke" in runner
    assert "linux-ops-certification-bundle" in runner
    assert "ops-certification-linux-ci.evidence" in runner
    assert "ops-certification-upgrade-rollback.marker" in runner
    assert "linqu-mem-service-ops-certification-bundle.tar" in runner
    assert "rpmbuild, rpm2cpio, cpio, rpm, curl, promtool" in runner

    missing_arg = subprocess.run(
        [str(runner_path), "--dry-run"],
        cwd=ROOT,
        check=False,
        text=True,
        capture_output=True,
    )
    dry_run = subprocess.run(
        [
            str(runner_path),
            "--rollback-rpm",
            "/tmp/linqu-mem-service-prev.rpm",
            "--out-dir",
            "/tmp/linqu-mem-service-ops",
            "--dry-run",
        ],
        cwd=ROOT,
        check=True,
        text=True,
        capture_output=True,
    )
    preflight = subprocess.run(
        [
            str(runner_path),
            "--rollback-rpm",
            "/tmp/linqu-mem-service-prev.rpm",
            "--out-dir",
            "/tmp/linqu-mem-service-ops",
            "--preflight",
        ],
        cwd=ROOT,
        check=False,
        text=True,
        capture_output=True,
    )

    assert missing_arg.returncode == 2
    assert "--rollback-rpm is required" in missing_arg.stderr
    assert "PACKAGE_OUT_DIR=/tmp/linqu-mem-service-ops" in dry_run.stdout
    assert "OPS_CERTIFICATION_ROLLBACK_RPM=/tmp/linqu-mem-service-prev.rpm" in dry_run.stdout
    assert "linux-ops-deployment-smoke" in dry_run.stdout
    assert "linux-ops-certification-bundle" in dry_run.stdout
    assert preflight.returncode == 1
    assert "PREFLIGHT FAIL" in preflight.stderr
    assert "rollback rpm not readable" in preflight.stderr


def test_mem_service_linux_ops_evidence_verifier_is_reusable_and_dry_runnable():
    verifier_path = MEM_SERVICE_ROOT / "scripts" / "verify_mem_service_linux_ops_evidence.sh"
    verifier = verifier_path.read_text()

    assert verifier_path.exists()
    assert verifier_path.stat().st_mode & 0o111
    assert "--evidence-file PATH" in verifier
    assert "linqu_mem_service_host" in verifier
    assert "libexec/lingqu/mem_service/linqu_mem_service_host" in verifier
    assert "DEFAULT_APP_DIR" in verifier
    assert "ops-certification-verify --evidence-file" in verifier
    assert "[mem-service-linux-ops-evidence] PASS evidence=" in verifier

    missing_arg = subprocess.run(
        [str(verifier_path), "--dry-run"],
        cwd=ROOT,
        check=False,
        text=True,
        capture_output=True,
    )
    dry_run = subprocess.run(
        [
            str(verifier_path),
            "--evidence-file",
            "/tmp/ops-certification-linux-ci.evidence",
            "--dry-run",
        ],
        cwd=ROOT,
        check=True,
        text=True,
        capture_output=True,
    )

    assert missing_arg.returncode == 2
    assert "--evidence-file is required" in missing_arg.stderr
    assert "make -C " in dry_run.stdout
    assert "linqu_mem_service_host" in dry_run.stdout
    assert (
        "ops-certification-verify --evidence-file "
        "/tmp/ops-certification-linux-ci.evidence"
    ) in dry_run.stdout


def test_mem_service_remote_transport_ci_runner_is_reusable_and_dry_runnable():
    runner_path = MEM_SERVICE_ROOT / "scripts" / "run_mem_service_remote_transport_ci.sh"
    runner = runner_path.read_text()

    assert runner_path.exists()
    assert runner_path.stat().st_mode & 0o111
    assert "--source tcp:IP:PORT" in runner
    assert "--producer-host HOST" in runner
    assert "--consumer-host HOST" in runner
    assert "--network-partition-marker PATH" in runner
    assert "--bundle-file PATH" in runner
    assert "--producer-ssh HOST" in runner
    assert "--producer-bin PATH" in runner
    assert "Required with --producer-ssh in source-tree mode" in runner
    assert "--producer-payload-len BYTES" in runner
    assert "--preflight" in runner
    assert "PREFLIGHT FAIL" in runner
    assert "remote-transport-serve-fixture --listen tcp:IP:PORT" in runner
    assert "ssh -o BatchMode=yes -o ConnectTimeout=10" in runner
    assert "\"$PRODUCER_SSH\"" in runner
    assert "\"$producer_bin\" remote-transport-serve-fixture" in runner
    assert "remote-transport-producer.log" in runner
    assert "remote-transport-generate-evidence" in runner
    assert "remote-transport-verify --evidence-file" in runner
    assert "remote-transport-certification-bundle" in runner
    assert "remote-transport-certification-bundle-verify" in runner
    assert "source_address_non_loopback" not in runner
    assert "non-loopback IPv4 address" in runner

    missing_arg = subprocess.run(
        [str(runner_path), "--dry-run"],
        cwd=ROOT,
        check=False,
        text=True,
        capture_output=True,
    )
    dry_run = subprocess.run(
        [
            str(runner_path),
            "--source",
            "tcp:10.0.0.11:9000",
            "--producer-host",
            "producer-a",
            "--consumer-host",
            "consumer-b",
            "--network-partition-marker",
            "/tmp/remote-transport.partition",
            "--evidence-file",
            "/tmp/remote-transport.evidence",
            "--bundle-file",
            "/tmp/linqu-mem-service-remote-transport-bundle.tar",
            "--storage-root",
            "/tmp/remote-transport.storage",
            "--dry-run",
        ],
        cwd=ROOT,
        check=True,
        text=True,
        capture_output=True,
    )
    preflight = subprocess.run(
        [
            str(runner_path),
            "--source",
            "tcp:10.0.0.11:9000",
            "--producer-host",
            "producer-a",
            "--consumer-host",
            "consumer-b",
            "--network-partition-marker",
            "/tmp/remote-transport.partition",
            "--preflight",
        ],
        cwd=ROOT,
        check=False,
        text=True,
        capture_output=True,
    )

    assert missing_arg.returncode == 2
    assert "--source is required" in missing_arg.stderr
    assert "make -C " in dry_run.stdout
    assert (
        "# producer: "
        in dry_run.stdout
        and "remote-transport-serve-fixture --listen tcp:10.0.0.11:9000 --payload-len 4096"
        in dry_run.stdout
    )
    assert "remote-transport-generate-evidence" in dry_run.stdout
    assert "--source tcp:10.0.0.11:9000" in dry_run.stdout
    assert "--producer-host producer-a" in dry_run.stdout
    assert "--consumer-host consumer-b" in dry_run.stdout
    assert "--network-partition-marker /tmp/remote-transport.partition" in dry_run.stdout
    assert "--evidence-file /tmp/remote-transport.evidence" in dry_run.stdout
    assert "REMOTE_TRANSPORT_EVIDENCE=/tmp/remote-transport.evidence" in dry_run.stdout
    assert "REMOTE_TRANSPORT_BUNDLE=/tmp/linqu-mem-service-remote-transport-bundle.tar" in dry_run.stdout
    assert "--storage-root /tmp/remote-transport.storage" in dry_run.stdout
    assert "remote-transport-verify --evidence-file /tmp/remote-transport.evidence" in dry_run.stdout
    assert "remote-transport-certification-bundle remote-transport-certification-bundle-verify" in dry_run.stdout
    ssh_dry_run = subprocess.run(
        [
            str(runner_path),
            "--source",
            "tcp:10.0.0.11:9000",
            "--producer-host",
            "producer-a",
            "--consumer-host",
            "consumer-b",
            "--network-partition-marker",
            "/tmp/remote-transport.partition",
            "--producer-ssh",
            "producer-a",
            "--producer-bin",
            "/usr/libexec/lingqu/mem_service/linqu_mem_service_host",
            "--producer-payload-len",
            "8192",
            "--dry-run",
        ],
        cwd=ROOT,
        check=True,
        text=True,
        capture_output=True,
    )
    missing_producer_bin = subprocess.run(
        [
            str(runner_path),
            "--source",
            "tcp:10.0.0.11:9000",
            "--producer-host",
            "producer-a",
            "--consumer-host",
            "consumer-b",
            "--network-partition-marker",
            "/tmp/remote-transport.partition",
            "--producer-ssh",
            "producer-a",
            "--dry-run",
        ],
        cwd=ROOT,
        check=False,
        text=True,
        capture_output=True,
    )
    assert (
        "ssh -o BatchMode=yes -o ConnectTimeout=10 "
        "producer-a /usr/libexec/lingqu/mem_service/linqu_mem_service_host "
        "remote-transport-serve-fixture --listen tcp:10.0.0.11:9000 --payload-len 8192"
        in ssh_dry_run.stdout
    )
    assert missing_producer_bin.returncode == 2
    assert "--producer-bin is required with --producer-ssh in source-tree mode" in missing_producer_bin.stderr
    assert preflight.returncode == 1
    assert "PREFLIGHT FAIL" in preflight.stderr
    assert "network partition marker not readable" in preflight.stderr


def test_mem_service_release_certification_ci_runner_is_reusable_and_dry_runnable():
    runner_path = MEM_SERVICE_ROOT / "scripts" / "run_mem_service_release_certification_ci.sh"
    runner = runner_path.read_text()

    assert runner_path.exists()
    assert runner_path.stat().st_mode & 0o111
    assert "--rollback-rpm PATH" in runner
    assert "--source tcp:IP:PORT" in runner
    assert "--producer-host HOST" in runner
    assert "--consumer-host HOST" in runner
    assert "--network-partition-marker PATH" in runner
    assert "--producer-ssh HOST" in runner
    assert "--producer-bin PATH" in runner
    assert "Required with --producer-ssh in source-tree mode" in runner
    assert "--producer-payload-len BYTES" in runner
    assert "--preflight" in runner
    assert "PREFLIGHT FAIL" in runner
    assert "installed-sdk-pkgconfig-smoke installed-sdk-runtime-smoke" in runner
    assert "verify_mem_service_installed_sdk.sh" in runner
    assert "run_mem_service_linux_ops_ci.sh" in runner
    assert "run_mem_service_remote_transport_ci.sh" in runner
    assert "remote_transport_producer_args" in runner
    assert "verify_mem_service_release_certification.sh" in runner
    assert "linqu-mem-service-ops-certification-bundle.tar" in runner
    assert "linqu-mem-service-remote-transport-bundle.tar" in runner

    missing_arg = subprocess.run(
        [str(runner_path), "--dry-run"],
        cwd=ROOT,
        check=False,
        text=True,
        capture_output=True,
    )
    dry_run = subprocess.run(
        [
            str(runner_path),
            "--rollback-rpm",
            "/tmp/linqu-mem-service-prev.rpm",
            "--source",
            "tcp:10.0.0.11:9000",
            "--producer-host",
            "producer-a",
            "--consumer-host",
            "consumer-b",
            "--network-partition-marker",
            "/tmp/remote-transport.partition",
            "--out-dir",
            "/tmp/linqu-mem-service-release-certification",
            "--dry-run",
        ],
        cwd=ROOT,
        check=True,
        text=True,
        capture_output=True,
    )
    preflight_dry_run = subprocess.run(
        [
            str(runner_path),
            "--rollback-rpm",
            "/tmp/linqu-mem-service-prev.rpm",
            "--source",
            "tcp:10.0.0.11:9000",
            "--producer-host",
            "producer-a",
            "--consumer-host",
            "consumer-b",
            "--network-partition-marker",
            "/tmp/remote-transport.partition",
            "--out-dir",
            "/tmp/linqu-mem-service-release-certification",
            "--preflight",
            "--dry-run",
        ],
        cwd=ROOT,
        check=True,
        text=True,
        capture_output=True,
    )
    preflight = subprocess.run(
        [
            str(runner_path),
            "--rollback-rpm",
            "/tmp/linqu-mem-service-prev.rpm",
            "--source",
            "tcp:10.0.0.11:9000",
            "--producer-host",
            "producer-a",
            "--consumer-host",
            "consumer-b",
            "--network-partition-marker",
            "/tmp/remote-transport.partition",
            "--preflight",
        ],
        cwd=ROOT,
        check=False,
        text=True,
        capture_output=True,
    )
    ssh_dry_run = subprocess.run(
        [
            str(runner_path),
            "--rollback-rpm",
            "/tmp/linqu-mem-service-prev.rpm",
            "--source",
            "tcp:10.0.0.11:9000",
            "--producer-host",
            "producer-a",
            "--consumer-host",
            "consumer-b",
            "--network-partition-marker",
            "/tmp/remote-transport.partition",
            "--producer-ssh",
            "producer-a",
            "--producer-bin",
            "/usr/libexec/lingqu/mem_service/linqu_mem_service_host",
            "--producer-payload-len",
            "8192",
            "--out-dir",
            "/tmp/linqu-mem-service-release-certification",
            "--dry-run",
        ],
        cwd=ROOT,
        check=True,
        text=True,
        capture_output=True,
    )
    missing_producer_bin = subprocess.run(
        [
            str(runner_path),
            "--rollback-rpm",
            "/tmp/linqu-mem-service-prev.rpm",
            "--source",
            "tcp:10.0.0.11:9000",
            "--producer-host",
            "producer-a",
            "--consumer-host",
            "consumer-b",
            "--network-partition-marker",
            "/tmp/remote-transport.partition",
            "--producer-ssh",
            "producer-a",
            "--dry-run",
        ],
        cwd=ROOT,
        check=False,
        text=True,
        capture_output=True,
    )

    assert missing_arg.returncode == 2
    assert "--rollback-rpm is required" in missing_arg.stderr
    assert "run_mem_service_linux_ops_ci.sh" in dry_run.stdout
    assert "installed-sdk-pkgconfig-smoke installed-sdk-runtime-smoke" in dry_run.stdout
    assert "--rollback-rpm /tmp/linqu-mem-service-prev.rpm" in dry_run.stdout
    assert "run_mem_service_remote_transport_ci.sh" in dry_run.stdout
    assert "--source tcp:10.0.0.11:9000" in dry_run.stdout
    assert "--producer-host producer-a" in dry_run.stdout
    assert "--consumer-host consumer-b" in dry_run.stdout
    assert "--network-partition-marker /tmp/remote-transport.partition" in dry_run.stdout
    assert "verify_mem_service_release_certification.sh --ops-bundle-file" in dry_run.stdout
    assert "verify_mem_service_ops_certification_bundle.sh --bundle-file" in dry_run.stdout
    assert "verify_mem_service_remote_transport_bundle.sh --bundle-file" in dry_run.stdout
    assert "release-readiness --ops-evidence-file" in dry_run.stdout
    assert "--producer-ssh producer-a" in ssh_dry_run.stdout
    assert "--producer-bin /usr/libexec/lingqu/mem_service/linqu_mem_service_host" in ssh_dry_run.stdout
    assert "--producer-payload-len 8192" in ssh_dry_run.stdout
    assert missing_producer_bin.returncode == 2
    assert "--producer-bin is required with --producer-ssh in source-tree mode" in missing_producer_bin.stderr
    assert "/tmp/linqu-mem-service-release-certification/linux_ops" in dry_run.stdout
    assert "/tmp/linqu-mem-service-release-certification/remote_transport" in dry_run.stdout
    assert "preflight: final release readiness gate" in preflight_dry_run.stdout
    assert "verify_mem_service_release_certification.sh --ops-bundle-file" in preflight_dry_run.stdout
    assert "verify_mem_service_ops_certification_bundle.sh --bundle-file" in preflight_dry_run.stdout
    assert "verify_mem_service_remote_transport_bundle.sh --bundle-file" in preflight_dry_run.stdout
    assert "release-readiness --ops-evidence-file" in preflight_dry_run.stdout
    assert preflight.returncode == 1
    assert "PREFLIGHT FAIL" in preflight.stderr
    assert "rollback rpm not readable" in preflight.stderr


def test_mem_service_remote_transport_evidence_verifier_is_reusable_and_dry_runnable():
    verifier_path = MEM_SERVICE_ROOT / "scripts" / "verify_mem_service_remote_transport_evidence.sh"
    verifier = verifier_path.read_text()

    assert verifier_path.exists()
    assert verifier_path.stat().st_mode & 0o111
    assert "--evidence-file PATH" in verifier
    assert "linqu_mem_service_host" in verifier
    assert "libexec/lingqu/mem_service/linqu_mem_service_host" in verifier
    assert "DEFAULT_APP_DIR" in verifier
    assert "remote-transport-verify --evidence-file" in verifier
    assert "[mem-service-remote-transport-evidence] PASS evidence=" in verifier

    missing_arg = subprocess.run(
        [str(verifier_path), "--dry-run"],
        cwd=ROOT,
        check=False,
        text=True,
        capture_output=True,
    )
    dry_run = subprocess.run(
        [
            str(verifier_path),
            "--evidence-file",
            "/tmp/remote-transport.evidence",
            "--dry-run",
        ],
        cwd=ROOT,
        check=True,
        text=True,
        capture_output=True,
    )

    assert missing_arg.returncode == 2
    assert "--evidence-file is required" in missing_arg.stderr
    assert "make -C " in dry_run.stdout
    assert "linqu_mem_service_host" in dry_run.stdout
    assert (
        "remote-transport-verify --evidence-file /tmp/remote-transport.evidence"
        in dry_run.stdout
    )


def test_mem_service_ops_certification_bundle_verifier_is_reusable_and_dry_runnable():
    verifier_path = MEM_SERVICE_ROOT / "scripts" / "verify_mem_service_ops_certification_bundle.sh"
    verifier = verifier_path.read_text()

    assert verifier_path.exists()
    assert verifier_path.stat().st_mode & 0o111
    assert "--bundle-file PATH" in verifier
    assert "ops-certification-bundle.manifest" in verifier
    assert "ops-certification-linux-ci.evidence" in verifier
    assert "ops-certification-upgrade-rollback.marker" in verifier
    assert "ops-certification-verify --evidence-file" in verifier
    assert "libexec/lingqu/mem_service/linqu_mem_service_host" in verifier
    assert "DEFAULT_APP_DIR" in verifier
    assert "unsafe tar entry" in verifier
    assert "[mem-service-ops-certification-bundle] PASS bundle=" in verifier

    missing_arg = subprocess.run(
        [str(verifier_path), "--dry-run"],
        cwd=ROOT,
        check=False,
        text=True,
        capture_output=True,
    )
    dry_run = subprocess.run(
        [
            str(verifier_path),
            "--bundle-file",
            "/tmp/linqu-mem-service-ops-certification-bundle.tar",
            "--work-dir",
            "/tmp/linqu-mem-service-ops-certification-bundle.verify",
            "--dry-run",
        ],
        cwd=ROOT,
        check=True,
        text=True,
        capture_output=True,
    )

    assert missing_arg.returncode == 2
    assert "--bundle-file is required" in missing_arg.stderr
    assert "tar -tf /tmp/linqu-mem-service-ops-certification-bundle.tar" in dry_run.stdout
    assert "ops-certification-bundle.manifest" in dry_run.stdout
    assert "ops-certification-verify --evidence-file" in dry_run.stdout


def test_mem_service_remote_transport_bundle_verifier_is_reusable_and_dry_runnable():
    verifier_path = MEM_SERVICE_ROOT / "scripts" / "verify_mem_service_remote_transport_bundle.sh"
    verifier = verifier_path.read_text()

    assert verifier_path.exists()
    assert verifier_path.stat().st_mode & 0o111
    assert "--bundle-file PATH" in verifier
    assert "remote-transport-bundle.manifest" in verifier
    assert "remote-transport.evidence" in verifier
    assert "remote-transport-verify --evidence-file" in verifier
    assert "libexec/lingqu/mem_service/linqu_mem_service_host" in verifier
    assert "DEFAULT_APP_DIR" in verifier
    assert "unsafe tar entry" in verifier
    assert "[mem-service-remote-transport-bundle] PASS bundle=" in verifier

    missing_arg = subprocess.run(
        [str(verifier_path), "--dry-run"],
        cwd=ROOT,
        check=False,
        text=True,
        capture_output=True,
    )
    dry_run = subprocess.run(
        [
            str(verifier_path),
            "--bundle-file",
            "/tmp/linqu-mem-service-remote-transport-bundle.tar",
            "--work-dir",
            "/tmp/linqu-mem-service-remote-transport-bundle.verify",
            "--dry-run",
        ],
        cwd=ROOT,
        check=True,
        text=True,
        capture_output=True,
    )

    assert missing_arg.returncode == 2
    assert "--bundle-file is required" in missing_arg.stderr
    assert "tar -tf /tmp/linqu-mem-service-remote-transport-bundle.tar" in dry_run.stdout
    assert "remote-transport-bundle.manifest" in dry_run.stdout
    assert "remote-transport.evidence" in dry_run.stdout
    assert "remote-transport-verify --evidence-file" in dry_run.stdout


def test_mem_service_release_certification_verifier_is_reusable_and_dry_runnable():
    verifier_path = MEM_SERVICE_ROOT / "scripts" / "verify_mem_service_release_certification.sh"
    verifier = verifier_path.read_text()

    assert verifier_path.exists()
    assert verifier_path.stat().st_mode & 0o111
    assert "--ops-bundle-file PATH" in verifier
    assert "--remote-transport-bundle-file PATH" in verifier
    assert "verify_mem_service_ops_certification_bundle.sh" in verifier
    assert "verify_mem_service_remote_transport_bundle.sh" in verifier
    assert "installed libexec binary" in verifier
    assert "source-tree app directory" in verifier
    assert "[mem-service-release-certification] PASS ops_bundle=" in verifier
    assert "readiness=certified" in verifier

    missing_arg = subprocess.run(
        [str(verifier_path), "--dry-run"],
        cwd=ROOT,
        check=False,
        text=True,
        capture_output=True,
    )
    dry_run = subprocess.run(
        [
            str(verifier_path),
            "--ops-bundle-file",
            "/tmp/linqu-mem-service-ops-certification-bundle.tar",
            "--remote-transport-bundle-file",
            "/tmp/linqu-mem-service-remote-transport-bundle.tar",
            "--work-dir",
            "/tmp/linqu-mem-service-release-certification.verify",
            "--dry-run",
        ],
        cwd=ROOT,
        check=True,
        text=True,
        capture_output=True,
    )

    assert missing_arg.returncode == 2
    assert "--ops-bundle-file is required" in missing_arg.stderr
    assert "verify_mem_service_ops_certification_bundle.sh --bundle-file /tmp/linqu-mem-service-ops-certification-bundle.tar" in dry_run.stdout
    assert "verify_mem_service_remote_transport_bundle.sh --bundle-file /tmp/linqu-mem-service-remote-transport-bundle.tar" in dry_run.stdout
    assert "release-readiness --ops-evidence-file" in dry_run.stdout
    assert "/tmp/linqu-mem-service-release-certification.verify/ops" in dry_run.stdout
    assert "/tmp/linqu-mem-service-release-certification.verify/remote-transport" in dry_run.stdout


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
    assert 'MEM_SERVICE_BIN "/bin/linqu_mem_service"' in app_source
    assert "LINGQU_MEM_SERVICE_UB_SSD_GSVA" in app_source


def test_mem_service_has_component_and_cli_entrypoints():
    build_script = (ROOT / "scripts" / "build_initramfs.sh").read_text()
    run_app = (ROOT / "initramfs" / "run_app").read_text()
    components_readme = (ROOT / "components" / "README.md").read_text()
    component_dir = MEM_SERVICE_ROOT / "components" / "mem_service"
    app_dir = MEM_SERVICE_ROOT / "apps" / "mem_service"
    readme = (component_dir / "README.md").read_text()
    app_makefile = (app_dir / "Makefile").read_text()
    app_source = (app_dir / "mem_service.c").read_text()
    release_manifest = (app_dir / "release-manifest.txt").read_text()
    package_manifest = (app_dir / "package-manifest.txt").read_text()
    wire_schema_manifest = (app_dir / "wire-schema.txt").read_text()
    admin_output_schema = (app_dir / "admin-output-schema.txt").read_text()
    upgrade_rollback_policy = (app_dir / "upgrade-rollback-policy.txt").read_text()
    ops_certification_policy = (app_dir / "ops-certification-policy.txt").read_text()
    alert_rules = (
        app_dir / "deploy" / "linqu_mem_service.prometheus-alerts.yml"
    ).read_text()
    api_abi_policy = (app_dir / "api-abi-policy.txt").read_text()
    compat_matrix = (app_dir / "compat-matrix.txt").read_text()
    compat_baseline = (app_dir / "compat-baseline-v1.txt").read_text()
    compat_old_new = (app_dir / "compat-old-new-matrix.txt").read_text()
    config_schema = (app_dir / "configs" / "mem_service.conf.schema").read_text()
    config_example = (app_dir / "configs" / "mem_service.example.conf").read_text()
    config_runtime = (app_dir / "configs" / "mem_service.runtime.conf").read_text()
    config_host_runtime = (
        app_dir / "configs" / "mem_service.host.runtime.conf"
    ).read_text()
    deploy_manifest = (app_dir / "deploy" / "linqu_mem_service.service").read_text()
    host_deploy_manifest = (
        app_dir / "deploy" / "linqu_mem_service.host.service"
    ).read_text()
    serving_example = (app_dir / "examples" / "mem_service_serving_example.c").read_text()
    pretraining_example = (
        app_dir / "examples" / "mem_service_pretraining_example.c"
    ).read_text()

    assert 'MEM_SERVICE_SRC="$MEM_SERVICE_ROOT/components/mem_service/mem_service_module.c"' in build_script
    assert 'MEM_SERVICE_CLUSTER_UTILS_SRC="$MEM_SERVICE_ROOT/components/mem_service/mem_service_cluster_utils.c"' in build_script
    assert 'MEM_SERVICE_CLUSTER_PAYLOAD_SRC="$MEM_SERVICE_ROOT/components/mem_service/mem_service_cluster_payload.c"' in build_script
    assert 'MEM_SERVICE_CLUSTER_READ_SRC="$MEM_SERVICE_ROOT/components/mem_service/mem_service_cluster_read.c"' in build_script
    assert 'MEM_SERVICE_CLUSTER_RUNTIME_SRC="$MEM_SERVICE_ROOT/components/mem_service/mem_service_cluster_runtime.c"' in build_script
    assert 'MEM_SERVICE_CLUSTER_QUEUE_SRC="$MEM_SERVICE_ROOT/components/mem_service/mem_service_cluster_queue.c"' in build_script
    assert 'MEM_SERVICE_CLUSTER_OBSERVE_SRC="$MEM_SERVICE_ROOT/components/mem_service/mem_service_cluster_observe.c"' in build_script
    assert 'MEM_SERVICE_OBMM_OBJECT_FLOW_SRC="$MEM_SERVICE_ROOT/components/mem_service/mem_service_obmm_object_flow.c"' in build_script
    assert 'MEM_SERVICE_METADATA_SRC="$MEM_SERVICE_ROOT/components/mem_service/mem_service_metadata.c"' in build_script
    assert 'MEM_SERVICE_PROVIDER_SRC="$MEM_SERVICE_ROOT/components/mem_service/mem_service_provider.c"' in build_script
    assert 'MEM_SERVICE_DAEMON_SRC="$MEM_SERVICE_ROOT/components/mem_service/mem_service_daemon.c"' in build_script
    assert 'MEM_SERVICE_CLIENT_SRC="$MEM_SERVICE_ROOT/components/mem_service/mem_service_client.c"' in build_script
    assert 'MEM_SERVICE_WIRE_CLIENT_SRC="$MEM_SERVICE_ROOT/components/mem_service/mem_service_wire_client.c"' in build_script
    assert 'MEM_SERVICE_KEYS_SRC="$MEM_SERVICE_ROOT/components/mem_service/mem_service_keys.c"' in build_script
    assert 'MEM_SERVICE_OBJECT_REFS_SRC="$MEM_SERVICE_ROOT/components/mem_service/mem_service_object_refs.c"' in build_script
    assert 'MEM_SERVICE_OBMM_OBJECTS_SRC="$MEM_SERVICE_ROOT/components/mem_service/mem_service_obmm_objects.c"' in build_script
    assert 'MEM_SERVICE_GSVA_ACCESS_HDR="$MEM_SERVICE_ROOT/components/mem_service/mem_service_gsva_access.h"' in build_script
    assert 'MEM_SERVICE_UB_SSD_GSVA_BACKEND_SRC="$MEM_SERVICE_ROOT/components/mem_service/mem_service_ub_ssd_gsva_backend.c"' in build_script
    assert 'MEM_SERVICE_UB_SSD_GSVA_IO_SRC="$MEM_SERVICE_ROOT/components/mem_service/mem_service_ub_ssd_gsva_io.c"' in build_script
    assert 'MEM_SERVICE_RECORDS_SRC="$MEM_SERVICE_ROOT/components/mem_service/mem_service_records.c"' in build_script
    assert 'MEM_SERVICE_QWEN3_RECORDS_SRC="$MEM_SERVICE_ROOT/components/mem_service/mem_service_qwen3_records.c"' in build_script
    assert 'MEM_SERVICE_QWEN3_RUNTIME_SRC="$MEM_SERVICE_ROOT/components/mem_service/mem_service_qwen3_runtime.c"' in build_script
    assert 'MEM_SERVICE_QWEN3_DECODE_BARRIER_SRC="$MEM_SERVICE_ROOT/components/mem_service/mem_service_qwen3_decode_barrier.c"' in build_script
    assert 'MEM_SERVICE_QWEN3_KV_STATE_FLOW_SRC="$MEM_SERVICE_ROOT/components/mem_service/mem_service_qwen3_kv_state_flow.c"' in build_script
    assert 'MEM_SERVICE_QWEN3_TERMINAL_TOKEN_FLOW_SRC="$MEM_SERVICE_ROOT/components/mem_service/mem_service_qwen3_terminal_token_flow.c"' in build_script
    assert 'MEM_SERVICE_QWEN3_RUNTIME_RANGE_WAIT_FLOW_SRC="$MEM_SERVICE_ROOT/components/mem_service/mem_service_qwen3_runtime_range_wait_flow.c"' in build_script
    assert 'MEM_SERVICE_QWEN3_RUNTIME_RANGE_PUBLISH_FLOW_SRC="$MEM_SERVICE_ROOT/components/mem_service/mem_service_qwen3_runtime_range_publish_flow.c"' in build_script
    assert 'MEM_SERVICE_QWEN3_ENGRAM_PUBLISH_FLOW_SRC="$MEM_SERVICE_ROOT/components/mem_service/mem_service_qwen3_engram_publish_flow.c"' in build_script
    assert 'MEM_SERVICE_QWEN3_ENGRAM_WAIT_FLOW_SRC="$MEM_SERVICE_ROOT/components/mem_service/mem_service_qwen3_engram_wait_flow.c"' in build_script
    assert 'MEM_SERVICE_QWEN3_SRC="$MEM_SERVICE_ROOT/components/mem_service/mem_service_qwen3.c"' in build_script
    assert 'MEM_SERVICE_CLI_SRC="$MEM_SERVICE_ROOT/apps/mem_service/mem_service.c"' in build_script
    assert 'MEM_SERVICE_CLI_BIN="$OUT_DIR/linqu_mem_service"' in build_script
    assert 'MEM_SERVICE_QWEN3_CLI_BIN="$OUT_DIR/linqu_mem_service_qwen3"' in build_script
    assert '"$LLM_INFER_APP_SRC" "$MEM_SERVICE_SRC" "$MEM_SERVICE_CLUSTER_UTILS_SRC" "$MEM_SERVICE_CLUSTER_PAYLOAD_SRC" "$MEM_SERVICE_CLUSTER_READ_SRC" "$MEM_SERVICE_CLUSTER_RUNTIME_SRC" "$MEM_SERVICE_CLUSTER_QUEUE_SRC" "$MEM_SERVICE_CLUSTER_OBSERVE_SRC" "$MEM_SERVICE_OBMM_OBJECT_FLOW_SRC" "$MEM_SERVICE_CLIENT_SRC" "$MEM_SERVICE_WIRE_CLIENT_SRC" "$MEM_SERVICE_METADATA_SRC" "$MEM_SERVICE_PROVIDER_SRC" "$MEM_SERVICE_KEYS_SRC" "$MEM_SERVICE_OBJECT_REFS_SRC" "$MEM_SERVICE_OBMM_OBJECTS_SRC" "$MEM_SERVICE_UB_SSD_GSVA_BACKEND_SRC" "$MEM_SERVICE_UB_SSD_GSVA_IO_SRC" "$MEM_SERVICE_RECORDS_SRC" "$MEM_SERVICE_PROFILE_SRC" "$MEM_SERVICE_DEEPSEEK_V4_FLASH_SRC" "$MEM_SERVICE_EXPERT_ROUTE_FLOW_SRC" "$MEM_SERVICE_EXPERT_CACHE_SRC" "$MEM_SERVICE_QWEN3_RECORDS_SRC" "$MEM_SERVICE_QWEN3_RUNTIME_SRC" "$MEM_SERVICE_QWEN3_DECODE_BARRIER_SRC" "$MEM_SERVICE_QWEN3_KV_STATE_FLOW_SRC" "$MEM_SERVICE_QWEN3_TERMINAL_TOKEN_FLOW_SRC" "$MEM_SERVICE_QWEN3_RUNTIME_RANGE_WAIT_FLOW_SRC" "$MEM_SERVICE_QWEN3_RUNTIME_RANGE_PUBLISH_FLOW_SRC" "$MEM_SERVICE_QWEN3_ENGRAM_PUBLISH_FLOW_SRC" "$MEM_SERVICE_QWEN3_ENGRAM_WAIT_FLOW_SRC" "$MEM_SERVICE_QWEN3_SRC" "$LLM_INFER_SRC" -lm -o "$LLM_INFER_APP_BIN"' in build_script
    assert '"$MEM_SERVICE_CLI_SRC" "$MEM_SERVICE_DAEMON_SRC" "$MEM_SERVICE_CLIENT_SRC" "$MEM_SERVICE_WIRE_CLIENT_SRC" "$MEM_SERVICE_METADATA_SRC" "$MEM_SERVICE_PROVIDER_SRC" "$MEM_SERVICE_KEYS_SRC" "$MEM_SERVICE_OBJECT_REFS_SRC" "$MEM_SERVICE_UB_SSD_GSVA_BACKEND_SRC" "$MEM_SERVICE_UB_SSD_GSVA_IO_SRC" "$MEM_SERVICE_RECORDS_SRC" -lm -o "$MEM_SERVICE_CLI_BIN"' in build_script
    assert "-DMEM_SERVICE_ENABLE_QWEN3_INSPECT" in build_script
    assert '"$MEM_SERVICE_CLI_SRC" "$MEM_SERVICE_SRC" "$MEM_SERVICE_CLUSTER_UTILS_SRC" "$MEM_SERVICE_CLUSTER_PAYLOAD_SRC" "$MEM_SERVICE_CLUSTER_READ_SRC" "$MEM_SERVICE_CLUSTER_RUNTIME_SRC" "$MEM_SERVICE_CLUSTER_QUEUE_SRC" "$MEM_SERVICE_CLUSTER_OBSERVE_SRC" "$MEM_SERVICE_OBMM_OBJECT_FLOW_SRC" "$MEM_SERVICE_DAEMON_SRC" "$MEM_SERVICE_CLIENT_SRC" "$MEM_SERVICE_WIRE_CLIENT_SRC" "$MEM_SERVICE_METADATA_SRC" "$MEM_SERVICE_PROVIDER_SRC" "$MEM_SERVICE_KEYS_SRC" "$MEM_SERVICE_OBJECT_REFS_SRC" "$MEM_SERVICE_OBMM_OBJECTS_SRC" "$MEM_SERVICE_UB_SSD_GSVA_BACKEND_SRC" "$MEM_SERVICE_UB_SSD_GSVA_IO_SRC" "$MEM_SERVICE_RECORDS_SRC" "$MEM_SERVICE_PROFILE_SRC" "$MEM_SERVICE_DEEPSEEK_V4_FLASH_SRC" "$MEM_SERVICE_EXPERT_ROUTE_FLOW_SRC" "$MEM_SERVICE_EXPERT_CACHE_SRC" "$MEM_SERVICE_QWEN3_RECORDS_SRC" "$MEM_SERVICE_QWEN3_RUNTIME_SRC" "$MEM_SERVICE_QWEN3_DECODE_BARRIER_SRC" "$MEM_SERVICE_QWEN3_KV_STATE_FLOW_SRC" "$MEM_SERVICE_QWEN3_TERMINAL_TOKEN_FLOW_SRC" "$MEM_SERVICE_QWEN3_RUNTIME_RANGE_WAIT_FLOW_SRC" "$MEM_SERVICE_QWEN3_RUNTIME_RANGE_PUBLISH_FLOW_SRC" "$MEM_SERVICE_QWEN3_ENGRAM_PUBLISH_FLOW_SRC" "$MEM_SERVICE_QWEN3_ENGRAM_WAIT_FLOW_SRC" "$MEM_SERVICE_QWEN3_SRC" "$LLM_INFER_SRC" -lm -o "$MEM_SERVICE_QWEN3_CLI_BIN"' in build_script
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
    assert "linqu_mem_service_runtime_quota_fixtures" in run_app
    assert "/bin/linqu_mem_service runtime-quota-fixtures" in run_app
    assert "linqu_mem_service_retention_fixtures" in run_app
    assert "/bin/linqu_mem_service retention-fixtures" in run_app
    assert "linqu_mem_service_checkpoint_retention_fixtures" in run_app
    assert "/bin/linqu_mem_service checkpoint-retention-fixtures" in run_app
    assert "linqu_mem_service_payload_gc_fixtures" in run_app
    assert "/bin/linqu_mem_service payload-gc-fixtures" in run_app
    assert "linqu_mem_service_record_retention_fixtures" in run_app
    assert "/bin/linqu_mem_service record-retention-fixtures" in run_app
    assert "linqu_mem_service_encryption_fixtures" in run_app
    assert "/bin/linqu_mem_service encryption-fixtures" in run_app
    assert "linqu_mem_service_metrics_export_fixtures" in run_app
    assert "/bin/linqu_mem_service metrics-export-fixtures" in run_app
    assert "linqu_mem_service_collector_fixtures" in run_app
    assert "/bin/linqu_mem_service collector-fixtures" in run_app
    assert "linqu_mem_service_admin_output_fixtures" in run_app
    assert "/bin/linqu_mem_service admin-output-fixtures" in run_app
    assert "linqu_mem_service_upgrade_rollback_fixtures" in run_app
    assert "/bin/linqu_mem_service upgrade-rollback-fixtures" in run_app
    assert "linqu_mem_service_alert_fixtures" in run_app
    assert "/bin/linqu_mem_service alert-fixtures" in run_app
    assert "linqu_mem_service_alert_integration_fixtures" in run_app
    assert "/bin/linqu_mem_service alert-integration-fixtures" in run_app
    assert "linqu_mem_service_ops_certification_fixtures" in run_app
    assert "/bin/linqu_mem_service ops-certification-fixtures" in run_app
    assert "linqu_mem_service_ops_certification_evidence_fixtures" in run_app
    assert "/bin/linqu_mem_service ops-certification-evidence-fixtures" in run_app
    assert "linqu_mem_service_client_retry_fixtures" in run_app
    assert "/bin/linqu_mem_service client-retry-fixtures" in run_app
    assert "linqu_mem_service_api_abi_fixtures" in run_app
    assert "/bin/linqu_mem_service api-abi-fixtures" in run_app
    assert "linqu_mem_service_compat_fixtures" in run_app
    assert "/bin/linqu_mem_service compat-fixtures" in run_app
    assert "linqu_mem_service_compat_baseline_fixtures" in run_app
    assert "/bin/linqu_mem_service compat-baseline-fixtures" in run_app
    assert "linqu_mem_service_compat_old_new_fixtures" in run_app
    assert "/bin/linqu_mem_service compat-old-new-fixtures" in run_app
    assert "linqu_mem_service_package_fixtures" in run_app
    assert "/bin/linqu_mem_service package-fixtures" in run_app
    assert "linqu_mem_service_release_fixtures" in run_app
    assert "/bin/linqu_mem_service release-fixtures" in run_app
    assert (app_dir / "mem_service.c").exists()
    assert (app_dir / "Makefile").exists()
    assert (app_dir / "release-manifest.txt").exists()
    assert (app_dir / "package-manifest.txt").exists()
    assert (app_dir / "wire-schema.txt").exists()
    assert (app_dir / "admin-output-schema.txt").exists()
    assert (app_dir / "upgrade-rollback-policy.txt").exists()
    assert (app_dir / "ops-certification-policy.txt").exists()
    assert (app_dir / "api-abi-policy.txt").exists()
    assert (app_dir / "compat-matrix.txt").exists()
    assert (app_dir / "compat-baseline-v1.txt").exists()
    assert (app_dir / "compat-old-new-matrix.txt").exists()
    assert (app_dir / "configs" / "mem_service.conf.schema").exists()
    assert (app_dir / "configs" / "mem_service.example.conf").exists()
    assert (app_dir / "configs" / "mem_service.runtime.conf").exists()
    assert (app_dir / "configs" / "mem_service.host.runtime.conf").exists()
    assert (app_dir / "deploy" / "linqu_mem_service.service").exists()
    assert (app_dir / "deploy" / "linqu_mem_service.host.service").exists()
    assert (app_dir / "deploy" / "linqu_mem_service.prometheus-alerts.yml").exists()
    assert (app_dir / "examples" / "mem_service_serving_example.c").exists()
    assert (app_dir / "examples" / "mem_service_pretraining_example.c").exists()
    assert "linqu_mem_service_core" in app_makefile
    assert "linqu_mem_service_qwen3" in app_makefile
    assert "-DMEM_SERVICE_ENABLE_QWEN3_INSPECT" in app_makefile
    assert "MEM_SERVICE_RELEASE_MANIFEST := release-manifest.txt" in app_makefile
    assert "MEM_SERVICE_PACKAGE_MANIFEST := package-manifest.txt" in app_makefile
    assert "MEM_SERVICE_WIRE_SCHEMA_MANIFEST := wire-schema.txt" in app_makefile
    assert "MEM_SERVICE_ADMIN_OUTPUT_SCHEMA := admin-output-schema.txt" in app_makefile
    assert "MEM_SERVICE_UPGRADE_ROLLBACK_POLICY := upgrade-rollback-policy.txt" in app_makefile
    assert "MEM_SERVICE_OPS_CERTIFICATION_POLICY := ops-certification-policy.txt" in app_makefile
    assert "MEM_SERVICE_API_ABI_POLICY := api-abi-policy.txt" in app_makefile
    assert "MEM_SERVICE_COMPAT_MATRIX := compat-matrix.txt" in app_makefile
    assert "MEM_SERVICE_COMPAT_BASELINE_V1 := compat-baseline-v1.txt" in app_makefile
    assert "MEM_SERVICE_COMPAT_OLD_NEW_MATRIX := compat-old-new-matrix.txt" in app_makefile
    assert "MEM_SERVICE_CONFIG_SCHEMA := configs/mem_service.conf.schema" in app_makefile
    assert "MEM_SERVICE_CONFIG_EXAMPLE := configs/mem_service.example.conf" in app_makefile
    assert "MEM_SERVICE_CONFIG_RUNTIME := configs/mem_service.runtime.conf" in app_makefile
    assert "MEM_SERVICE_DEPLOY_MANIFEST := deploy/linqu_mem_service.service" in app_makefile
    assert "MEM_SERVICE_HOST_DEPLOY_MANIFEST := deploy/linqu_mem_service.host.service" in app_makefile
    assert "MEM_SERVICE_ALERT_RULES := deploy/linqu_mem_service.prometheus-alerts.yml" in app_makefile
    assert "MEM_SERVICE_PACKAGE_TARBALL_NAME := linqu_mem_service-installed-layout-v1.tar" in app_makefile
    assert "MEM_SERVICE_DEB_NAME := linqu-mem-service" in app_makefile
    assert "MEM_SERVICE_DEB_ARCH ?= arm64" in app_makefile
    assert "MEM_SERVICE_RPM_NAME := linqu-mem-service" in app_makefile
    assert "MEM_SERVICE_RPM_ARCH ?= aarch64" in app_makefile
    assert "MEM_SERVICE_RPM_SPEC := packaging/linqu-mem-service.spec" in app_makefile
    assert "PACKAGE_OUT_DIR ?= $(ROOT)/out/mem_service" in app_makefile
    assert "package-tarball:" in app_makefile
    assert "package-tarball-smoke: package-tarball" in app_makefile
    assert "package-deb:" in app_makefile
    assert "package-deb-smoke: package-deb" in app_makefile
    assert "package-rpm:" in app_makefile
    assert "package-rpm-smoke: package-rpm" in app_makefile
    assert "linux-ops-certification-smoke: package-rpm-smoke linqu_mem_service_host" in app_makefile
    assert "linux-ops-evidence-verify: linqu_mem_service_host" in app_makefile
    assert "linux-ops-certification-bundle: package-rpm-smoke linux-ops-evidence-verify" in app_makefile
    assert "linux-ops-certification-bundle-verify: linqu_mem_service_host" in app_makefile
    assert "verify_mem_service_ops_certification_bundle.sh" in app_makefile
    assert "./linqu_mem_service_host ops-certification-verify --evidence-file" in app_makefile
    assert "bundle_schema=linqu-mem-service-ops-certification-bundle-v1" in app_makefile
    assert "OPS_CERTIFICATION_ROLLBACK_RPM ?=" in app_makefile
    assert "OPS_CERTIFICATION_BUNDLE := $(PACKAGE_OUT_DIR)/linqu-mem-service-ops-certification-bundle.tar" in app_makefile
    assert "remote-transport-evidence-verify: linqu_mem_service_host" in app_makefile
    assert "remote-transport-certification-bundle: remote-transport-evidence-verify" in app_makefile
    assert "remote-transport-certification-bundle-verify: linqu_mem_service_host" in app_makefile
    assert "verify_mem_service_remote_transport_bundle.sh" in app_makefile
    assert "./linqu_mem_service_host remote-transport-verify --evidence-file" in app_makefile
    assert "bundle_schema=linqu-mem-service-remote-transport-bundle-v1" in app_makefile
    assert "REMOTE_TRANSPORT_BUNDLE := $(PACKAGE_OUT_DIR)/linqu-mem-service-remote-transport-bundle.tar" in app_makefile
    assert "release-certification-verify: linqu_mem_service_host" in app_makefile
    assert "verify_mem_service_release_certification.sh" in app_makefile
    assert "--ops-bundle-file $(abspath $(OPS_CERTIFICATION_BUNDLE))" in app_makefile
    assert "--remote-transport-bundle-file $(abspath $(REMOTE_TRANSPORT_BUNDLE))" in app_makefile
    assert "linux-ops-upgrade-rollback-smoke: package-rpm-smoke" in app_makefile
    assert "linux-ops-deployment-smoke: linux-ops-upgrade-rollback-smoke linqu_mem_service_host" in app_makefile
    assert "install: $(MEM_SERVICE_RELEASE_MANIFEST)" in app_makefile
    assert "rm -f linqu_mem_service linqu_mem_service_host" in app_makefile
    assert '$(MAKE) -B linqu_mem_service CC="$(CC)" CFLAGS="$(CFLAGS)"' in app_makefile
    assert (
        '$(MAKE) -B linqu_mem_service_host HOST_CC="$(HOST_CC)" '
        'HOST_CFLAGS="$(HOST_CFLAGS)"'
    ) in app_makefile
    assert "tar -cf $(PACKAGE_TARBALL) -C $(PACKAGE_STAGE_ROOT) usr" in app_makefile
    assert 'f.write(b"!<arch>\\n")' in app_makefile
    assert "^distributable_package_format=tar$$" in app_makefile
    assert "^distributable_package_gate=package-tarball-smoke$$" in app_makefile
    assert "$(PACKAGE_VERIFY_ROOT)/usr/share/lingqu/mem_service/scripts/run_mem_service_release_certification_ci.sh" in app_makefile
    assert "^native_package_format=deb$$" in app_makefile
    assert "^native_package_gate=package-deb-smoke$$" in app_makefile
    assert "$(DEB_VERIFY_ROOT)/data/usr/share/lingqu/mem_service/scripts/verify_mem_service_installed_layout.sh --no-runtime" in app_makefile
    assert "$(DEB_VERIFY_ROOT)/data/usr/share/lingqu/mem_service/scripts/run_mem_service_release_certification_ci.sh" in app_makefile
    assert "^rpm_native_package_format=rpm$$" in app_makefile
    assert "^rpm_native_package_gate=package-rpm-smoke$$" in app_makefile
    assert "$(RPM_VERIFY_ROOT)/usr/share/lingqu/mem_service/scripts/run_mem_service_release_certification_ci.sh" in app_makefile
    assert "verify_mem_service_installed_sdk.sh --work-dir /tmp/linqu_mem_service_package_sdk --dry-run" in app_makefile
    assert "verify_mem_service_installed_sdk.sh --work-dir /tmp/linqu_mem_service_deb_sdk --dry-run" in app_makefile
    assert "verify_mem_service_installed_sdk.sh --work-dir /tmp/linqu_mem_service_rpm_sdk --dry-run" in app_makefile
    assert "mem_service_serving_example unix:/tmp/linqu_mem_service_package_sdk/mem_service.sock" in app_makefile
    assert "mem_service_pretraining_example unix:/tmp/linqu_mem_service_package_sdk/mem_service.sock" in app_makefile
    assert "--preflight --dry-run | grep -q 'release-readiness --ops-evidence-file'" in app_makefile
    assert app_makefile.count("| grep -q 'release-readiness --ops-evidence-file'") >= 8
    assert "MEM_SERVICE_CLIENT_EXAMPLES :=" in app_makefile
    assert "examples/mem_service_serving_example.c" in app_makefile
    assert "examples/mem_service_pretraining_example.c" in app_makefile
    assert "INSTALL_EXAMPLEDIR := $(INSTALL_DATADIR)/examples" in app_makefile
    assert "INSTALL_CONFIGDIR := $(INSTALL_DATADIR)/config" in app_makefile
    assert "INSTALL_DEPLOYDIR := $(INSTALL_DATADIR)/deploy" in app_makefile
    assert "INSTALL_HOSTDIR := $(DESTDIR)$(PREFIX)/libexec/lingqu/mem_service" in app_makefile
    assert "SYSCONFDIR ?= /etc" in app_makefile
    assert "INSTALL_SYSCONFDIR := $(DESTDIR)$(SYSCONFDIR)/lingqu/mem_service" in app_makefile
    assert "SYSTEMDUNITDIR ?= /usr/lib/systemd/system" in app_makefile
    assert "INSTALL_SYSTEMDUNITDIR := $(DESTDIR)$(SYSTEMDUNITDIR)" in app_makefile
    assert "linqu_mem_service_host: $(MEM_SERVICE_CORE_SRCS)" in app_makefile
    assert "host-artifact-smoke: linqu_mem_service_host" in app_makefile
    assert "./linqu_mem_service_host upgrade-rollback-runtime-fixtures" in app_makefile
    assert "./linqu_mem_service_host runtime-quota-fixtures" in app_makefile
    assert "./linqu_mem_service_host compat-runtime-fixtures" in app_makefile
    assert "MEM_SERVICE_PUBLIC_HEADERS :=" in app_makefile
    assert "$(ROOT)/components/mem_service/mem_service_client.h" in app_makefile
    assert "$(ROOT)/components/mem_service/mem_service_wire_schema.h" in app_makefile
    assert "MEM_SERVICE_CLIENT_SDK_SRCS :=" in app_makefile
    assert "$(ROOT)/components/mem_service/mem_service_client.c" in app_makefile
    assert "$(ROOT)/components/mem_service/mem_service_wire_client.c" in app_makefile
    assert "$(ROOT)/components/mem_service/mem_service_provider.c" in app_makefile
    assert "MEM_SERVICE_PROVIDER_SDK_SRCS :=" in app_makefile
    assert (
        "$(ROOT)/components/mem_service/providers/mem_service_provider_roce.c"
        in app_makefile
    )
    assert "$(MEM_SERVICE_CONFIG_SCHEMA)" in app_makefile
    assert "$(MEM_SERVICE_CONFIG_EXAMPLE)" in app_makefile
    assert "$(MEM_SERVICE_CONFIG_RUNTIME)" in app_makefile
    assert "$(MEM_SERVICE_CONFIG_HOST_RUNTIME)" in app_makefile
    assert "$(MEM_SERVICE_DEPLOY_MANIFEST)" in app_makefile
    assert "$(MEM_SERVICE_HOST_DEPLOY_MANIFEST)" in app_makefile
    assert "MEM_SERVICE_RELEASE_SCRIPTS :=" in app_makefile
    assert "$(ROOT)/scripts/verify_mem_service_installed_layout.sh" in app_makefile
    assert "$(ROOT)/scripts/verify_mem_service_installed_sdk.sh" in app_makefile
    assert "$(ROOT)/scripts/run_mem_service_linux_ops_ci.sh" in app_makefile
    assert "$(ROOT)/scripts/verify_mem_service_release_certification.sh" in app_makefile
    assert "$(ROOT)/scripts/run_mem_service_release_certification_ci.sh" in app_makefile
    assert "INSTALL_SCRIPTSDIR := $(INSTALL_DATADIR)/scripts" in app_makefile
    assert "INSTALL_PKGCONFIGDIR := $(DESTDIR)$(PREFIX)/lib/pkgconfig" in app_makefile
    assert "$(INSTALL_PKGCONFIGDIR)/$(MEM_SERVICE_PKGCONFIG_NAME)" in app_makefile
    assert (
        "sdk_sources=$${sourcedir}/mem_service_client.c "
        "$${sourcedir}/mem_service_wire_client.c "
        "$${sourcedir}/mem_service_provider.c"
    ) in app_makefile
    assert (
        "payload_provider_roce_sources="
        "$${sourcedir}/mem_service_provider_roce.c"
        in app_makefile
    )
    assert "payload_provider_roce_libs=-lrdmacm -libverbs" in app_makefile
    assert "cp $(MEM_SERVICE_RELEASE_SCRIPTS) $(INSTALL_SCRIPTSDIR)/" in app_makefile
    assert "test -x $(INSTALL_SCRIPTSDIR)/verify_mem_service_installed_layout.sh" in app_makefile
    assert "test -x $(INSTALL_SCRIPTSDIR)/verify_mem_service_installed_sdk.sh" in app_makefile
    assert "verify_mem_service_installed_sdk.sh --work-dir /tmp/linqu_mem_service_installed_sdk --dry-run" in app_makefile
    assert "mem_service_serving_example unix:/tmp/linqu_mem_service_installed_sdk/mem_service.sock" in app_makefile
    assert "mem_service_pretraining_example unix:/tmp/linqu_mem_service_installed_sdk/mem_service.sock" in app_makefile
    assert "$(INSTALL_SCRIPTSDIR)/verify_mem_service_installed_layout.sh --no-runtime" in app_makefile
    assert "lingqu-mem-service.pc" in app_makefile
    assert "test -x $(INSTALL_SCRIPTSDIR)/verify_mem_service_release_certification.sh" in app_makefile
    assert "test -x $(INSTALL_SCRIPTSDIR)/run_mem_service_release_certification_ci.sh" in app_makefile
    assert "verify_mem_service_linux_ops_evidence.sh --evidence-file /tmp/linqu_mem_service_ops.evidence --dry-run" in app_makefile
    assert "verify_mem_service_remote_transport_evidence.sh --evidence-file /tmp/linqu_mem_service_remote_transport.evidence --dry-run" in app_makefile
    assert "verify_mem_service_ops_certification_bundle.sh --bundle-file /tmp/linqu_mem_service_ops_bundle.tar --dry-run" in app_makefile
    assert "verify_mem_service_remote_transport_bundle.sh --bundle-file /tmp/linqu_mem_service_remote_transport_bundle.tar --dry-run" in app_makefile
    assert "verify_mem_service_release_certification.sh --ops-bundle-file /tmp/linqu_mem_service_ops_bundle.tar" in app_makefile
    assert "run_mem_service_release_certification_ci.sh --rollback-rpm /tmp/linqu-mem-service-prev.rpm" in app_makefile
    assert "run_mem_service_remote_transport_ci.sh --source tcp:10.0.0.11:9000 --producer-host producer-a --consumer-host consumer-b --network-partition-marker /tmp/remote-transport.partition --producer-ssh producer-a --producer-payload-len 8192 --dry-run" in app_makefile
    assert "run_mem_service_remote_transport_ci.sh --source tcp:10.0.0.11:9000 --producer-host producer-a --consumer-host consumer-b --network-partition-marker /tmp/remote-transport.partition --producer-ssh producer-a --producer-bin /usr/libexec/lingqu/mem_service/linqu_mem_service_host --producer-payload-len 8192 --dry-run" in app_makefile
    assert "producer-a $(PACKAGE_VERIFY_ROOT)/usr/libexec/lingqu/mem_service/linqu_mem_service_host" in app_makefile
    assert "producer-a $(DEB_VERIFY_ROOT)/data/usr/libexec/lingqu/mem_service/linqu_mem_service_host" in app_makefile
    assert "grep -q -- '--producer-ssh producer-a'" in app_makefile
    assert "run_mem_service_release_certification_ci.sh --rollback-rpm /tmp/linqu-mem-service-prev.rpm --source tcp:10.0.0.11:9000 --producer-host producer-a --consumer-host consumer-b --network-partition-marker /tmp/remote-transport.partition --producer-ssh producer-a --producer-bin /usr/libexec/lingqu/mem_service/linqu_mem_service_host --producer-payload-len 8192 --dry-run" in app_makefile
    assert "| grep -q 'ssh -o BatchMode=yes -o ConnectTimeout=10 producer-a'" in app_makefile
    assert "| grep -q 'producer-payload-len 8192'" in app_makefile
    assert "run_mem_service_release_certification_ci.sh --rollback-rpm /tmp/linqu-mem-service-prev.rpm --source tcp:10.0.0.11:9000 --producer-host producer-a --consumer-host consumer-b --network-partition-marker /tmp/remote-transport.partition --preflight --dry-run" in app_makefile
    assert "| grep -q 'release-readiness --ops-evidence-file'" in app_makefile
    assert "$(INSTALL_HOSTDIR)/linqu_mem_service_host ops-certification-verify" in app_makefile
    assert "$(INSTALL_HOSTDIR)/linqu_mem_service_host remote-transport-verify" in app_makefile
    assert "$(INSTALL_HOSTDIR)/linqu_mem_service_host release-readiness | grep -q '^release_certification_verify=" in app_makefile
    assert "$(INSTALL_HOSTDIR)/linqu_mem_service_host release-readiness | grep -q '^release_certification_readiness_gate=" in app_makefile
    verifier = (MEM_SERVICE_ROOT / "scripts" / "verify_mem_service_installed_layout.sh").read_text()
    assert "PKGCONFIG_FILE=" in verifier
    assert "lib/pkgconfig/lingqu-mem-service.pc" in verifier
    assert "^file_class=pkgconfig count=1$" in verifier
    assert "^sdk_sources=[$][{]sourcedir[}]/mem_service_client[.]c" in verifier
    assert "^payload_provider_roce_sources=[$][{]sourcedir[}]" in verifier
    assert "^payload_provider_roce_libs=-lrdmacm -libverbs$" in verifier
    assert "^metrics_export_format=prometheus-text$$" in app_makefile
    assert "^admin_output_schema=share/lingqu/mem_service/admin-output-schema.txt$$" in app_makefile
    assert "^admin_output_schema_checksum=0xef4c77f8$$" in app_makefile
    assert "^admin_output_format=text-kv$$" in app_makefile
    assert "^admin_metric_prefix=lingqu_mem_service_$$" in app_makefile
    assert "^upgrade_rollback_policy=share/lingqu/mem_service/upgrade-rollback-policy.txt$$" in app_makefile
    assert "^package_manifest_checksum=0xcd341bd9$$" in app_makefile
    assert "./linqu_mem_service_host retention-fixtures" in app_makefile
    assert "./linqu_mem_service_host checkpoint-retention-fixtures" in app_makefile
    assert "./linqu_mem_service_host payload-gc-fixtures" in app_makefile
    assert "./linqu_mem_service_host record-retention-fixtures" in app_makefile
    assert "installed-sdk-example-smoke: install" in app_makefile
    assert "installed-sdk-pkgconfig-smoke: install" in app_makefile
    assert "$(PKG_CONFIG) --define-prefix --exists lingqu-mem-service" in app_makefile
    assert "$(PKG_CONFIG) --define-prefix --variable=sdk_sources lingqu-mem-service" in app_makefile
    assert "installed-sdk-runtime-smoke: installed-sdk-example-smoke" in app_makefile
    assert "daemon.restart.stdout" in app_makefile
    assert "daemon.restart.stderr" in app_makefile
    assert "$(INSTALL_EXAMPLEDIR)/mem_service_serving_example.c" in app_makefile
    assert "$(INSTALL_EXAMPLEDIR)/mem_service_pretraining_example.c" in app_makefile
    assert "$(INSTALL_SRCDIR)/mem_service_client.c" in app_makefile
    assert "$(INSTALL_SRCDIR)/mem_service_wire_client.c" in app_makefile
    assert "$(INSTALL_SRCDIR)/mem_service_provider.c" in app_makefile
    assert "$(INSTALL_SRCDIR)/mem_service_provider_roce.c" in app_makefile
    assert "$(INSTALL_PKGCONFIGDIR)/$(MEM_SERVICE_PKGCONFIG_NAME)" in app_makefile
    assert "^installed_sdk_example_smoke=installed-sdk-example-smoke$$" in app_makefile
    assert "^release_readiness_command=release-readiness$$" in app_makefile
    assert "^release_readiness_contract=text-kv$$" in app_makefile
    assert (
        "^release_readiness_evidence_verify=release-readiness --ops-evidence-file --remote-transport-evidence-file$$"
        in app_makefile
    )
    assert "^release_readiness_gate=release-readiness-fixtures$$" in app_makefile
    assert (
        "^installed_sdk_preflight=scripts/verify_mem_service_installed_sdk.sh --preflight$$"
        in app_makefile
    )
    assert (
        "^installed_sdk_preflight_scope="
        "pkg-config-cflags+sdk-sources+examples+host-binary-no-compile$$"
        in app_makefile
    )
    assert "^package_gate=package-fixtures$$" in app_makefile
    assert "^upgrade_rollback_policy_checksum=0x096e86d0$$" in app_makefile
    assert "^upgrade_rollback_runtime_gate=upgrade-rollback-runtime-fixtures$$" in app_makefile
    assert "^compat_runtime_gate=compat-runtime-fixtures$$" in app_makefile
    assert "^serving_fail_closed_matrix=certified$$" in app_makefile
    assert "^pretraining_fail_closed_matrix=certified$$" in app_makefile
    assert "^payload_ownership_matrix=certified$$" in app_makefile
    assert "^payload_ownership_scope=artifact-query-expected-owner$$" in app_makefile
    assert "^payload_ownership_gate=serving-fail-closed-fixtures,pretraining-fail-closed-fixtures$$" in app_makefile
    assert "^wire_payload_typed_binary_format=typed-binary-v1$$" in app_makefile
    assert "^upgrade_policy=current-version-only$$" in app_makefile
    assert "^rollback_policy=current-version-only$$" in app_makefile
    assert "^old_server_runtime_binary=certified$$" in app_makefile
    assert "^alert_rules=share/lingqu/mem_service/deploy/linqu_mem_service.prometheus-alerts.yml$$" in app_makefile
    assert "^alert_rules_checksum=0x05a9245c$$" in app_makefile
    assert "^alert_rule_count=6$$" in app_makefile
    assert "^    expr: increase(lingqu_mem_service_capacity_exceeded_count\\[5m\\]) > 0$$" in app_makefile
    assert "^alert_integration_smoke=alert-integration-fixtures$$" in app_makefile
    assert "^ops_certification_policy=share/lingqu/mem_service/ops-certification-policy.txt$$" in app_makefile
    assert "^ops_certification_gate=ops-certification-fixtures$$" in app_makefile
    assert "^ops_certification_evidence_schema=ops-certification-evidence-v1$$" in app_makefile
    assert "^ops_certification_evidence_gate=ops-certification-evidence-fixtures$$" in app_makefile
    assert "^ops_certification_generate=ops-certification-generate-evidence$$" in app_makefile
    assert "^ops_certification_linux_ci_gate=ops-certification-linux-ci-smoke$$" in app_makefile
    assert "^linux_ops_certification_smoke=linux-ops-certification-smoke$$" in app_makefile
    assert "^linux_ops_evidence_verify=linux-ops-evidence-verify$$" in app_makefile
    assert "^linux_ops_certification_bundle=linux-ops-certification-bundle$$" in app_makefile
    assert "^linux_ops_certification_bundle_verify=linux-ops-certification-bundle-verify$$" in app_makefile
    assert "^release_certification_verify=release-certification-verify$$" in app_makefile
    assert "^release_certification_verify_script=scripts/verify_mem_service_release_certification.sh$$" in app_makefile
    assert "^release_certification_ci=scripts/run_mem_service_release_certification_ci.sh$$" in app_makefile
    assert "^release_certification_preflight=scripts/run_mem_service_release_certification_ci.sh --preflight$$" in app_makefile
    assert (
        "^release_certification_readiness_gate=release-readiness --ops-evidence-file --remote-transport-evidence-file$$"
        in app_makefile
    )
    assert "^linux_ops_ci=scripts/run_mem_service_linux_ops_ci.sh$$" in app_makefile
    assert "^linux_ops_ci_preflight=scripts/run_mem_service_linux_ops_ci.sh --preflight$$" in app_makefile
    assert "^remote_payload_production_transport_ci=scripts/run_mem_service_remote_transport_ci.sh$$" in app_makefile
    assert "^remote_payload_production_transport_ci_preflight=scripts/run_mem_service_remote_transport_ci.sh --preflight$$" in app_makefile
    assert "^linux_ops_upgrade_rollback_smoke=linux-ops-upgrade-rollback-smoke$$" in app_makefile
    assert "^linux_ops_deployment_smoke=linux-ops-deployment-smoke$$" in app_makefile
    assert "^ops_certification_verify=ops-certification-verify --evidence-file$$" in app_makefile
    assert "^real_systemd_environment=not-certified$$" in app_makefile
    assert "^production_collector_alert_environment=not-certified$$" in app_makefile
    assert "^rpm_package=not-certified$$" in app_makefile
    assert "^client_retry_policy=explicit-max-attempts-backoff$$" in app_makefile
    assert "^api_abi_policy=share/lingqu/mem_service/api-abi-policy.txt$$" in app_makefile
    assert "^api_abi_policy_checksum=0xd0cc1392$$" in app_makefile
    assert "^client_api_version=1$$" in app_makefile
    assert "^client_abi_version=1$$" in app_makefile
    assert "^client_record_abi_size=808$$" in app_makefile
    assert "^compat_matrix=share/lingqu/mem_service/compat-matrix.txt$$" in app_makefile
    assert "^compat_matrix_checksum=0xe6d3e50c$$" in app_makefile
    assert "^compat_baseline=share/lingqu/mem_service/compat-baseline-v1.txt$$" in app_makefile
    assert "^compat_baseline_checksum=0xb93a31bc$$" in app_makefile
    assert "^compat_old_new_matrix=share/lingqu/mem_service/compat-old-new-matrix.txt$$" in app_makefile
    assert "^compat_old_new_matrix_checksum=0xbc0e044d$$" in app_makefile
    assert "^host_daemon_binary=libexec/lingqu/mem_service/linqu_mem_service_host$$" in app_makefile
    assert "^host_daemon_artifact_smoke=host-artifact-smoke$$" in app_makefile
    assert "^host_deployment_manifest=share/lingqu/mem_service/deploy/linqu_mem_service.host.service$$" in app_makefile
    assert "^deployment_smoke=deployment-fixtures$$" in app_makefile
    assert "^host_service_manager_smoke=installed-host-service-manager-smoke$$" in app_makefile
    assert "^host_service_manager_lifecycle=host-serve-config-ready-scrape-sigterm$$" in app_makefile
    assert "^service_manager_lifecycle=serve-config-ready-scrape-sigterm$$" in app_makefile
    assert "^service_manager_shutdown=signal-clean-stop$$" in app_makefile
    assert "^durable_backend=snapshot+journal$$" in app_makefile
    assert "^durable_catalog=storage-root-v1$$" in app_makefile
    assert "^durable_catalog_manifest=catalog/manifest.txt$$" in app_makefile
    assert "^payload_block_backend=sealed-local-block-v1,sealed-chunked-block-v1,transport-loopback-block-v1,transport-tcp-block-v1,ub-ssd-gsva-v1$$" in app_makefile
    assert "^remote_payload_block_backend=transport-loopback-block-v1,transport-tcp-block-v1$$" in app_makefile
    assert "^remote_payload_block_backend_gate=remote-block-backend-policy-fixtures$$" in app_makefile
    assert "^remote_payload_block_data_gate=transport-block-fixtures$$" in app_makefile
    assert "^remote_payload_network_transport=tcp-loopback-certified$$" in app_makefile
    assert "^remote_payload_network_transport_gate=network-transport-block-fixtures$$" in app_makefile
    assert "^remote_payload_network_transport_make_gate=network-transport-block-smoke$$" in app_makefile
    assert "^remote_payload_production_network_transport=not-certified$$" in app_makefile
    assert "^remote_payload_production_transport_evidence_schema=remote-transport-evidence-v1$$" in app_makefile
    assert "^remote_payload_production_transport_evidence_gate=remote-transport-evidence-fixtures$$" in app_makefile
    assert "^remote_payload_production_transport_generate=remote-transport-generate-evidence$$" in app_makefile
    assert "^remote_payload_production_transport_verify=remote-transport-verify --evidence-file$$" in app_makefile
    assert "^remote_payload_production_transport_evidence_verify=scripts/verify_mem_service_remote_transport_evidence.sh$$" in app_makefile
    assert "^remote_payload_production_transport_bundle=remote-transport-certification-bundle$$" in app_makefile
    assert "^remote_payload_production_transport_bundle_verify=remote-transport-certification-bundle-verify$$" in app_makefile
    assert "^remote_payload_production_transport_bundle_script=scripts/verify_mem_service_remote_transport_bundle.sh$$" in app_makefile
    assert "^required_gate=remote-transport-evidence-fixtures$$" in app_makefile
    assert "network-transport-block-smoke: linqu_mem_service_host" in app_makefile
    assert "^metrics_listen_config=metrics_listen$$" in app_makefile
    assert "^metrics_http_listener=tcp-ipv4$$" in app_makefile
    assert "^metrics_scrape_path=/metrics$$" in app_makefile
    assert "^metrics_listen=tcp:127.0.0.1:9900$$" in app_makefile
    assert "install-smoke: install" in app_makefile
    assert "print-release-manifest" in app_makefile
    assert "print-package-manifest" in app_makefile
    assert "print-wire-schema" in app_makefile
    assert "print-admin-output-schema" in app_makefile
    assert "print-upgrade-rollback-policy" in app_makefile
    assert "print-ops-certification-policy" in app_makefile
    assert "print-alert-rules" in app_makefile
    assert "print-api-abi-policy" in app_makefile
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
    assert "$(MEM_SERVICE_UB_SSD_GSVA_BACKEND)" in core_sources.group("body")
    assert "$(MEM_SERVICE_UB_SSD_GSVA_IO)" in core_sources.group("body")
    assert '#include "components/mem_service/mem_service_daemon.h"' in app_source
    assert '#include "components/mem_service/mem_service_ub_ssd_gsva_backend.h"' in app_source
    assert '#include "components/mem_service/mem_service_wire_client.h"' in app_source
    assert 'strcmp(argv[1], "wire-fixtures")' in app_source
    assert 'strcmp(argv[1], "wire-schema")' in app_source
    assert 'strcmp(argv[1], "wire-schema-fixtures")' in app_source
    assert 'strcmp(argv[1], "journal-fixtures")' in app_source
    assert 'strcmp(argv[1], "config-fixtures")' in app_source
    assert 'strcmp(argv[1], "metrics-export-fixtures")' in app_source
    assert 'strcmp(argv[1], "collector-fixtures")' in app_source
    assert 'strcmp(argv[1], "deployment-fixtures")' in app_source
    assert 'strcmp(argv[1], "admin-output-schema")' in app_source
    assert 'strcmp(argv[1], "admin-output-fixtures")' in app_source
    assert 'strcmp(argv[1], "upgrade-rollback-policy")' in app_source
    assert 'strcmp(argv[1], "upgrade-rollback-fixtures")' in app_source
    assert 'strcmp(argv[1], "alert-rules")' in app_source
    assert 'strcmp(argv[1], "alert-fixtures")' in app_source
    assert 'strcmp(argv[1], "alert-integration-fixtures")' in app_source
    assert 'strcmp(argv[1], "ops-certification-policy")' in app_source
    assert 'strcmp(argv[1], "ops-certification-fixtures")' in app_source
    assert 'strcmp(argv[1], "ops-certification-evidence-fixtures")' in app_source
    assert 'strcmp(argv[1], "ops-certification-generate-evidence")' in app_source
    assert 'strcmp(argv[1], "ops-certification-linux-ci-smoke")' in app_source
    assert 'strcmp(argv[1], "ops-certification-verify")' in app_source
    assert 'strcmp(argv[1], "durable-catalog-fixtures")' in app_source
    assert 'strcmp(argv[1], "client-retry-fixtures")' in app_source
    assert 'strcmp(argv[1], "api-abi-policy")' in app_source
    assert 'strcmp(argv[1], "api-abi-fixtures")' in app_source
    assert 'strcmp(argv[1], "compat-matrix")' in app_source
    assert 'strcmp(argv[1], "compat-fixtures")' in app_source
    assert 'strcmp(argv[1], "compat-baseline-v1")' in app_source
    assert 'strcmp(argv[1], "compat-baseline-fixtures")' in app_source
    assert 'strcmp(argv[1], "compat-old-new-matrix")' in app_source
    assert 'strcmp(argv[1], "compat-old-new-fixtures")' in app_source
    assert 'strcmp(argv[1], "package-manifest")' in app_source
    assert 'strcmp(argv[1], "package-fixtures")' in app_source
    assert 'strcmp(argv[1], "version")' in app_source
    assert 'strcmp(argv[1], "version-fixtures")' in app_source
    assert 'strcmp(argv[1], "serve")' in app_source
    assert 'option_value(argc, argv, "--config")' in app_source
    assert 'option_value(argc, argv, "--metrics-listen")' in app_source
    assert (
        "mem_service_run_unix_daemon_with_store_metrics_catalog_and_limits"
        in app_source
    )
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
    assert "render_api_abi_policy" in app_source
    assert "run_api_abi_fixture_check" in app_source
    assert "MEM_SERVICE_DEPLOYMENT_SMOKE_VERSION 1U" in app_source
    assert "render_metrics_http_response" in app_source
    assert "run_collector_fixture_check" in app_source
    assert "collector_metric_value_at_least" in app_source
    assert "run_deployment_fixture_check" in app_source
    assert "MEM_SERVICE_COMPAT_MATRIX_EXPECTED_LEN 1979U" in app_source
    assert "MEM_SERVICE_COMPAT_MATRIX_EXPECTED_CHECKSUM 0xe6d3e50cU" in app_source
    assert "MEM_SERVICE_COMPAT_BASELINE_V1_EXPECTED_LEN 1252U" in app_source
    assert "MEM_SERVICE_COMPAT_BASELINE_V1_EXPECTED_CHECKSUM 0xb93a31bcU" in app_source
    assert "MEM_SERVICE_COMPAT_OLD_NEW_MATRIX_EXPECTED_LEN 1734U" in app_source
    assert "MEM_SERVICE_COMPAT_OLD_NEW_MATRIX_EXPECTED_CHECKSUM 0xbc0e044dU" in app_source
    assert 'strcmp(argv[1], "compat-runtime-fixtures")' in app_source
    assert "mem_service_run_compat_runtime_fixture_check" in app_source
    assert 'strcmp(argv[1], "serving-fail-closed-fixtures")' in app_source
    assert "mem_service_run_serving_fail_closed_fixture_check" in app_source
    assert 'strcmp(argv[1], "pretraining-fail-closed-fixtures")' in app_source
    assert "mem_service_run_pretraining_fail_closed_fixture_check" in app_source
    assert 'strcmp(argv[1], "typed-payload-fixtures")' in app_source
    assert "mem_service_run_typed_payload_fixture_check" in app_source
    assert 'strcmp(argv[1], "remote-block-backend-policy-fixtures")' in app_source
    assert 'strcmp(argv[1], "ub-ssd-gsva-descriptor-fixtures")' in app_source
    assert 'strcmp(argv[1], "transport-block-fixtures")' in app_source
    assert 'strcmp(argv[1], "network-transport-block-fixtures")' in app_source
    assert 'strcmp(argv[1], "remote-transport-serve-fixture")' in app_source
    assert "run_remote_block_backend_policy_fixture_check" in app_source
    assert "run_ub_ssd_gsva_descriptor_fixture_check" in app_source
    assert "mem_service_run_transport_block_fixture_check" in app_source
    assert "mem_service_run_network_transport_block_fixture_check" in app_source
    assert "mem_service_run_tcp_payload_fixture_source" in app_source
    assert "run_wire_schema_manifest" in app_source
    assert "run_wire_schema_fixture_check" in app_source
    assert "MEM_SERVICE_WIRE_SCHEMA_MANIFEST_EXPECTED_LEN 12456U" in app_source
    assert "MEM_SERVICE_WIRE_SCHEMA_MANIFEST_EXPECTED_CHECKSUM 0x14a081c9U" in app_source
    assert "MEM_SERVICE_API_ABI_POLICY_EXPECTED_LEN 856U" in app_source
    assert "MEM_SERVICE_API_ABI_POLICY_EXPECTED_CHECKSUM 0xd0cc1392U" in app_source
    assert "MEM_SERVICE_ADMIN_OUTPUT_SCHEMA_EXPECTED_LEN 6925U" in app_source
    assert "MEM_SERVICE_ADMIN_OUTPUT_SCHEMA_EXPECTED_CHECKSUM 0xef4c77f8U" in app_source
    assert "render_admin_output_schema" in app_source
    assert "run_admin_output_fixture_check" in app_source
    assert "MEM_SERVICE_UPGRADE_ROLLBACK_POLICY_EXPECTED_LEN 2143U" in app_source
    assert "MEM_SERVICE_UPGRADE_ROLLBACK_POLICY_EXPECTED_CHECKSUM 0x096e86d0U" in app_source
    assert "render_upgrade_rollback_policy" in app_source
    assert "run_upgrade_rollback_fixture_check" in app_source
    assert "mem_service_run_upgrade_rollback_runtime_fixture_check" in app_source
    assert "MEM_SERVICE_ALERT_RULES_EXPECTED_LEN 2096U" in app_source
    assert "MEM_SERVICE_ALERT_RULES_EXPECTED_CHECKSUM 0x05a9245cU" in app_source
    assert "MEM_SERVICE_OPS_CERTIFICATION_POLICY_EXPECTED_LEN 1118U" in app_source
    assert "MEM_SERVICE_OPS_CERTIFICATION_POLICY_EXPECTED_CHECKSUM 0xe77c644bU" in app_source
    assert "MEM_SERVICE_OPS_CERTIFICATION_EVIDENCE_VERSION 1U" in app_source
    assert "render_ops_certification_policy" in app_source
    assert "run_ops_certification_fixture_check" in app_source
    assert "run_ops_certification_evidence_fixture_check" in app_source
    assert "run_ops_certification_generate_evidence" in app_source
    assert "run_ops_certification_linux_ci_smoke" in app_source
    assert "run_ops_certification_verify" in app_source
    assert "MEM_SERVICE_REMOTE_TRANSPORT_EVIDENCE_VERSION 1U" in app_source
    assert "run_remote_transport_evidence_fixture_check" in app_source
    assert "run_remote_transport_generate_evidence" in app_source
    assert "run_remote_transport_verify" in app_source
    assert "render_alert_rules" in app_source
    assert "run_alert_fixture_check" in app_source
    assert "run_alert_integration_fixture_check" in app_source
    assert "MEM_SERVICE_RELEASE_VERSION \"0.1.0\"" in app_source
    assert "MEM_SERVICE_PACKAGE_MANIFEST_EXPECTED_LEN 9703U" in app_source
    assert "MEM_SERVICE_PACKAGE_MANIFEST_EXPECTED_CHECKSUM 0xcd341bd9U" in app_source
    assert 'strcmp(argv[1], "release-readiness")' in app_source
    assert 'strcmp(argv[1], "release-readiness-fixtures")' in app_source
    assert "render_release_readiness" in app_source
    assert "ops_certification_ci=scripts/run_mem_service_linux_ops_ci.sh" in app_source
    assert (
        "ops_certification_ci_preflight=scripts/run_mem_service_linux_ops_ci.sh --preflight"
        in app_source
    )
    assert "remote_transport_ci=scripts/run_mem_service_remote_transport_ci.sh" in app_source
    assert (
        "remote_transport_ci_preflight=scripts/run_mem_service_remote_transport_ci.sh --preflight"
        in app_source
    )
    assert (
        "release_certification_ci=scripts/run_mem_service_release_certification_ci.sh"
        in app_source
    )
    assert (
        "release_certification_preflight=scripts/run_mem_service_release_certification_ci.sh --preflight"
        in app_source
    )
    assert "run_release_readiness_fixture_check" in app_source
    assert 'strcmp(argv[1], "restore-policy-fixtures")' in app_source
    assert "mem_service_run_restore_policy_fixture_check" in app_source
    assert 'strcmp(argv[1], "runtime-quota-fixtures")' in app_source
    assert "mem_service_run_runtime_quota_fixture_check" in app_source
    assert 'strcmp(argv[1], "retention-fixtures")' in app_source
    assert "mem_service_run_retention_fixture_check" in app_source
    assert 'strcmp(argv[1], "checkpoint-retention-fixtures")' in app_source
    assert "mem_service_run_checkpoint_retention_fixture_check" in app_source
    assert 'strcmp(argv[1], "payload-gc-fixtures")' in app_source
    assert "mem_service_run_payload_gc_fixture_check" in app_source
    assert 'strcmp(argv[1], "record-retention-fixtures")' in app_source
    assert "mem_service_run_record_retention_fixture_check" in app_source
    assert 'strcmp(argv[1], "encryption-policy")' in app_source
    assert 'strcmp(argv[1], "encryption-fixtures")' in app_source
    assert "run_encryption_fixture_check" in app_source
    assert 'append_optional_payload_field(payload, payload_len, argc, argv, "--expected-owner", "expected_owner")' in app_source
    assert 'MEM_SERVICE_NATIVE_RPM_NAME "linqu-mem-service-0.1.0-1.aarch64.rpm"' in app_source
    assert 'MEM_SERVICE_PACKAGE_TARBALL_NAME "linqu_mem_service-installed-layout-v1.tar"' in app_source
    assert 'MEM_SERVICE_NATIVE_DEB_NAME "linqu-mem-service_0.1.0-1_arm64.deb"' in app_source
    assert "render_package_manifest" in app_source
    assert "run_package_fixture_check" in app_source
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
    assert "package_format=installed-layout-v1" in release_manifest
    assert "package_manifest=share/lingqu/mem_service/package-manifest.txt" in release_manifest
    assert "service_version=0.1.0" in release_manifest
    assert "package_manifest_checksum=0xcd341bd9" in release_manifest
    assert "binary_version_command=version" in release_manifest
    assert "binary_version_contract=text-kv" in release_manifest
    assert "binary_version_gate=version-fixtures" in release_manifest
    assert "release_readiness_command=release-readiness" in release_manifest
    assert "release_readiness_contract=text-kv" in release_manifest
    assert (
        "release_readiness_evidence_verify=release-readiness --ops-evidence-file --remote-transport-evidence-file"
        in release_manifest
    )
    assert "release_readiness_gate=release-readiness-fixtures" in release_manifest
    assert "service_auth_boundary=unix-socket-local-only" in release_manifest
    assert "metrics_auth_boundary=loopback-only" in release_manifest
    assert "config_security_gate=config-fixtures" in release_manifest
    assert "deployment_quota_contract=max-records+max-payload-bytes" in release_manifest
    assert "deployment_quota_gate=config-fixtures" in release_manifest
    assert "retention_policy=manual-or-audit-log-limit" in release_manifest
    assert "retention_policy_gate=config-fixtures,retention-fixtures" in release_manifest
    assert "checkpoint_retention_policy=manual-or-latest-limit" in release_manifest
    assert (
        "checkpoint_retention_gate=config-fixtures,checkpoint-retention-fixtures"
        in release_manifest
    )
    assert "record_retention_policy=manual-or-global-kind-tenant-latest-or-ttl" in release_manifest
    assert "record_retention_gate=config-fixtures,record-retention-fixtures" in release_manifest
    assert (
        "payload_block_gc=record-and-checkpoint-retention-orphan-blocks"
        in release_manifest
    )
    assert (
        "payload_block_gc_gate=payload-gc-fixtures,record-retention-fixtures"
        in release_manifest
    )
    assert "encryption_policy=explicit-none-only" in release_manifest
    assert "encryption_at_rest=not-certified" in release_manifest
    assert "encryption_policy_command=encryption-policy" in release_manifest
    assert "encryption_policy_gate=encryption-fixtures" in release_manifest
    assert "runtime_quota_admission=max-records+max-payload-bytes" in release_manifest
    assert "runtime_quota_gate=runtime-quota-fixtures" in release_manifest
    assert "restore_policy=transactional-staged-restore" in release_manifest
    assert "restore_policy_scope=full-snapshot+paged-snapshot" in release_manifest
    assert "restore_policy_gate=restore-policy-fixtures" in release_manifest
    assert (
        "installed_sdk_preflight=scripts/verify_mem_service_installed_sdk.sh --preflight"
        in release_manifest
    )
    assert (
        "installed_sdk_preflight_scope="
        "pkg-config-cflags+sdk-sources+examples+host-binary-no-compile"
        in release_manifest
    )
    assert "installed_sdk_example_smoke=installed-sdk-example-smoke" in release_manifest
    assert "installed_sdk_pkgconfig_smoke=installed-sdk-pkgconfig-smoke" in release_manifest
    assert (
        "installed_sdk_pkgconfig_smoke_scope="
        "pkg-config-cflags+sdk-sources-external-client-compile"
        in release_manifest
    )
    assert "installed_sdk_runtime_smoke=installed-sdk-runtime-smoke" in release_manifest
    assert "installed_sdk_runtime_reuse=installed-sdk-runtime-smoke" in release_manifest
    assert "pkgconfig=lib/pkgconfig/lingqu-mem-service.pc" in release_manifest
    assert "pkgconfig_name=lingqu-mem-service" in release_manifest
    assert "pkgconfig_cflags=-I${includedir}" in release_manifest
    assert (
        "pkgconfig_sdk_sources=${sourcedir}/mem_service_client.c "
        "${sourcedir}/mem_service_wire_client.c "
        "${sourcedir}/mem_service_provider.c"
        in release_manifest
    )
    assert (
        "pkgconfig_payload_provider_roce_sources="
        "${sourcedir}/mem_service_provider_roce.c"
        in release_manifest
    )
    assert (
        "pkgconfig_payload_provider_roce_libs=-lrdmacm -libverbs"
        in release_manifest
    )
    assert (
        "installed_sdk_runtime_smoke_scope=installed-host-daemon+serving+pretraining-runtime"
        in release_manifest
    )
    assert (
        "installed_sdk_runtime_reuse_scope=daemon-restart+durable-store+serving+pretraining"
        in release_manifest
    )
    assert "package_gate=package-fixtures" in release_manifest
    assert (
        "distributable_package=out/mem_service/"
        "linqu_mem_service-installed-layout-v1.tar"
    ) in release_manifest
    assert "distributable_package_format=tar" in release_manifest
    assert "distributable_package_root=usr+etc" in release_manifest
    assert "distributable_package_gate=package-tarball-smoke" in release_manifest
    assert "native_package=out/mem_service/linqu-mem-service_0.1.0-1_arm64.deb" in release_manifest
    assert "native_package_format=deb" in release_manifest
    assert "native_package_arch=arm64" in release_manifest
    assert "native_package_gate=package-deb-smoke" in release_manifest
    assert "native_package_runtime=not-executed-cross-compiled-arm64" in release_manifest
    assert "rpm_native_package=out/mem_service/linqu-mem-service-0.1.0-1.aarch64.rpm" in release_manifest
    assert "rpm_native_package_format=rpm" in release_manifest
    assert "rpm_native_package_arch=aarch64" in release_manifest
    assert "rpm_native_package_gate=package-rpm-smoke" in release_manifest
    assert "rpm_native_package_runtime=requires-linux-rpm-toolchain" in release_manifest
    assert "core_binary=bin/linqu_mem_service" in release_manifest
    assert (
        "host_daemon_binary=libexec/lingqu/mem_service/linqu_mem_service_host"
        in release_manifest
    )
    assert "host_daemon_artifact_smoke=host-artifact-smoke" in release_manifest
    assert "public_header=include/lingqu/mem_service/mem_service_client.h" in release_manifest
    assert "public_header=include/lingqu/mem_service/mem_service_provider.h" in release_manifest
    assert (
        "public_header=include/lingqu/mem_service/mem_service_provider_roce.h"
        in release_manifest
    )
    assert "client_source=src/lingqu/mem_service/mem_service_client.c" in release_manifest
    assert "client_source=src/lingqu/mem_service/mem_service_provider.c" in release_manifest
    assert (
        "provider_source=src/lingqu/mem_service/mem_service_provider_roce.c"
        in release_manifest
    )
    assert (
        "example_source=share/lingqu/mem_service/examples/"
        "mem_service_serving_example.c"
    ) in release_manifest
    assert (
        "example_source=share/lingqu/mem_service/examples/"
        "mem_service_pretraining_example.c"
    ) in release_manifest
    assert "wire_schema_manifest=share/lingqu/mem_service/wire-schema.txt" in release_manifest
    assert "admin_output_schema=share/lingqu/mem_service/admin-output-schema.txt" in release_manifest
    assert "admin_output_schema_checksum=0xef4c77f8" in release_manifest
    assert "admin_output_format=text-kv" in release_manifest
    assert "admin_metric_prefix=lingqu_mem_service_" in release_manifest
    assert "upgrade_rollback_policy=share/lingqu/mem_service/upgrade-rollback-policy.txt" in release_manifest
    assert "upgrade_rollback_policy_checksum=0x096e86d0" in release_manifest
    assert "upgrade_rollback_runtime_gate=upgrade-rollback-runtime-fixtures" in release_manifest
    assert "compat_runtime_gate=compat-runtime-fixtures" in release_manifest
    assert "serving_fail_closed_matrix=certified" in release_manifest
    assert "pretraining_fail_closed_matrix=certified" in release_manifest
    assert "payload_ownership_matrix=certified" in release_manifest
    assert "payload_ownership_scope=artifact-query-expected-owner" in release_manifest
    assert "payload_ownership_gate=serving-fail-closed-fixtures,pretraining-fail-closed-fixtures" in release_manifest
    assert "wire_payload_typed_binary_format=typed-binary-v1" in release_manifest
    assert "upgrade_policy=current-version-only" in release_manifest
    assert "rollback_policy=current-version-only" in release_manifest
    assert "old_server_runtime_binary=certified" in release_manifest
    assert "alert_rules=share/lingqu/mem_service/deploy/linqu_mem_service.prometheus-alerts.yml" in release_manifest
    assert "alert_rules_format=prometheus-rules-yaml" in release_manifest
    assert "alert_rules_checksum=0x05a9245c" in release_manifest
    assert "alert_rule_count=6" in release_manifest
    assert "alert_rules_gate=alert-fixtures" in release_manifest
    assert "alert_integration_smoke=alert-integration-fixtures" in release_manifest
    assert "api_abi_policy=share/lingqu/mem_service/api-abi-policy.txt" in release_manifest
    assert "api_abi_policy_checksum=0xd0cc1392" in release_manifest
    assert "client_api_version=1" in release_manifest
    assert "client_abi_version=1" in release_manifest
    assert "client_record_abi_size=808" in release_manifest
    assert "compat_matrix=share/lingqu/mem_service/compat-matrix.txt" in release_manifest
    assert "compat_matrix_checksum=0xe6d3e50c" in release_manifest
    assert "compat_baseline=share/lingqu/mem_service/compat-baseline-v1.txt" in release_manifest
    assert "compat_baseline_checksum=0xb93a31bc" in release_manifest
    assert "compat_old_new_matrix=share/lingqu/mem_service/compat-old-new-matrix.txt" in release_manifest
    assert "compat_old_new_matrix_checksum=0xbc0e044d" in release_manifest
    assert "deployment_smoke=deployment-fixtures" in release_manifest
    assert "host_deployment_manifest=share/lingqu/mem_service/deploy/linqu_mem_service.host.service" in release_manifest
    assert "systemd_unit=lib/systemd/system/linqu_mem_service.service" in release_manifest
    assert "host_systemd_unit=lib/systemd/system/linqu_mem_service.host.service" in release_manifest
    assert "host_service_manager_smoke=installed-host-service-manager-smoke" in release_manifest
    assert "host_service_manager_lifecycle=host-serve-config-ready-scrape-sigterm" in release_manifest
    assert "service_manager_lifecycle=serve-config-ready-scrape-sigterm" in release_manifest
    assert "service_manager_shutdown=signal-clean-stop" in release_manifest
    assert "durable_backend=snapshot+journal" in release_manifest
    assert "durable_catalog=storage-root-v1" in release_manifest
    assert "durable_catalog_manifest=catalog/manifest.txt" in release_manifest
    assert "payload_block_backend=sealed-local-block-v1,sealed-chunked-block-v1,transport-loopback-block-v1,transport-tcp-block-v1,ub-ssd-gsva-v1" in release_manifest
    assert "remote_payload_block_backend=transport-loopback-block-v1,transport-tcp-block-v1" in release_manifest
    assert "remote_payload_block_backend_gate=remote-block-backend-policy-fixtures" in release_manifest
    assert "remote_payload_block_data_gate=transport-block-fixtures" in release_manifest
    assert "remote_payload_network_transport=tcp-loopback-certified" in release_manifest
    assert "remote_payload_network_transport_gate=network-transport-block-fixtures" in release_manifest
    assert "remote_payload_network_transport_make_gate=network-transport-block-smoke" in release_manifest
    assert "durable_journal=store-path.journal" in release_manifest
    assert "metrics_listen_config=metrics_listen" in release_manifest
    assert "metrics_http_listener=tcp-ipv4" in release_manifest
    assert "metrics_scrape_path=/metrics" in release_manifest
    assert "metrics_http_content_type=text/plain; version=0.0.4" in release_manifest
    assert "mem_service_api_abi_policy_version=1" in api_abi_policy
    assert "client_api_version=1" in api_abi_policy
    assert "client_abi_version=1" in api_abi_policy
    assert "client_record_abi_size=808" in api_abi_policy
    assert "old_client_new_server_policy=compatible-within-v1" in api_abi_policy
    assert (
        "new_client_old_server_policy=certified"
        in api_abi_policy
    )
    assert "mem_service_compat_matrix_version=1" in compat_matrix
    assert "wire_version_current=1" in compat_matrix
    assert "wire_schema_manifest_checksum=0x14a081c9" in compat_matrix
    assert "idempotency_conflict_status=version_conflict" in compat_matrix
    assert "idempotency_persistence=store-journal-and-full-snapshot" in compat_matrix
    assert "audit_log_persistence=store-journal-and-full-snapshot" in compat_matrix
    assert "journal_scope=completed-idempotency-and-audit-events" in compat_matrix
    assert "compat_test=journal-fixtures" in compat_matrix
    assert "compat_test=deployment-fixtures" in compat_matrix
    assert "mem_service_compat_baseline_version=1" in compat_baseline
    assert "old_client_new_server=compatible-within-v1" in compat_baseline
    assert "new_client_old_server=certified" in compat_baseline
    assert "baseline_payload=register_training_artifact:v1-training-step-compatible" in compat_baseline
    assert "mem_service_old_new_compat_matrix_version=1" in compat_old_new
    assert "certified_pair=current-v1-client->old-v1-schema-profile" in compat_old_new
    assert "certified_pair=current-v1-client->old-v1-runtime-binary" in compat_old_new
    assert "case=old-client-current-server:runtime-compatible" in compat_old_new
    assert "case=current-client-current-server:compat-runtime-fixtures" in compat_old_new
    assert "evidence=compat-runtime-fixtures" in compat_old_new
    assert "certification_limit=none" in compat_old_new
    assert "wire_schema_manifest_len=12456" in release_manifest
    assert "wire_schema_manifest_checksum=0x14a081c9" in release_manifest
    assert "admin_output_schema_len=6925" in release_manifest
    assert "admin_output_schema_checksum=0xef4c77f8" in release_manifest
    assert "upgrade_rollback_policy_len=2143" in release_manifest
    assert "upgrade_rollback_policy_checksum=0x096e86d0" in release_manifest
    assert "package_manifest_len=9703" in release_manifest
    assert "package_manifest_checksum=0xcd341bd9" in release_manifest
    assert "release_script_root=share/lingqu/mem_service/scripts" in release_manifest
    assert (
        "release_script=share/lingqu/mem_service/scripts/"
        "verify_mem_service_installed_layout.sh"
        in release_manifest
    )
    assert (
        "release_script=share/lingqu/mem_service/scripts/"
        "verify_mem_service_installed_sdk.sh"
        in release_manifest
    )
    assert (
        "release_script=share/lingqu/mem_service/scripts/"
        "verify_mem_service_release_certification.sh"
        in release_manifest
    )
    assert (
        "release_script=share/lingqu/mem_service/scripts/run_mem_service_linux_ops_ci.sh"
        in release_manifest
    )
    assert (
        "release_script=share/lingqu/mem_service/scripts/"
        "verify_mem_service_ops_certification_bundle.sh"
        in release_manifest
    )
    assert (
        "release_script=share/lingqu/mem_service/scripts/"
        "run_mem_service_remote_transport_ci.sh"
        in release_manifest
    )
    assert (
        "release_script=share/lingqu/mem_service/scripts/"
        "run_mem_service_release_certification_ci.sh"
        in release_manifest
    )
    assert (
        "release_script=share/lingqu/mem_service/scripts/"
        "verify_mem_service_remote_transport_bundle.sh"
        in release_manifest
    )
    assert "remote_payload_production_network_transport=not-certified" in release_manifest
    assert (
        "remote_payload_production_transport_evidence_schema=remote-transport-evidence-v1"
        in release_manifest
    )
    assert (
        "remote_payload_production_transport_evidence_gate=remote-transport-evidence-fixtures"
        in release_manifest
    )
    assert (
        "remote_payload_production_transport_generate=remote-transport-generate-evidence"
        in release_manifest
    )
    assert (
        "remote_payload_production_transport_verify=remote-transport-verify --evidence-file"
        in release_manifest
    )
    assert (
        "remote_payload_production_transport_ci=scripts/run_mem_service_remote_transport_ci.sh"
        in release_manifest
    )
    assert (
        "remote_payload_production_transport_ci_preflight=scripts/run_mem_service_remote_transport_ci.sh --preflight"
        in release_manifest
    )
    assert (
        "remote_payload_production_transport_evidence_verify=scripts/verify_mem_service_remote_transport_evidence.sh"
        in release_manifest
    )
    assert (
        "remote_payload_production_transport_bundle=remote-transport-certification-bundle"
        in release_manifest
    )
    assert (
        "remote_payload_production_transport_bundle_verify=remote-transport-certification-bundle-verify"
        in release_manifest
    )
    assert (
        "remote_payload_production_transport_bundle_script=scripts/verify_mem_service_remote_transport_bundle.sh"
        in release_manifest
    )
    assert "ops_certification_policy=share/lingqu/mem_service/ops-certification-policy.txt" in release_manifest
    assert "ops_certification_policy_len=1118" in release_manifest
    assert "ops_certification_policy_checksum=0xe77c644b" in release_manifest
    assert "ops_certification_gate=ops-certification-fixtures" in release_manifest
    assert "ops_certification_evidence_schema=ops-certification-evidence-v1" in release_manifest
    assert "ops_certification_evidence_gate=ops-certification-evidence-fixtures" in release_manifest
    assert "ops_certification_generate=ops-certification-generate-evidence" in release_manifest
    assert "ops_certification_linux_ci_gate=ops-certification-linux-ci-smoke" in release_manifest
    assert "linux_ops_certification_smoke=linux-ops-certification-smoke" in release_manifest
    assert "linux_ops_evidence_verify=linux-ops-evidence-verify" in release_manifest
    assert "linux_ops_certification_bundle=linux-ops-certification-bundle" in release_manifest
    assert "linux_ops_certification_bundle_verify=linux-ops-certification-bundle-verify" in release_manifest
    assert "linux_ops_ci=scripts/run_mem_service_linux_ops_ci.sh" in release_manifest
    assert "linux_ops_ci_preflight=scripts/run_mem_service_linux_ops_ci.sh --preflight" in release_manifest
    assert "release_certification_verify=release-certification-verify" in release_manifest
    assert (
        "release_certification_verify_script=scripts/verify_mem_service_release_certification.sh"
        in release_manifest
    )
    assert (
        "release_certification_ci=scripts/run_mem_service_release_certification_ci.sh"
        in release_manifest
    )
    assert (
        "release_certification_preflight=scripts/run_mem_service_release_certification_ci.sh --preflight"
        in release_manifest
    )
    assert (
        "release_certification_readiness_gate=release-readiness --ops-evidence-file --remote-transport-evidence-file"
        in release_manifest
    )
    assert "linux_ops_upgrade_rollback_smoke=linux-ops-upgrade-rollback-smoke" in release_manifest
    assert "linux_ops_deployment_smoke=linux-ops-deployment-smoke" in release_manifest
    assert "ops_certification_verify=ops-certification-verify --evidence-file" in release_manifest
    assert "real_systemd_environment=not-certified" in release_manifest
    assert "production_collector_alert_environment=not-certified" in release_manifest
    assert "rpm_package=not-certified" in release_manifest
    assert "api_abi_policy_len=856" in release_manifest
    assert "api_abi_policy_checksum=0xd0cc1392" in release_manifest
    assert "config_schema_version=1" in release_manifest
    assert "config_schema=share/lingqu/mem_service/config/mem_service.conf.schema" in release_manifest
    assert "config_example=share/lingqu/mem_service/config/mem_service.example.conf" in release_manifest
    assert "runtime_config=etc/lingqu/mem_service/mem_service.conf" in release_manifest
    assert (
        "runtime_config_source=share/lingqu/mem_service/config/mem_service.runtime.conf"
        in release_manifest
    )
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
    assert "field_count=164" in wire_schema_manifest
    assert "field=resolve_runtime_handoff.expected_owner type=u32 required=0" in wire_schema_manifest
    assert "field=query_execution_artifact.expected_owner type=u32 required=0" in wire_schema_manifest
    assert "field=query_training_artifact.expected_owner type=u32 required=0" in wire_schema_manifest
    assert "oneof_field=resolve_kv_segment.0.block_hash" in wire_schema_manifest
    assert "mem_service_admin_output_schema_version=1" in admin_output_schema
    assert "admin_command=metrics-export operation=metrics response=prometheus-text" in admin_output_schema
    assert "metrics_prometheus_prefix=lingqu_mem_service_" in admin_output_schema
    assert "metric_field=request_latency_max_ms type=gauge" in admin_output_schema
    assert "audit_record_delimiter=audit_begin/audit_end" in admin_output_schema
    assert "snapshot_page_field=next_index type=u64" in admin_output_schema
    assert "fail_closed_status=checksum_mismatch" in admin_output_schema
    assert "mem_service_upgrade_rollback_policy_version=1" in upgrade_rollback_policy
    assert "upgrade_policy=current-version-only" in upgrade_rollback_policy
    assert "rollback_policy=current-version-only" in upgrade_rollback_policy
    assert "same_version_runtime_gate=upgrade-rollback-runtime-fixtures" in upgrade_rollback_policy
    assert "old_server_runtime_binary=certified" in upgrade_rollback_policy
    assert "new_client_old_server=certified" in upgrade_rollback_policy
    assert "required_gate=upgrade-rollback-runtime-fixtures" in upgrade_rollback_policy
    assert "required_gate=compat-runtime-fixtures" in upgrade_rollback_policy
    assert "required_gate=package-fixtures" in upgrade_rollback_policy
    assert "required_gate=install-smoke" in upgrade_rollback_policy
    assert "mem_service_ops_certification_policy_version=1" in ops_certification_policy
    assert "certification_status=not-certified" in ops_certification_policy
    assert "admission_rule=fail-closed-until-external-evidence" in ops_certification_policy
    assert "evidence_schema=ops-certification-evidence-v1" in ops_certification_policy
    assert "evidence_generate=ops-certification-generate-evidence" in ops_certification_policy
    assert "evidence_ci_gate=ops-certification-linux-ci-smoke" in ops_certification_policy
    assert "evidence_gate=ops-certification-evidence-fixtures" in ops_certification_policy
    assert "external_gate=linux-systemd-service-smoke" in ops_certification_policy
    assert "external_gate=prometheus-alertmanager-rule-smoke" in ops_certification_policy
    assert "external_gate=rpm-package-smoke" in ops_certification_policy
    assert "mem_service_package_manifest_version=1" in package_manifest
    assert "package_format=installed-layout-v1" in package_manifest
    assert "artifact_format=tar" in package_manifest
    assert "artifact_name=linqu_mem_service-installed-layout-v1.tar" in package_manifest
    assert "artifact_root=usr+etc" in package_manifest
    assert "artifact_install_prefix=/usr" in package_manifest
    assert "artifact_contents=installed-layout-v1-root" in package_manifest
    assert "artifact_gate=package-tarball-smoke" in package_manifest
    assert "native_package_format=deb" in package_manifest
    assert "native_package_name=linqu-mem-service_0.1.0-1_arm64.deb" in package_manifest
    assert "native_package_arch=arm64" in package_manifest
    assert "native_package_payload=debian-binary+control.tar.gz+data.tar.gz" in package_manifest
    assert "native_package_gate=package-deb-smoke" in package_manifest
    assert "native_package_runtime=not-executed-cross-compiled-arm64" in package_manifest
    assert "rpm_package_format=rpm" in package_manifest
    assert "rpm_package_name=linqu-mem-service-0.1.0-1.aarch64.rpm" in package_manifest
    assert "rpm_package_arch=aarch64" in package_manifest
    assert "rpm_package_payload=rpm-cpio+metadata" in package_manifest
    assert "rpm_package_gate=package-rpm-smoke" in package_manifest
    assert "rpm_package_runtime=requires-linux-rpm-toolchain" in package_manifest
    assert "installed_file_count=52" in package_manifest
    assert "pkgconfig=lib/pkgconfig/lingqu-mem-service.pc" in package_manifest
    assert "pkgconfig_name=lingqu-mem-service" in package_manifest
    assert "pkgconfig_cflags=-I${includedir}" in package_manifest
    assert (
        "pkgconfig_sdk_sources=${sourcedir}/mem_service_client.c "
        "${sourcedir}/mem_service_wire_client.c "
        "${sourcedir}/mem_service_provider.c"
        in package_manifest
    )
    assert (
        "pkgconfig_payload_provider_roce_sources="
        "${sourcedir}/mem_service_provider_roce.c"
        in package_manifest
    )
    assert (
        "pkgconfig_payload_provider_roce_libs=-lrdmacm -libverbs"
        in package_manifest
    )
    assert "file_class=public_headers count=11" in package_manifest
    assert "file_class=provider_sources count=2" in package_manifest
    assert "release_script_root=share/lingqu/mem_service/scripts" in package_manifest
    assert "release_certification_ci=scripts/run_mem_service_release_certification_ci.sh" in package_manifest
    assert (
        "release_certification_preflight=scripts/run_mem_service_release_certification_ci.sh --preflight"
        in package_manifest
    )
    assert (
        "release_certification_readiness_gate=release-readiness --ops-evidence-file --remote-transport-evidence-file"
        in package_manifest
    )
    assert "linux_ops_ci=scripts/run_mem_service_linux_ops_ci.sh" in package_manifest
    assert (
        "linux_ops_ci_preflight=scripts/run_mem_service_linux_ops_ci.sh --preflight"
        in package_manifest
    )
    assert "remote_payload_production_transport_ci=scripts/run_mem_service_remote_transport_ci.sh" in package_manifest
    assert (
        "remote_payload_production_transport_ci_preflight=scripts/run_mem_service_remote_transport_ci.sh --preflight"
        in package_manifest
    )
    assert (
        "release_script=share/lingqu/mem_service/scripts/"
        "verify_mem_service_installed_layout.sh"
        in package_manifest
    )
    assert (
        "release_script=share/lingqu/mem_service/scripts/"
        "verify_mem_service_installed_sdk.sh"
        in package_manifest
    )
    assert (
        "release_script=share/lingqu/mem_service/scripts/"
        "verify_mem_service_release_certification.sh"
        in package_manifest
    )
    assert (
        "release_script=share/lingqu/mem_service/scripts/run_mem_service_linux_ops_ci.sh"
        in package_manifest
    )
    assert (
        "release_script=share/lingqu/mem_service/scripts/"
        "run_mem_service_remote_transport_ci.sh"
        in package_manifest
    )
    assert (
        "release_script=share/lingqu/mem_service/scripts/"
        "run_mem_service_release_certification_ci.sh"
        in package_manifest
    )
    assert "system_config_root=etc/lingqu/mem_service" in package_manifest
    assert "runtime_config=etc/lingqu/mem_service/mem_service.conf" in package_manifest
    assert (
        "runtime_config_source=share/lingqu/mem_service/config/mem_service.runtime.conf"
        in package_manifest
    )
    assert "host_runtime_config=etc/lingqu/mem_service/mem_service.host.conf" in package_manifest
    assert (
        "host_runtime_config_source=share/lingqu/mem_service/config/mem_service.host.runtime.conf"
        in package_manifest
    )
    assert "systemd_unit_root=lib/systemd/system" in package_manifest
    assert "systemd_unit=lib/systemd/system/linqu_mem_service.service" in package_manifest
    assert "host_systemd_unit=lib/systemd/system/linqu_mem_service.host.service" in package_manifest
    assert "binary_version_command=version" in package_manifest
    assert "binary_version_contract=text-kv" in package_manifest
    assert "binary_version_gate=version-fixtures" in package_manifest
    assert "file_class=runtime_config count=2" in package_manifest
    assert "file_class=systemd_units count=2" in package_manifest
    assert "file_class=pkgconfig count=1" in package_manifest
    assert "file_class=release_scripts count=10" in package_manifest
    assert "required_gate_count=34" in package_manifest
    assert "required_gate=remote-transport-evidence-fixtures" in package_manifest
    assert "required_gate=version-fixtures" in package_manifest
    assert "required_gate=release-readiness-fixtures" in package_manifest
    assert "required_gate=runtime-quota-fixtures" in package_manifest
    assert "required_gate=retention-fixtures" in package_manifest
    assert "required_gate=checkpoint-retention-fixtures" in package_manifest
    assert "required_gate=payload-gc-fixtures" in package_manifest
    assert "required_gate=record-retention-fixtures" in package_manifest
    assert "remote_payload_production_network_transport=not-certified" in package_manifest
    assert (
        "contract=upgrade-rollback-policy path=share/lingqu/mem_service/"
        "upgrade-rollback-policy.txt checksum=0x096e86d0"
    ) in package_manifest
    assert "required_gate=package-fixtures" in package_manifest
    assert "required_gate=upgrade-rollback-runtime-fixtures" in package_manifest
    assert "required_gate=compat-runtime-fixtures" in package_manifest
    assert "required_gate=ops-certification-fixtures" in package_manifest
    assert "required_gate=ops-certification-evidence-fixtures" in package_manifest
    assert "required_gate=ops-certification-linux-ci-smoke" in package_manifest
    assert "required_gate=package-tarball-smoke" in package_manifest
    assert "required_gate=package-deb-smoke" in package_manifest
    assert "required_gate=package-rpm-smoke" in package_manifest
    assert "required_gate=installed-sdk-example-smoke" in package_manifest
    assert "required_gate=installed-sdk-pkgconfig-smoke" in package_manifest
    assert "required_gate=installed-sdk-runtime-smoke" in package_manifest
    assert (
        "installed_sdk_preflight=scripts/verify_mem_service_installed_sdk.sh --preflight"
        in package_manifest
    )
    assert (
        "installed_sdk_preflight_scope="
        "pkg-config-cflags+sdk-sources+examples+host-binary-no-compile"
        in package_manifest
    )
    assert "installed_sdk_pkgconfig_smoke=installed-sdk-pkgconfig-smoke" in package_manifest
    assert (
        "installed_sdk_pkgconfig_smoke_scope="
        "pkg-config-cflags+sdk-sources-external-client-compile"
        in package_manifest
    )
    assert "installed_sdk_runtime_smoke=installed-sdk-runtime-smoke" in package_manifest
    assert "installed_sdk_runtime_reuse=installed-sdk-runtime-smoke" in package_manifest
    assert (
        "installed_sdk_runtime_reuse_scope=daemon-restart+durable-store+serving+pretraining"
        in package_manifest
    )
    assert "required_gate=restore-policy-fixtures" in package_manifest
    assert "restore_policy=transactional-staged-restore" in package_manifest
    assert "restore_policy_gate=restore-policy-fixtures" in package_manifest
    assert "contract=ops-certification-policy" in package_manifest
    assert "cross_version_upgrade=certified" in package_manifest
    assert "alert: LingquMemServiceDown" in alert_rules
    assert "increase(lingqu_mem_service_fail_closed_count[5m]) > 0" in alert_rules
    assert "increase(lingqu_mem_service_checksum_mismatch_count[5m]) > 0" in alert_rules
    assert "lingqu_mem_service_request_latency_max_ms > 100" in alert_rules
    assert "mem_service_config_schema_version=1" in config_schema
    assert "field=listen type=string" in config_schema
    assert "must be unix:<path> while auth_mode=none" in config_schema
    assert "field=store type=string" in config_schema
    assert "field=storage_root type=string" in config_schema
    assert "field=backend type=enum values=snapshot,snapshot+journal" in config_schema
    assert "field=retention type=enum values=manual,audit-log:<events>" in config_schema
    assert "field=checkpoint_retention type=enum values=manual,latest:<records>" in config_schema
    assert "field=record_retention type=enum values=manual,latest:<records>,ttl-ms:<age-ms>,kind:<record-kind>:latest:<records>,kind:<record-kind>:ttl-ms:<age-ms>,tenant:<owner-node>:latest:<records>,tenant:<owner-node>:ttl-ms:<age-ms>" in config_schema
    assert "field=encryption type=enum values=none" in config_schema
    assert "unsupported modes fail closed" in config_schema
    assert "field=metrics_listen type=string" in config_schema
    assert "must be tcp:127.0.0.1:<port>" in config_schema
    assert "listen=unix:/tmp/linqu_mem_service.sock" in config_example
    assert "store=/tmp/linqu_mem_service.store" in config_example
    assert "backend=snapshot+journal" in config_example
    assert "checkpoint_retention=manual" in config_example
    assert "record_retention=manual" in config_example
    assert "encryption=none" in config_example
    assert "metrics_listen=tcp:127.0.0.1:9900" in config_example
    assert "listen=unix:/run/lingqu/mem_service.sock" in config_runtime
    assert "store=/var/lib/lingqu/mem_service/store.snapshot" in config_runtime
    assert "storage_root=/var/lib/lingqu/mem_service" in config_runtime
    assert "backend=snapshot+journal" in config_runtime
    assert "checkpoint_retention=manual" in config_runtime
    assert "record_retention=manual" in config_runtime
    assert "metrics_listen=tcp:127.0.0.1:9900" in config_runtime
    assert "listen=unix:/run/lingqu/mem_service_host.sock" in config_host_runtime
    assert "store=/var/lib/lingqu/mem_service_host/store.snapshot" in config_host_runtime
    assert "storage_root=/var/lib/lingqu/mem_service_host" in config_host_runtime
    assert "checkpoint_retention=manual" in config_host_runtime
    assert "record_retention=manual" in config_host_runtime
    assert "metrics_listen=tcp:127.0.0.1:9901" in config_host_runtime
    assert "service_auth_boundary=unix-socket-local-only" in package_manifest
    assert "metrics_auth_boundary=loopback-only" in package_manifest
    assert "config_security_gate=config-fixtures" in package_manifest
    assert "deployment_quota_contract=max-records+max-payload-bytes" in package_manifest
    assert "deployment_quota_gate=config-fixtures" in package_manifest
    assert "retention_policy=manual-or-audit-log-limit" in package_manifest
    assert "retention_policy_gate=config-fixtures,retention-fixtures" in package_manifest
    assert "checkpoint_retention_policy=manual-or-latest-limit" in package_manifest
    assert (
        "checkpoint_retention_gate=config-fixtures,checkpoint-retention-fixtures"
        in package_manifest
    )
    assert "record_retention_policy=manual-or-global-kind-tenant-latest-or-ttl" in package_manifest
    assert "record_retention_gate=config-fixtures,record-retention-fixtures" in package_manifest
    assert (
        "payload_block_gc=record-and-checkpoint-retention-orphan-blocks"
        in package_manifest
    )
    assert (
        "payload_block_gc_gate=payload-gc-fixtures,record-retention-fixtures"
        in package_manifest
    )
    assert "encryption_policy=explicit-none-only" in package_manifest
    assert "encryption_at_rest=not-certified" in package_manifest
    assert "encryption_policy_command=encryption-policy" in package_manifest
    assert "encryption_policy_gate=encryption-fixtures" in package_manifest
    assert "runtime_quota_admission=max-records+max-payload-bytes" in package_manifest
    assert "runtime_quota_gate=runtime-quota-fixtures" in package_manifest
    assert "ExecStart=/usr/bin/linqu_mem_service serve --config /etc/lingqu/mem_service/mem_service.conf" in deploy_manifest
    assert (
        "ExecStart=/usr/libexec/lingqu/mem_service/linqu_mem_service_host "
        "serve --config /etc/lingqu/mem_service/mem_service.host.conf"
        in host_deploy_manifest
    )
    assert "RuntimeDirectory=lingqu" in deploy_manifest
    assert "StateDirectory=lingqu/mem_service" in deploy_manifest
    assert "RuntimeDirectory=lingqu" in host_deploy_manifest
    assert "StateDirectory=lingqu/mem_service_host" in host_deploy_manifest
    assert '#include "mem_service_client.h"' in serving_example
    assert "mem_service_client_register_prefix_entry" in serving_example
    assert "mem_service_client_publish_kv_segment" in serving_example
    assert "mem_service_client_publish_runtime_handoff" in serving_example
    assert "mem_service_client_register_execution_artifact" in serving_example
    assert "mem_service_serving_example=ok" in serving_example
    assert '.prefix_group = "serving-prefix-qwen3"' in serving_example
    assert '.prefix_group = "serving-kv-qwen3"' in serving_example
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
    assert (component_dir / "mem_service_module.c").exists()
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
    assert (component_dir / "mem_service_provider.c").exists()
    assert (component_dir / "mem_service_provider.h").exists()
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
    assert "components/mem_service/mem_service_module.c" in makefile
    assert "components/mem_service/mem_service_cluster_utils.c" in makefile
    assert "components/mem_service/mem_service_cluster_payload.c" in makefile
    assert "components/mem_service/mem_service_cluster_read.c" in makefile
    assert "components/mem_service/mem_service_cluster_runtime.c" in makefile
    assert "components/mem_service/mem_service_cluster_queue.c" in makefile
    assert "components/mem_service/mem_service_cluster_observe.c" in makefile
    assert "components/mem_service/mem_service_obmm_object_flow.c" in makefile
    assert "components/mem_service/mem_service_client.c" in makefile
    assert "components/mem_service/mem_service_wire_client.c" in makefile
    assert "components/mem_service/mem_service_metadata.c" in makefile
    assert "components/mem_service/mem_service_provider.c" in makefile
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
    assert "$(MEM_SERVICE_CLIENT)" in makefile
    assert "$(MEM_SERVICE_WIRE_CLIENT)" in makefile
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
    assert '"$MEM_SERVICE_CLIENT_SRC" "$MEM_SERVICE_WIRE_CLIENT_SRC"' in build_script
    assert '"$LLM_INFER_SRC" -lm -o "$LLM_INFER_APP_BIN"' in build_script
    assert "write_signature_line \"llm_infer_src\"" in build_script
    assert '#include "components/llm_infer/llm_infer.h"' in llm_infer_app_source
    assert '#include "components/mem_service/mem_service_client.h"' in llm_infer_app_source
    assert '#include "components/mem_service/mem_service_wire_client.h"' in llm_infer_app_source
    assert "--mem-service-serving-publish" in llm_infer_app_source
    assert "--mem-service-serving-verify" in llm_infer_app_source
    assert ".prefix_group = \"qwen3-serving-prefix\"" in llm_infer_app_source
    assert ".prefix_group = \"qwen3-serving-kv\"" in llm_infer_app_source
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


def test_pretraining_client_runs_against_independent_mem_service_daemon():
    app_dir = ROOT / "apps" / "pretraining_client"
    makefile = (app_dir / "Makefile").read_text()
    readme = (app_dir / "README.md").read_text()
    app_source = (app_dir / "pretraining_client.c").read_text()
    build_script = (ROOT / "scripts" / "build_initramfs.sh").read_text()
    run_app = (ROOT / "initramfs" / "run_app").read_text()
    runner = (ROOT / "scripts" / "run_ub_dual_node_apps.sh").read_text()
    apps_readme = (ROOT / "apps" / "README.md").read_text()

    assert (app_dir / "pretraining_client.c").exists()
    assert (app_dir / "Makefile").exists()
    assert "all: linqu_pretraining_client" in makefile
    assert "components/mem_service/mem_service_client.c" in makefile
    assert "components/mem_service/mem_service_wire_client.c" in makefile
    assert "components/mem_service/mem_service_keys.c" in makefile
    assert "components/mem_service/mem_service_object_refs.c" in makefile
    assert "components/mem_service/mem_service_records.c" in makefile
    assert '#include "components/mem_service/mem_service_client.h"' in app_source
    assert "--mem-service-pretraining-publish" in app_source
    assert "--mem-service-pretraining-verify" in app_source
    assert "mem_service_client_publish_dataset_shard" in app_source
    assert "mem_service_client_resolve_dataset_shard" in app_source
    assert "mem_service_client_publish_sample_batch" in app_source
    assert "mem_service_client_resolve_sample_batch" in app_source
    assert "mem_service_client_publish_checkpoint" in app_source
    assert "mem_service_client_resolve_checkpoint" in app_source
    assert "mem_service_client_publish_gradient_bucket" in app_source
    assert "mem_service_client_resolve_gradient_bucket" in app_source
    assert "mem_service_client_publish_optimizer_state" in app_source
    assert "mem_service_client_resolve_optimizer_state" in app_source
    assert "mem_service_client_commit_training_step" in app_source
    assert "mem_service_client_resolve_training_step" in app_source
    assert "linqu_pretraining_client_mem_service_publish=ok" in app_source
    assert "linqu_pretraining_client_mem_service_verify=ok" in app_source
    assert "warm_reuse=1" in app_source
    assert "PRETRAINING_CLIENT_APP_SRC=" in build_script
    assert "PRETRAINING_CLIENT_APP_BIN=" in build_script
    assert "write_signature_line \"pretraining_client_app_src\"" in build_script
    assert '"$PRETRAINING_CLIENT_APP_SRC" "$MEM_SERVICE_CLIENT_SRC"' in build_script
    assert 'cp "$PRETRAINING_CLIENT_APP_BIN" "$INITRAMFS_DIR/bin/linqu_pretraining_client"' in build_script
    assert "run_pretraining_client_mem_service" in run_app
    assert "linqu_pretraining_client_mem_service=1" in run_app
    assert "--mem-service-pretraining-publish" in run_app
    assert "--mem-service-pretraining-verify" in run_app
    assert "linqu_pretraining_client mem_service publish restart verify done" in run_app
    assert "pretraining_client_mem_service" in runner
    assert 'flag="linqu_pretraining_client_mem_service=1"' in runner
    assert "pretraining_client_mem_service_enabled=1" in runner
    assert "linqu_pretraining_client mem_service publish restart verify done" in runner
    assert "scripts/run_ub_dual_node_apps.sh --app pretraining_client_mem_service" in apps_readme
    assert "`mem_service` socket API" in readme


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
    assert "LINGQU_MEM_SERVICE_UB_SSD_GSVA .* status=ok" in two_node_ssd_gsva_runner
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
    assert "LINGQU_MEM_SERVICE_UB_SSD_GSVA .* status=ok" in eight_node_ssd_gsva_runner
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
    w4_eight_runner = (ROOT / "scripts" / "run_llm_infer_eight_node_guest.sh").read_text()
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
    assert 'flag="linqu_llm_infer_mem_service=1"' in script
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
    assert "link_busybox_applet rm" in build_script
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
    assert "linqu_llm_infer_mem_service=1" in run_app
    assert "run_llm_infer" in run_app
    assert "run_llm_infer_mem_service" in run_app
    assert "/bin/linqu_mem_service serve --listen" in run_app
    assert '/bin/busybox rm -f "$socket"' in run_app
    assert "--mem-service-serving-publish" in run_app
    assert "--mem-service-serving-verify" in run_app
    assert "linqu_llm_infer mem_service publish restart verify done" in run_app
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
    assert "APP_WAIT_SECS * ${SIM_W5_SERVING_DECODE_STEPS_TOTAL:-$SIM_QWEN3_GUEST_DECODE_STEPS}" in w4_eight_runner
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
