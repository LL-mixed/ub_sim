use std::collections::{BTreeMap, BTreeSet};
use std::fs::{self, File, OpenOptions};
use std::io::{BufRead, BufReader, Read, Seek, SeekFrom, Write};
use std::os::unix::fs::FileTypeExt;
use std::os::unix::process::CommandExt;
use std::path::{Path, PathBuf};
use std::process::{Command as StdCommand, Stdio};
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::Arc;
use std::time::{Duration, SystemTime, UNIX_EPOCH};

use serde::{Deserialize, Serialize};
use tokio::io::AsyncWriteExt;
use tokio::net::UnixStream;
use tokio::process::Command;
use tokio::sync::{Mutex, RwLock};

use crate::domain::{
    topology_nodes, ControlCapability, DemoCatalog, DemoDefinition, DemoReadiness, GuestEngine,
    LogChunk, NodeInputDefinition, NodeInputKind, NodeInputRequest, NodeInputResult, NodeStatus,
    ReadinessIssue, ResolvedCommand, RunRecord, RunStatus, StartRunRequest,
    TargetPreparationResult,
};
use crate::target::{ExecutionTarget, ExecutionTargetKind, TargetRegistry};

const MAX_LOG_LINES: usize = 500;
const MAX_NODE_INPUT_BYTES: usize = 4096;
const NODE_INPUT_TIMEOUT: Duration = Duration::from_secs(5);
const NODE_INPUT_DRAIN_DELAY: Duration = Duration::from_millis(200);
const TARGET_PREPARATION_TIMEOUT: Duration = Duration::from_secs(20 * 60);
const NODE_STATUS_TAIL_BYTES: u64 = 256 * 1024;
const TARGET_NATIVE_TOOLS: &str = "git make cargo python3 cmake ninja pkg-config cc setsid tar curl zsh bc bison flex cpio rsync qemu-img partprobe vgscan vgchange";
const TARGET_PKG_CONFIG_DEPS: &str = "glib-2.0 libelf liburing openssl pixman-1 zlib";
const OPENEULER_BUILD_PACKAGES: &str = "bc bison cmake cpio curl elfutils-libelf-devel flex glib2-devel liburing-devel lvm2 openssl-devel parted pixman-devel qemu-img zlib-devel pkgconf-pkg-config ninja-build gcc gcc-c++ make zsh python3-pip rsync";
const STOP_REQUESTED_MARKER: &str = "stop.requested";
const KERNEL_BUILD_POLICY_REV: &str = "3";
const KERNEL_SIGNATURE_PATHS: &[&str] = &[
    "drivers/ub/obmm",
    "drivers/ub/ubus/ub_npu.c",
    "drivers/ub/ubus/ub_ssd.c",
    "drivers/ub/ubus/sim",
    "include/linux/obmm.h",
    "include/uapi/asm-generic/mman-common.h",
    "include/uapi/ub/obmm_async_load.h",
    "include/uapi/ub/ub_npu.h",
    "include/uapi/ub/ub_ssd.h",
    "include/uapi/ub/obmm.h",
    "mm/mmap.c",
];

#[derive(Debug, thiserror::Error)]
pub enum RunManagerError {
    #[error("unknown demo: {0}")]
    UnknownDemo(String),
    #[error("unknown execution target: {0}")]
    UnknownTarget(String),
    #[error("unknown run: {0}")]
    UnknownRun(String),
    #[error("unknown node {node} in run {run}")]
    UnknownNode { run: String, node: String },
    #[error("demo is not ready: {0}")]
    MissingRequirement(String),
    #[error("unsafe repository path: {0}")]
    UnsafePath(String),
    #[error("run {0} is already terminal")]
    TerminalRun(String),
    #[error("run {0} is already active")]
    ActiveRun(String),
    #[error("invalid node input: {0}")]
    InvalidNodeInput(String),
    #[error("node input is unavailable: {0}")]
    NodeInputUnavailable(String),
    #[error("target preparation is unavailable: {0}")]
    TargetPreparationUnavailable(String),
    #[error("demo {demo} is not ready: {reason}")]
    NotReady { demo: String, reason: String },
    #[error(transparent)]
    Domain(#[from] crate::domain::DomainError),
    #[error(transparent)]
    Io(#[from] std::io::Error),
    #[error(transparent)]
    Json(#[from] serde_json::Error),
}

#[derive(Clone)]
pub struct RunManager {
    inner: Arc<RunManagerInner>,
}

struct RunManagerInner {
    repo_root: PathBuf,
    state_root: PathBuf,
    catalog: Arc<DemoCatalog>,
    targets: Arc<TargetRegistry>,
    runs: RwLock<BTreeMap<String, RunRecord>>,
    stop_requested: RwLock<BTreeSet<String>>,
    start_lock: Mutex<()>,
    remote_log_sync: Mutex<()>,
    node_input_lock: Mutex<()>,
    counter: AtomicU64,
}

#[derive(Debug, Serialize, Deserialize)]
struct RemoteRunPlan {
    local_repo_root: PathBuf,
    run_id: String,
    source_head: String,
    target: ExecutionTarget,
    resolved: ResolvedCommand,
}

impl RunManager {
    pub fn new(repo_root: impl AsRef<Path>, catalog: DemoCatalog) -> Result<Self, RunManagerError> {
        Self::with_targets(repo_root, catalog, TargetRegistry::local_only())
    }

    pub fn with_targets(
        repo_root: impl AsRef<Path>,
        catalog: DemoCatalog,
        targets: TargetRegistry,
    ) -> Result<Self, RunManagerError> {
        let repo_root = repo_root.as_ref().canonicalize()?;
        let state_root = repo_root.join("out/sim-console/runs");
        fs::create_dir_all(&state_root)?;
        let runs = load_runs(&state_root)?;
        Ok(Self {
            inner: Arc::new(RunManagerInner {
                repo_root,
                state_root,
                catalog: Arc::new(catalog),
                targets: Arc::new(targets),
                runs: RwLock::new(runs),
                stop_requested: RwLock::new(BTreeSet::new()),
                start_lock: Mutex::new(()),
                remote_log_sync: Mutex::new(()),
                node_input_lock: Mutex::new(()),
                counter: AtomicU64::new(0),
            }),
        })
    }

    pub fn catalog(&self) -> Arc<DemoCatalog> {
        self.inner.catalog.clone()
    }

    pub fn targets(&self) -> Arc<TargetRegistry> {
        self.inner.targets.clone()
    }

    pub fn repo_root(&self) -> &Path {
        &self.inner.repo_root
    }

    pub async fn readiness(
        &self,
        target_id: Option<&str>,
    ) -> Result<Vec<DemoReadiness>, RunManagerError> {
        let target = self.resolve_target(target_id)?.clone();
        match target.kind {
            ExecutionTargetKind::Local => {
                let guest_artifact_issues = guest_artifact_readiness(&self.inner.repo_root);
                Ok(self
                    .inner
                    .catalog
                    .demos
                    .iter()
                    .map(|demo| self.local_demo_readiness(demo, &target, &guest_artifact_issues))
                    .collect())
            }
            ExecutionTargetKind::Ssh => self.remote_readiness(&target).await,
        }
    }

    pub async fn prepare_target(
        &self,
        target_id: &str,
    ) -> Result<TargetPreparationResult, RunManagerError> {
        let _start_guard = self.inner.start_lock.lock().await;
        if let Some(active_run) = self
            .inner
            .runs
            .read()
            .await
            .values()
            .find(|record| !record.status.is_terminal())
        {
            return Err(RunManagerError::ActiveRun(active_run.id.clone()));
        }
        let target = self.resolve_target(Some(target_id))?.clone();
        if target.kind != ExecutionTargetKind::Ssh {
            return Err(RunManagerError::TargetPreparationUnavailable(format!(
                "target {} is local and does not need farm preparation",
                target.id
            )));
        }
        let source_revision = git_head(&self.inner.repo_root)?;
        let bootstrap = run_remote_command_output(
            &target,
            remote_target_bootstrap_command(&target)?,
            TARGET_PREPARATION_TIMEOUT,
            "prepare target source and tools",
        )
        .await?;
        let bootstrap_log = String::from_utf8_lossy(&bootstrap.stdout);
        let source_repo_created = bootstrap_log
            .lines()
            .any(|line| line == "SOURCE_REPO_CREATED");
        let installed_tools = bootstrap_log
            .lines()
            .filter_map(|line| line.strip_prefix("TOOL_INSTALLED\t"))
            .map(str::to_string)
            .collect::<Vec<_>>();

        let preparation_root = self
            .inner
            .state_root
            .parent()
            .unwrap_or(&self.inner.state_root)
            .join("target-preparation")
            .join(&target.id);
        fs::create_dir_all(&preparation_root)?;
        let source_repo = remote_workspace_source_repo(&target)?.to_string();
        let source_parent = Path::new(&source_repo)
            .parent()
            .and_then(Path::to_str)
            .ok_or_else(|| RunManagerError::UnknownTarget(target.id.clone()))?;
        let remote_bundle_root = format!("{source_parent}/.sim-console-target-preparation");
        let mut prepared_files = Vec::new();
        for relative_path in &target.bootstrap_files {
            let local_file = self.resolve_existing_path(relative_path)?;
            if !local_file.is_file() {
                return Err(RunManagerError::MissingRequirement(relative_path.clone()));
            }
            let remote_file = format!("{source_repo}/{relative_path}");
            transfer_target_file(&target, &local_file, &remote_file).await?;
            prepared_files.push(relative_path.clone());
        }
        let mut prepared_submodules = Vec::new();
        for mirror in &target.submodule_mirrors {
            let expected = submodule_commit_at_revision(
                &self.inner.repo_root,
                &source_revision,
                &mirror.path,
            )?;
            let remote_repo = format!("{source_repo}/{}", mirror.path);
            let output = run_remote_command_output(
                &target,
                remote_source_mirror_prepare_command(mirror, &remote_repo, &expected),
                TARGET_PREPARATION_TIMEOUT,
                &format!("prepare source mirror {}", mirror.path),
            )
            .await?;
            if String::from_utf8_lossy(&output.stdout)
                .lines()
                .any(|line| line == "MIRROR_NEEDS_CHECKOUT_PACK")
            {
                let local_repo = self.inner.repo_root.join(&mirror.path);
                let pack_path = preparation_root.join(format!("{}.pack", expected));
                create_checkout_pack(&local_repo, &expected, &pack_path)?;
                let remote_pack = format!("{remote_bundle_root}/{}.pack", expected);
                transfer_target_file(&target, &pack_path, &remote_pack).await?;
                run_remote_command_output(
                    &target,
                    remote_source_mirror_import_checkout_pack_command(
                        &remote_repo,
                        &remote_pack,
                        &expected,
                    ),
                    TARGET_PREPARATION_TIMEOUT,
                    &format!("seed source mirror {}", mirror.path),
                )
                .await?;
                fs::remove_file(&pack_path).ok();
            }
            prepared_submodules.push(mirror.path.clone());
        }

        let readiness = self.remote_readiness(&target).await?;
        let ready_demos = readiness.iter().filter(|item| item.ready).count();
        Ok(TargetPreparationResult {
            target_id: target.id,
            source_repo,
            source_revision,
            source_repo_created,
            installed_tools,
            prepared_submodules,
            prepared_files,
            ready_demos,
            blocked_demos: readiness.len() - ready_demos,
        })
    }

    pub async fn start(&self, request: StartRunRequest) -> Result<RunRecord, RunManagerError> {
        let _start_guard = self.inner.start_lock.lock().await;
        if let Some(active_run) = self
            .inner
            .runs
            .read()
            .await
            .values()
            .find(|record| !record.status.is_terminal())
        {
            return Err(RunManagerError::ActiveRun(active_run.id.clone()));
        }
        let demo = self
            .inner
            .catalog
            .find(&request.demo_id)
            .cloned()
            .ok_or_else(|| RunManagerError::UnknownDemo(request.demo_id.clone()))?;
        let target = self.resolve_target(request.target_id.as_deref())?.clone();
        let readiness = self
            .readiness(Some(&target.id))
            .await?
            .into_iter()
            .find(|item| item.demo_id == demo.id)
            .ok_or_else(|| RunManagerError::UnknownDemo(demo.id.clone()))?;
        if !readiness.ready {
            return Err(RunManagerError::NotReady {
                demo: demo.id,
                reason: readiness
                    .issues
                    .iter()
                    .map(|issue| issue.message.as_str())
                    .collect::<Vec<_>>()
                    .join("; "),
            });
        }
        if target.kind == ExecutionTargetKind::Local {
            self.check_requirements(&demo.required_paths)?;
        }

        let run_id = self.next_run_id();
        let mut resolved = demo.resolve_command(&run_id, &request.parameters)?;
        inject_target_model_source(&demo, &target, &mut resolved)?;
        inject_target_guest_engine(&demo, &target, &mut resolved)?;
        let source_revision = match target.kind {
            ExecutionTargetKind::Local => git_head(&self.inner.repo_root).ok(),
            ExecutionTargetKind::Ssh => Some(git_head(&self.inner.repo_root)?),
        };
        let run_dir = self.inner.state_root.join(&run_id);
        fs::create_dir_all(&run_dir)?;
        let process_log = run_dir.join("process.log");

        let mut record = RunRecord {
            id: run_id.clone(),
            demo_id: demo.id.clone(),
            demo_title: demo.title.clone(),
            target_id: target.id.clone(),
            source_revision: source_revision.clone(),
            status: RunStatus::Starting,
            created_at_ms: now_ms(),
            started_at_ms: None,
            finished_at_ms: None,
            pid: None,
            exit_code: None,
            parameters: resolved.parameters.clone(),
            nodes: topology_nodes(demo.node_count),
            process_log_path: relative_display(&self.inner.repo_root, &process_log),
            message: None,
        };
        persist_record(&run_dir, &record)?;
        self.inner
            .runs
            .write()
            .await
            .insert(run_id.clone(), record.clone());

        let stdout = OpenOptions::new()
            .create(true)
            .append(true)
            .open(&process_log)?;
        let stderr = stdout.try_clone()?;
        let mut command = match target.kind {
            ExecutionTargetKind::Local => {
                let program = self.resolve_existing_path(&resolved.program)?;
                let mut command = Command::new(program);
                command
                    .current_dir(&self.inner.repo_root)
                    .args(&resolved.args)
                    .envs(&resolved.environment);
                command
            }
            ExecutionTargetKind::Ssh => {
                let plan_path = run_dir.join("remote-plan.json");
                let plan = RemoteRunPlan {
                    local_repo_root: self.inner.repo_root.clone(),
                    run_id: run_id.clone(),
                    source_head: source_revision
                        .clone()
                        .expect("SSH source revision was resolved before run creation"),
                    target: target.clone(),
                    resolved: resolved.clone(),
                };
                fs::write(&plan_path, serde_json::to_vec_pretty(&plan)?)?;
                let mut command = Command::new(std::env::current_exe()?);
                command.arg("__remote-worker").arg(plan_path);
                command
            }
        };
        command
            .stdin(Stdio::null())
            .stdout(Stdio::from(stdout))
            .stderr(Stdio::from(stderr));
        command.as_std_mut().process_group(0);

        let mut child = match command.spawn() {
            Ok(child) => child,
            Err(error) => {
                record.status = RunStatus::Failed;
                record.finished_at_ms = Some(now_ms());
                record.message = Some(format!("failed to start: {error}"));
                persist_record(&run_dir, &record)?;
                self.inner.runs.write().await.insert(run_id, record.clone());
                return Err(RunManagerError::Io(error));
            }
        };

        record.status = RunStatus::Running;
        record.started_at_ms = Some(now_ms());
        record.pid = child.id();
        persist_record(&run_dir, &record)?;
        self.inner
            .runs
            .write()
            .await
            .insert(run_id.clone(), record.clone());

        if target.kind == ExecutionTargetKind::Ssh {
            let manager = self.clone();
            let monitored_run_id = run_id.clone();
            tokio::spawn(async move {
                manager.monitor_remote_logs(monitored_run_id).await;
            });
        }

        let manager = self.clone();
        tokio::spawn(async move {
            let result = child.wait().await;
            manager.finish_run(&run_id, result).await;
        });
        Ok(record)
    }

    pub async fn list(&self) -> Vec<RunRecord> {
        let ids: Vec<String> = self.inner.runs.read().await.keys().cloned().collect();
        let mut records = Vec::with_capacity(ids.len());
        for id in ids {
            if let Ok(record) = self.get(&id).await {
                records.push(record);
            }
        }
        records.sort_by(|left, right| right.created_at_ms.cmp(&left.created_at_ms));
        records
    }

    pub async fn get(&self, run_id: &str) -> Result<RunRecord, RunManagerError> {
        let mut runs = self.inner.runs.write().await;
        let record = runs
            .get_mut(run_id)
            .ok_or_else(|| RunManagerError::UnknownRun(run_id.to_string()))?;
        let original_nodes = record.nodes.clone();
        self.refresh_nodes(record)?;
        if record.nodes != original_nodes {
            persist_record(&self.inner.state_root.join(run_id), record)?;
        }
        Ok(record.clone())
    }

    pub async fn stop(&self, run_id: &str) -> Result<RunRecord, RunManagerError> {
        let record = self.get(run_id).await?;
        if record.status.is_terminal() {
            return Err(RunManagerError::TerminalRun(run_id.to_string()));
        }
        let pid = record
            .pid
            .ok_or_else(|| RunManagerError::TerminalRun(run_id.to_string()))?;
        self.inner
            .stop_requested
            .write()
            .await
            .insert(run_id.to_string());

        let target = self.resolve_target(Some(&record.target_id))?.clone();
        if target.kind == ExecutionTargetKind::Ssh {
            let mut remote_stop = ssh_command(&target)?;
            remote_stop.arg(remote_stop_command(&target, run_id)?);
            let output = tokio::time::timeout(
                Duration::from_secs(target.connect_timeout_secs.unwrap_or(10) + 5),
                remote_stop.output(),
            )
            .await
            .map_err(|_| {
                RunManagerError::Io(std::io::Error::new(
                    std::io::ErrorKind::TimedOut,
                    format!("remote stop timed out for run {run_id}"),
                ))
            })??;
            if !output.status.success() {
                return Err(RunManagerError::Io(std::io::Error::other(format!(
                    "remote stop failed: {}",
                    String::from_utf8_lossy(&output.stderr).trim()
                ))));
            }
        }

        fs::write(
            self.inner
                .state_root
                .join(run_id)
                .join(STOP_REQUESTED_MARKER),
            b"stop requested\n",
        )?;

        let result = unsafe { libc::kill(-(pid as i32), libc::SIGTERM) };
        if result != 0 {
            return Err(RunManagerError::Io(std::io::Error::last_os_error()));
        }
        {
            let mut runs = self.inner.runs.write().await;
            if let Some(current) = runs.get_mut(run_id) {
                current.message = Some("stop requested".to_string());
                persist_record(&self.inner.state_root.join(run_id), current)?;
            }
        }

        let manager = self.clone();
        let owned_run_id = run_id.to_string();
        tokio::spawn(async move {
            tokio::time::sleep(Duration::from_secs(3)).await;
            if let Ok(current) = manager.get(&owned_run_id).await {
                if !current.status.is_terminal() {
                    if let Some(pid) = current.pid {
                        unsafe {
                            libc::kill(-(pid as i32), libc::SIGKILL);
                        }
                    }
                }
            }
        });
        self.get(run_id).await
    }

    pub async fn logs(
        &self,
        run_id: &str,
        node: Option<&str>,
        cursor: usize,
    ) -> Result<LogChunk, RunManagerError> {
        let record = self.get(run_id).await?;
        let path = match node {
            Some(node_id) => {
                let node_record = record
                    .nodes
                    .iter()
                    .find(|item| item.id == node_id)
                    .ok_or_else(|| RunManagerError::UnknownNode {
                        run: run_id.to_string(),
                        node: node_id.to_string(),
                    })?;
                node_record
                    .log_path
                    .as_ref()
                    .map(|path| self.inner.repo_root.join(path))
                    .unwrap_or_else(|| self.inner.repo_root.join(&record.process_log_path))
            }
            None => self.inner.repo_root.join(&record.process_log_path),
        };
        let (lines, next_cursor) = read_log_chunk(&path, cursor)?;
        Ok(LogChunk {
            run_id: run_id.to_string(),
            node: node.map(str::to_string),
            cursor,
            next_cursor,
            complete: record.status.is_terminal(),
            lines,
        })
    }

    pub async fn send_node_input(
        &self,
        run_id: &str,
        node_id: &str,
        request: NodeInputRequest,
    ) -> Result<NodeInputResult, RunManagerError> {
        let _input_guard = self.inner.node_input_lock.lock().await;
        let record = self.get(run_id).await?;
        if record.status.is_terminal() {
            return Err(RunManagerError::TerminalRun(run_id.to_string()));
        }
        if !record.nodes.iter().any(|node| node.id == node_id) {
            return Err(RunManagerError::UnknownNode {
                run: run_id.to_string(),
                node: node_id.to_string(),
            });
        }
        let demo = self
            .inner
            .catalog
            .find(&record.demo_id)
            .ok_or_else(|| RunManagerError::UnknownDemo(record.demo_id.clone()))?;
        if !demo.controls.contains(&ControlCapability::NodeInput) {
            return Err(RunManagerError::NodeInputUnavailable(format!(
                "demo {} does not expose node input",
                demo.id
            )));
        }
        let adapter = demo.node_input.as_ref().ok_or_else(|| {
            RunManagerError::NodeInputUnavailable(format!(
                "demo {} has no node input adapter",
                demo.id
            ))
        })?;
        let payload = node_input_payload(request)?;
        let manifest = resolve_node_input_manifest(adapter, run_id)?;
        let target = self.resolve_target(Some(&record.target_id))?.clone();

        let bytes_written = match target.kind {
            ExecutionTargetKind::Local => {
                let manifest = self.inner.repo_root.join(manifest);
                send_local_node_input(adapter, &manifest, node_id, &payload).await?
            }
            ExecutionTargetKind::Ssh => {
                send_remote_node_input(&target, adapter, &manifest, node_id, &payload).await?
            }
        };
        append_node_input_event(
            &self.inner.repo_root.join(&record.process_log_path),
            node_id,
            bytes_written,
        )?;
        Ok(NodeInputResult {
            run_id: run_id.to_string(),
            node_id: node_id.to_string(),
            bytes_written,
        })
    }

    fn next_run_id(&self) -> String {
        let sequence = self.inner.counter.fetch_add(1, Ordering::Relaxed);
        format!("sim-console-{}-{sequence}", now_ms())
    }

    fn resolve_existing_path(&self, value: &str) -> Result<PathBuf, RunManagerError> {
        let path = self.inner.repo_root.join(value);
        let canonical = path
            .canonicalize()
            .map_err(|_| RunManagerError::MissingRequirement(value.to_string()))?;
        if !canonical.starts_with(&self.inner.repo_root) {
            return Err(RunManagerError::UnsafePath(value.to_string()));
        }
        Ok(canonical)
    }

    fn check_requirements(&self, paths: &[String]) -> Result<(), RunManagerError> {
        for path in paths {
            self.resolve_existing_path(path)?;
        }
        Ok(())
    }

    fn resolve_target(&self, target_id: Option<&str>) -> Result<&ExecutionTarget, RunManagerError> {
        self.inner.targets.resolve(target_id).ok_or_else(|| {
            RunManagerError::UnknownTarget(
                target_id
                    .unwrap_or(&self.inner.targets.default_target)
                    .to_string(),
            )
        })
    }

    fn local_demo_readiness(
        &self,
        demo: &DemoDefinition,
        target: &ExecutionTarget,
        guest_artifact_issues: &[ReadinessIssue],
    ) -> DemoReadiness {
        let mut issues = Vec::new();
        for path in &demo.required_paths {
            if self.resolve_existing_path(path).is_err() {
                issues.push(ReadinessIssue {
                    code: "required_path_missing".to_string(),
                    message: format!("Required repository path is missing: {path}"),
                    remedy: "Initialize the required submodule or restore the registered repository path."
                        .to_string(),
                });
            }
        }
        if demo.requires_guest_artifacts {
            issues.extend_from_slice(guest_artifact_issues);
        }
        let missing_model_sources = target
            .model_sources
            .iter()
            .filter(|(_, path)| !Path::new(path).exists())
            .map(|(model_source, _)| model_source.clone())
            .collect();
        if let Some(issue) = model_source_readiness_issue(demo, target, &missing_model_sources) {
            issues.push(issue);
        }
        let disk_image_missing = target
            .open_euler_disk_image
            .as_deref()
            .is_some_and(|path| !Path::new(path).is_file());
        if let Some(issue) = guest_engine_readiness_issue(demo, target, disk_image_missing) {
            issues.push(issue);
        }
        DemoReadiness {
            demo_id: demo.id.clone(),
            target_id: target.id.clone(),
            ready: issues.is_empty(),
            issues,
        }
    }

    async fn remote_readiness(
        &self,
        target: &ExecutionTarget,
    ) -> Result<Vec<DemoReadiness>, RunManagerError> {
        let mut command = ssh_command(target)?;
        command.arg(remote_probe_command(target, &self.inner.repo_root)?);
        let output = match tokio::time::timeout(
            Duration::from_secs(target.connect_timeout_secs.unwrap_or(10) + 5),
            command.output(),
        )
        .await
        {
            Ok(Ok(output)) => output,
            Ok(Err(error)) => {
                return Ok(self.remote_target_blocked_readiness(
                    target,
                    "target_unreachable",
                    format!("Failed to contact target {}: {error}", target.id),
                    "Check SSH connectivity and the target registry entry.",
                ));
            }
            Err(_) => {
                return Ok(self.remote_target_blocked_readiness(
                    target,
                    "target_unreachable",
                    format!("Timed out contacting target {}", target.id),
                    "Check SSH connectivity and host load, then refresh readiness.",
                ));
            }
        };
        if !output.status.success() {
            return Ok(self.remote_target_blocked_readiness(
                target,
                "target_probe_failed",
                format!(
                    "Target probe failed on {}: {}",
                    target.id,
                    String::from_utf8_lossy(&output.stderr).trim()
                ),
                "Verify the remote shell and configured repository path.",
            ));
        }

        let probe = String::from_utf8_lossy(&output.stdout);
        let mut common_issues = Vec::new();
        let mut simpler_toolchain_issues = Vec::new();
        let mut missing_model_sources = BTreeSet::new();
        let mut open_euler_disk_image_missing = false;
        for line in probe.lines() {
            if line == "SOURCE_REPO_MISSING" {
                common_issues.push(ReadinessIssue {
                    code: "remote_source_repo_missing".to_string(),
                    message: format!(
                        "Git source repository is missing on {} at {}",
                        target.id,
                        target.workspace_source_repo.as_deref().unwrap_or("<unset>")
                    ),
                    remedy: "Clone the repository at workspace_source_repo on the target."
                        .to_string(),
                });
            } else if let Some(tool) = line.strip_prefix("TOOL_MISSING\t") {
                common_issues.push(ReadinessIssue {
                    code: "remote_tool_missing".to_string(),
                    message: format!("Required build tool is missing on {}: {tool}", target.id),
                    remedy: "Use Prepare target farm to install the reviewed build tools."
                        .to_string(),
                });
            } else if let Some(details) = line.strip_prefix("TOOL_VERSION_MISMATCH\t") {
                let mut fields = details.split('\t');
                let tool = fields.next().unwrap_or("unknown");
                let required = fields.next().unwrap_or("unknown");
                let actual = fields.next().unwrap_or("missing");
                simpler_toolchain_issues.push(ReadinessIssue {
                    code: "remote_tool_version_mismatch".to_string(),
                    message: format!(
                        "Required build tool version is unavailable on {}: {tool} requires {required}, found {actual}",
                        target.id
                    ),
                    remedy: "Use Prepare target farm to install the reviewed compiler toolchain."
                        .to_string(),
                });
            } else if let Some(path) = line.strip_prefix("BOOTSTRAP_FILE_MISSING\t") {
                common_issues.push(ReadinessIssue {
                    code: "remote_bootstrap_file_missing".to_string(),
                    message: format!(
                        "Target {} is missing prepared build input: {path}",
                        target.id
                    ),
                    remedy: "Prepare the target farm to seed the registered build input."
                        .to_string(),
                });
            } else if let Some(details) = line.strip_prefix("SUBMODULE_CACHE_MISSING\t") {
                if let Some((path, revision)) = details.split_once('\t') {
                    common_issues.push(ReadinessIssue {
                        code: "remote_submodule_cache_missing".to_string(),
                        message: format!(
                            "Target {} is missing submodule object {} at {}",
                            target.id,
                            short_revision(revision),
                            path
                        ),
                        remedy: "Prepare the target farm to seed the exact committed object."
                            .to_string(),
                    });
                }
            } else if let Some(details) = line.strip_prefix("MODEL_SOURCE_MISSING\t") {
                if let Some((model_source, _)) = details.split_once('\t') {
                    missing_model_sources.insert(model_source.to_string());
                }
            } else if line == "OPEN_EULER_DISK_IMAGE_MISSING" {
                open_euler_disk_image_missing = true;
            }
        }
        let submodule_issues = target
            .submodule_mirrors
            .iter()
            .filter_map(|mirror| {
                submodule_checkout_mismatch(&self.inner.repo_root, &mirror.path)
                    .ok()
                    .flatten()
                    .map(|(expected, actual)| ReadinessIssue {
                        code: "submodule_gitlink_mismatch".to_string(),
                        message: format!(
                            "Local submodule checkout differs from HEAD gitlink: {} \
                             (HEAD={}, checkout={})",
                            mirror.path,
                            short_revision(&expected),
                            short_revision(&actual)
                        ),
                        remedy: format!(
                            "Commit the intended {} gitlink before remote execution.",
                            mirror.path
                        ),
                    })
            })
            .collect::<Vec<_>>();

        Ok(self
            .inner
            .catalog
            .demos
            .iter()
            .map(|demo| {
                let mut issues = common_issues.clone();
                if demo.requires_simpler_toolchain {
                    issues.extend(simpler_toolchain_issues.clone());
                }
                if demo.requires_guest_artifacts {
                    issues.extend(submodule_issues.clone());
                }
                for path in &demo.required_paths {
                    if !path_committed_at_head(&self.inner.repo_root, path) {
                        issues.push(ReadinessIssue {
                            code: "required_path_not_committed".to_string(),
                            message: format!("Required path is not present in local HEAD: {path}"),
                            remedy: "Commit the launcher or configuration before remote execution."
                                .to_string(),
                        });
                    }
                }
                if let Some(issue) =
                    model_source_readiness_issue(demo, target, &missing_model_sources)
                {
                    issues.push(issue);
                }
                if let Some(issue) =
                    guest_engine_readiness_issue(demo, target, open_euler_disk_image_missing)
                {
                    issues.push(issue);
                }
                DemoReadiness {
                    demo_id: demo.id.clone(),
                    target_id: target.id.clone(),
                    ready: issues.is_empty(),
                    issues,
                }
            })
            .collect())
    }

    fn remote_target_blocked_readiness(
        &self,
        target: &ExecutionTarget,
        code: &str,
        message: String,
        remedy: &str,
    ) -> Vec<DemoReadiness> {
        self.inner
            .catalog
            .demos
            .iter()
            .map(|demo| DemoReadiness {
                demo_id: demo.id.clone(),
                target_id: target.id.clone(),
                ready: false,
                issues: vec![ReadinessIssue {
                    code: code.to_string(),
                    message: message.clone(),
                    remedy: remedy.to_string(),
                }],
            })
            .collect()
    }

    async fn monitor_remote_logs(&self, run_id: String) {
        loop {
            let _ = self.sync_remote_logs(&run_id).await;
            let terminal = self
                .inner
                .runs
                .read()
                .await
                .get(&run_id)
                .is_none_or(|record| record.status.is_terminal());
            if terminal {
                return;
            }
            tokio::time::sleep(Duration::from_secs(5)).await;
        }
    }

    async fn sync_remote_logs(&self, run_id: &str) -> Result<(), RunManagerError> {
        let _sync_guard = self.inner.remote_log_sync.lock().await;
        let target_id = self
            .inner
            .runs
            .read()
            .await
            .get(run_id)
            .map(|record| record.target_id.clone())
            .ok_or_else(|| RunManagerError::UnknownRun(run_id.to_string()))?;
        let target = self.resolve_target(Some(&target_id))?.clone();
        if target.kind != ExecutionTargetKind::Ssh {
            return Ok(());
        }

        let mut command = ssh_command(&target)?;
        command.arg(remote_log_archive_command(&target, run_id)?);
        let output = tokio::time::timeout(
            Duration::from_secs(target.connect_timeout_secs.unwrap_or(10) + 10),
            command.output(),
        )
        .await
        .map_err(|_| {
            RunManagerError::Io(std::io::Error::new(
                std::io::ErrorKind::TimedOut,
                format!("remote log sync timed out for run {run_id}"),
            ))
        })??;
        if !output.status.success() {
            return Err(RunManagerError::Io(std::io::Error::other(format!(
                "remote log sync failed: {}",
                String::from_utf8_lossy(&output.stderr).trim()
            ))));
        }

        let run_dir = self.inner.state_root.join(run_id);
        let archive = run_dir.join("remote-node-logs.tar");
        let destination = run_dir.join("remote-node-logs");
        let staging = run_dir.join("remote-node-logs.tmp");
        fs::write(&archive, &output.stdout)?;
        fs::remove_dir_all(&staging).ok();
        fs::create_dir_all(&staging)?;
        let status = StdCommand::new("tar")
            .arg("-xf")
            .arg(&archive)
            .arg("-C")
            .arg(&staging)
            .status()?;
        fs::remove_file(&archive).ok();
        if !status.success() {
            fs::remove_dir_all(&staging).ok();
            return Err(RunManagerError::Io(std::io::Error::other(
                "failed to extract mirrored remote node logs",
            )));
        }
        fs::remove_dir_all(&destination).ok();
        fs::rename(staging, destination)?;
        Ok(())
    }

    async fn finish_run(
        &self,
        run_id: &str,
        result: Result<std::process::ExitStatus, std::io::Error>,
    ) {
        let target_id = self
            .inner
            .runs
            .read()
            .await
            .get(run_id)
            .map(|record| record.target_id.clone());
        if target_id
            .as_deref()
            .and_then(|id| self.inner.targets.find(id))
            .is_some_and(|target| target.kind == ExecutionTargetKind::Ssh)
        {
            let _ = self.sync_remote_logs(run_id).await;
        }
        let stop_requested = self.inner.stop_requested.write().await.remove(run_id)
            || self
                .inner
                .state_root
                .join(run_id)
                .join(STOP_REQUESTED_MARKER)
                .is_file();
        let mut runs = self.inner.runs.write().await;
        let Some(record) = runs.get_mut(run_id) else {
            return;
        };
        record.finished_at_ms = Some(now_ms());
        match result {
            Ok(status) if stop_requested => {
                record.status = RunStatus::Stopped;
                record.exit_code = status.code();
                record.message = Some("stopped by user".to_string());
            }
            Ok(status) if status.success() => {
                record.status = RunStatus::Passed;
                record.exit_code = status.code();
                record.message = Some("process completed successfully".to_string());
            }
            Ok(status) => {
                record.status = RunStatus::Failed;
                record.exit_code = status.code();
                record.message = Some(format!("process exited with {status}"));
            }
            Err(error) => {
                record.status = RunStatus::Failed;
                record.message = Some(format!("process wait failed: {error}"));
            }
        }
        if let Err(error) = self.refresh_nodes(record) {
            record.message = Some(format!(
                "{}; node refresh failed: {error}",
                record.message.as_deref().unwrap_or("run completed")
            ));
        }
        let _ = persist_record(&self.inner.state_root.join(run_id), record);
    }

    fn refresh_nodes(&self, record: &mut RunRecord) -> Result<(), RunManagerError> {
        let target = self.resolve_target(Some(&record.target_id))?;
        let discovered = match target.kind {
            ExecutionTargetKind::Local => discover_node_logs(&self.inner.repo_root, &record.id)?,
            ExecutionTargetKind::Ssh => {
                discover_mirrored_node_logs(&self.inner.state_root.join(&record.id))?
            }
        };
        for node in &mut record.nodes {
            if let Some(path) = discovered.get(&node.id) {
                node.log_path = Some(relative_display(&self.inner.repo_root, path));
                node.status = classify_node_log(path)?;
                if record.status == RunStatus::Passed && node.status != NodeStatus::Failed {
                    node.status = NodeStatus::Passed;
                }
            }
            if record.status == RunStatus::Stopped
                && matches!(
                    node.status,
                    NodeStatus::Unknown | NodeStatus::Booting | NodeStatus::Ready
                )
            {
                node.status = NodeStatus::Stopped;
            }
        }
        Ok(())
    }
}

fn node_input_payload(request: NodeInputRequest) -> Result<Vec<u8>, RunManagerError> {
    let mut payload = request.data.into_bytes();
    if request.append_newline {
        payload.push(b'\n');
    }
    if payload.is_empty() {
        return Err(RunManagerError::InvalidNodeInput(
            "input is empty and append_newline is false".to_string(),
        ));
    }
    if payload.len() > MAX_NODE_INPUT_BYTES {
        return Err(RunManagerError::InvalidNodeInput(format!(
            "payload is {} bytes; maximum is {MAX_NODE_INPUT_BYTES}",
            payload.len()
        )));
    }
    Ok(payload)
}

fn resolve_node_input_manifest(
    adapter: &NodeInputDefinition,
    run_id: &str,
) -> Result<PathBuf, RunManagerError> {
    let value = adapter.manifest.replace("{run_id}", run_id);
    if value.contains('{') || value.contains('}') {
        return Err(RunManagerError::NodeInputUnavailable(
            "node input manifest has an unresolved placeholder".to_string(),
        ));
    }
    let path = PathBuf::from(value);
    if path.is_absolute()
        || path.components().any(|component| {
            matches!(
                component,
                std::path::Component::ParentDir
                    | std::path::Component::RootDir
                    | std::path::Component::Prefix(_)
            )
        })
    {
        return Err(RunManagerError::UnsafePath(path.display().to_string()));
    }
    Ok(path)
}

async fn send_local_node_input(
    adapter: &NodeInputDefinition,
    manifest: &Path,
    node_id: &str,
    payload: &[u8],
) -> Result<usize, RunManagerError> {
    match adapter.kind {
        NodeInputKind::QemuSerialEnv => {
            let socket = serial_socket_from_manifest(adapter, manifest, node_id)?;
            let operation = async {
                let mut stream = UnixStream::connect(&socket).await?;
                stream.write_all(payload).await?;
                tokio::time::sleep(NODE_INPUT_DRAIN_DELAY).await;
                stream.shutdown().await
            };
            tokio::time::timeout(NODE_INPUT_TIMEOUT, operation)
                .await
                .map_err(|_| {
                    RunManagerError::NodeInputUnavailable(format!(
                        "timed out writing to {}",
                        socket.display()
                    ))
                })?
                .map_err(|error| {
                    RunManagerError::NodeInputUnavailable(format!(
                        "failed to write {}: {error}",
                        socket.display()
                    ))
                })?;
            Ok(payload.len())
        }
    }
}

fn serial_socket_from_manifest(
    adapter: &NodeInputDefinition,
    manifest: &Path,
    node_id: &str,
) -> Result<PathBuf, RunManagerError> {
    let source = fs::read_to_string(manifest).map_err(|error| {
        RunManagerError::NodeInputUnavailable(format!(
            "serial manifest is not ready at {}: {error}",
            manifest.display()
        ))
    })?;
    let variable = format!("{}_SERIAL_SOCKET", node_id.to_ascii_uppercase());
    let line_prefix = format!("export {variable}='");
    let mut matches = source.lines().filter_map(|line| {
        line.strip_prefix(&line_prefix)
            .and_then(|value| value.strip_suffix('\''))
    });
    let socket = matches.next().ok_or_else(|| {
        RunManagerError::NodeInputUnavailable(format!(
            "serial manifest has no endpoint for {node_id}"
        ))
    })?;
    if matches.next().is_some() {
        return Err(RunManagerError::NodeInputUnavailable(format!(
            "serial manifest has duplicate endpoints for {node_id}"
        )));
    }
    let path = PathBuf::from(socket);
    validate_serial_socket_path(&path, node_id, &adapter.socket_path_prefix)?;
    Ok(path)
}

fn validate_serial_socket_path(
    path: &Path,
    node_id: &str,
    allowed_prefix: &str,
) -> Result<(), RunManagerError> {
    let value = path.to_string_lossy();
    let expected_name_prefix = format!("{node_id}.");
    let valid_shape = value.starts_with(allowed_prefix)
        && path
            .parent()
            .and_then(Path::file_name)
            .and_then(|name| name.to_str())
            == Some("serial")
        && path
            .file_name()
            .and_then(|name| name.to_str())
            .is_some_and(|name| name.starts_with(&expected_name_prefix) && name.ends_with(".sock"));
    if !valid_shape {
        return Err(RunManagerError::NodeInputUnavailable(format!(
            "serial endpoint is outside the registered node socket namespace: {}",
            path.display()
        )));
    }
    let metadata = fs::metadata(path).map_err(|error| {
        RunManagerError::NodeInputUnavailable(format!(
            "serial endpoint is not ready at {}: {error}",
            path.display()
        ))
    })?;
    if !metadata.file_type().is_socket() {
        return Err(RunManagerError::NodeInputUnavailable(format!(
            "serial endpoint is not a UNIX socket: {}",
            path.display()
        )));
    }
    Ok(())
}

const REMOTE_NODE_INPUT_WRITER: &str = r#"import os
import socket
import stat
import sys
import time

manifest, node, allowed_prefix, max_bytes = sys.argv[1:]
max_bytes = int(max_bytes)
key = node.upper() + "_SERIAL_SOCKET"
line_prefix = "export " + key + "='"
matches = []
with open(manifest, "r", encoding="utf-8") as source:
    for raw_line in source:
        line = raw_line.rstrip("\n")
        if line.startswith(line_prefix) and line.endswith("'"):
            matches.append(line[len(line_prefix):-1])
if len(matches) != 1:
    raise RuntimeError("serial manifest endpoint count is " + str(len(matches)))
path = matches[0]
name = os.path.basename(path)
if not path.startswith(allowed_prefix):
    raise RuntimeError("serial endpoint is outside the registered namespace")
if os.path.basename(os.path.dirname(path)) != "serial":
    raise RuntimeError("serial endpoint parent is not serial")
if not name.startswith(node + ".") or not name.endswith(".sock"):
    raise RuntimeError("serial endpoint does not match the selected node")
if not stat.S_ISSOCK(os.stat(path).st_mode):
    raise RuntimeError("serial endpoint is not a UNIX socket")
payload = sys.stdin.buffer.read(max_bytes + 1)
if not payload or len(payload) > max_bytes:
    raise RuntimeError("invalid serial payload length")
stream = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
stream.settimeout(5)
try:
    stream.connect(path)
    stream.sendall(payload)
    time.sleep(0.2)
finally:
    stream.close()
sys.stdout.write(str(len(payload)))
"#;

async fn send_remote_node_input(
    target: &ExecutionTarget,
    adapter: &NodeInputDefinition,
    manifest: &Path,
    node_id: &str,
    payload: &[u8],
) -> Result<usize, RunManagerError> {
    match adapter.kind {
        NodeInputKind::QemuSerialEnv => {}
    }
    let remote_command = remote_node_input_command(target, adapter, manifest, node_id)?;
    let timeout = Duration::from_secs(target.connect_timeout_secs.unwrap_or(10) + 10);
    let operation = async {
        let mut command = ssh_command(target)?;
        command
            .arg(remote_command)
            .stdin(Stdio::piped())
            .stdout(Stdio::piped())
            .stderr(Stdio::piped());
        let mut child = command.spawn()?;
        let mut stdin = child.stdin.take().ok_or_else(|| {
            RunManagerError::Io(std::io::Error::other("SSH stdin was not created"))
        })?;
        stdin.write_all(payload).await?;
        stdin.shutdown().await?;
        drop(stdin);
        Ok::<_, RunManagerError>(child.wait_with_output().await?)
    };
    let output = tokio::time::timeout(timeout, operation)
        .await
        .map_err(|_| {
            RunManagerError::NodeInputUnavailable(format!(
                "timed out writing {node_id} serial input on {}",
                target.id
            ))
        })??;
    if !output.status.success() {
        return Err(RunManagerError::NodeInputUnavailable(format!(
            "remote serial writer failed on {}: {}",
            target.id,
            String::from_utf8_lossy(&output.stderr).trim()
        )));
    }
    let bytes_written = String::from_utf8_lossy(&output.stdout)
        .trim()
        .parse::<usize>()
        .map_err(|_| {
            RunManagerError::NodeInputUnavailable(
                "remote serial writer returned an invalid byte count".to_string(),
            )
        })?;
    if bytes_written != payload.len() {
        return Err(RunManagerError::NodeInputUnavailable(format!(
            "remote serial writer reported {bytes_written} of {} bytes",
            payload.len()
        )));
    }
    Ok(bytes_written)
}

fn remote_node_input_command(
    target: &ExecutionTarget,
    adapter: &NodeInputDefinition,
    manifest: &Path,
    node_id: &str,
) -> Result<String, RunManagerError> {
    let repo_root = remote_repo_root(target)?;
    let remote_manifest = format!(
        "{}/{}",
        repo_root.trim_end_matches('/'),
        manifest.to_string_lossy()
    );
    Ok(format!(
        "python3 -c {} {} {} {} {}",
        shell_quote(REMOTE_NODE_INPUT_WRITER),
        shell_quote(&remote_manifest),
        shell_quote(node_id),
        shell_quote(&adapter.socket_path_prefix),
        MAX_NODE_INPUT_BYTES
    ))
}

fn append_node_input_event(
    process_log: &Path,
    node_id: &str,
    bytes_written: usize,
) -> Result<(), RunManagerError> {
    let mut log = OpenOptions::new()
        .create(true)
        .append(true)
        .open(process_log)?;
    writeln!(
        log,
        "[sim-console] node_input node={node_id} bytes={bytes_written} status=sent"
    )?;
    Ok(())
}

pub fn run_remote_worker(plan_path: &Path) -> Result<(), RunManagerError> {
    let plan: RemoteRunPlan = serde_json::from_slice(&fs::read(plan_path)?)?;
    let source_head = plan.source_head.clone();
    let run_dir = plan_path
        .parent()
        .ok_or_else(|| RunManagerError::UnsafePath(plan_path.display().to_string()))?;
    let bundle_path = run_dir.join("source.bundle");
    println!(
        "[sim-console] preparing target={} source_head={}",
        plan.target.id,
        short_revision(&source_head)
    );
    let bundle_ref = format!("refs/sim-console/runs/{}", plan.run_id);
    run_checked(
        StdCommand::new("git")
            .arg("-C")
            .arg(&plan.local_repo_root)
            .args(["update-ref", &bundle_ref, &source_head]),
        "pin source revision",
    )?;
    let bundle_result = run_checked(
        StdCommand::new("git")
            .arg("-C")
            .arg(&plan.local_repo_root)
            .args(["bundle", "create"])
            .arg(&bundle_path)
            .arg(&bundle_ref),
        "create source bundle",
    );
    let unpin_result = run_checked(
        StdCommand::new("git")
            .arg("-C")
            .arg(&plan.local_repo_root)
            .args(["update-ref", "-d", &bundle_ref]),
        "unpin source revision",
    );
    bundle_result?;
    unpin_result?;

    let repo_root = remote_repo_root(&plan.target)?;
    let workspace_parent = Path::new(repo_root)
        .parent()
        .and_then(Path::to_str)
        .ok_or_else(|| RunManagerError::UnknownTarget(plan.target.id.clone()))?;
    let remote_bundle = format!("{workspace_parent}/source-{}.bundle", plan.run_id);
    let receive = format!(
        "set -eu; mkdir -p {}; cat > {}",
        shell_quote(workspace_parent),
        shell_quote(&remote_bundle)
    );
    let bundle = File::open(&bundle_path)?;
    let mut transfer = ssh_command_std(&plan.target)?;
    transfer
        .arg(receive)
        .stdin(Stdio::from(bundle))
        .stdout(Stdio::inherit())
        .stderr(Stdio::inherit());
    run_checked(&mut transfer, "transfer source bundle")?;

    let source_repo = remote_workspace_source_repo(&plan.target)?;
    let mut mirror_fetches = String::new();
    for mirror in &plan.target.submodule_mirrors {
        let expected =
            submodule_commit_at_revision(&plan.local_repo_root, &source_head, &mirror.path)?;
        let target_path = format!("{repo_root}/{}", mirror.path);
        let source_path = format!("{source_repo}/{}", mirror.path);
        mirror_fetches.push_str(&remote_mirror_fetch_command(
            mirror,
            &target_path,
            &source_path,
            &expected,
        ));
    }
    let bootstrap_file_copies = plan
        .target
        .bootstrap_files
        .iter()
        .map(|relative_path| {
            let source_file = format!("{source_repo}/{relative_path}");
            let target_file = format!("{repo_root}/{relative_path}");
            format!(
                "test -f {source_file}; mkdir -p {target_parent}; cp -f {source_file} {target_file}; ",
                source_file = shell_quote(&source_file),
                target_parent = shell_quote(
                    Path::new(&target_file)
                        .parent()
                        .and_then(Path::to_str)
                        .expect("validated bootstrap file has a parent")
                ),
                target_file = shell_quote(&target_file),
            )
        })
        .collect::<String>();
    let prepare_body = format!(
        "set -eu; source_repo={source_repo}; workspace={workspace}; bundle={bundle}; bundle_ref={bundle_ref}; head={head}; cleanup() {{ rm -f \"$bundle\"; }}; trap cleanup EXIT; git -c fetch.recurseSubmodules=false -C \"$source_repo\" fetch \"$bundle\" \"$bundle_ref\"; git -C \"$source_repo\" cat-file -e \"$head^{{commit}}\"; if git -C \"$workspace\" rev-parse --git-dir >/dev/null 2>&1; then git -C \"$workspace\" reset --hard \"$head\"; else if [ -e \"$workspace\" ]; then printf 'managed workspace exists but is not a Git worktree: %s\\n' \"$workspace\" >&2; exit 1; fi; git -C \"$source_repo\" worktree add --detach \"$workspace\" \"$head\"; fi; {bootstrap_file_copies} git -C \"$workspace\" submodule sync; git -C \"$workspace\" submodule init; git -C \"$workspace\" config --file .gitmodules --get-regexp '^submodule\\..*\\.path$' | while read -r key path; do source_path=\"$source_repo/$path\"; target_path=\"$workspace/$path\"; if ! git -C \"$target_path\" rev-parse --git-dir >/dev/null 2>&1 && git -C \"$source_path\" rev-parse --git-dir >/dev/null 2>&1; then rm -rf \"$target_path\"; mkdir -p \"$(dirname \"$target_path\")\"; git clone --shared --no-checkout \"$source_path\" \"$target_path\"; fi; done; {mirror_fetches} git -C \"$workspace\" submodule update --init --force; if [ -f \"$workspace/mem_service/.gitmodules\" ]; then git -C \"$workspace/mem_service\" submodule sync; git -C \"$workspace/mem_service\" submodule init; nested_source=\"$source_repo/mem_service/vendor/obmm\"; nested_target=\"$workspace/mem_service/vendor/obmm\"; if ! git -C \"$nested_target\" rev-parse --git-dir >/dev/null 2>&1 && git -C \"$nested_source\" rev-parse --git-dir >/dev/null 2>&1; then rm -rf \"$nested_target\"; mkdir -p \"$(dirname \"$nested_target\")\"; git clone --shared --no-checkout \"$nested_source\" \"$nested_target\"; fi; git -C \"$workspace/mem_service\" submodule update --init --force; fi; printf '[sim-console] target workspace ready head=%s path=%s\\n' \"$head\" \"$workspace\"",
        source_repo = shell_quote(source_repo),
        workspace = shell_quote(repo_root),
        bundle = shell_quote(&remote_bundle),
        bundle_ref = shell_quote(&bundle_ref),
        head = shell_quote(&source_head),
        bootstrap_file_copies = bootstrap_file_copies,
        mirror_fetches = mirror_fetches,
    );
    let prepare =
        remote_supervised_command(&plan.target, &plan.run_id, "prepare.pid", &prepare_body)?;
    let mut prepare_command = ssh_command_std(&plan.target)?;
    prepare_command
        .arg(prepare)
        .stdout(Stdio::inherit())
        .stderr(Stdio::inherit());
    run_checked(&mut prepare_command, "prepare remote worktree")?;
    fs::remove_file(&bundle_path).ok();

    println!(
        "[sim-console] launching run={} target={} repo={repo_root}",
        plan.run_id, plan.target.id
    );
    let mut launch = ssh_command_std(&plan.target)?;
    launch
        .arg(remote_launch_command(
            &plan.target,
            &plan.resolved,
            &plan.run_id,
        )?)
        .stdin(Stdio::null())
        .stdout(Stdio::inherit())
        .stderr(Stdio::inherit());
    let status = launch.status()?;
    if !status.success() {
        return Err(RunManagerError::Io(std::io::Error::other(format!(
            "remote launcher exited with {status}"
        ))));
    }
    Ok(())
}

fn run_checked(command: &mut StdCommand, action: &str) -> Result<(), RunManagerError> {
    let status = command.status()?;
    if status.success() {
        return Ok(());
    }
    Err(RunManagerError::Io(std::io::Error::other(format!(
        "{action} failed with {status}"
    ))))
}

fn run_checked_output(
    command: &mut StdCommand,
    action: &str,
) -> Result<std::process::Output, RunManagerError> {
    let output = command.output()?;
    if output.status.success() {
        return Ok(output);
    }
    Err(RunManagerError::Io(std::io::Error::other(format!(
        "{action} failed with {}: {}",
        output.status,
        String::from_utf8_lossy(&output.stderr).trim()
    ))))
}

fn ssh_command_std(target: &ExecutionTarget) -> Result<StdCommand, RunManagerError> {
    let host = target
        .ssh_host
        .as_deref()
        .ok_or_else(|| RunManagerError::UnknownTarget(target.id.clone()))?;
    let mut command = StdCommand::new("ssh");
    command
        .arg("-T")
        .arg("-o")
        .arg("BatchMode=yes")
        .arg("-o")
        .arg(format!(
            "ConnectTimeout={}",
            target.connect_timeout_secs.unwrap_or(10)
        ))
        .arg("--")
        .arg(host);
    Ok(command)
}

fn git_head(repo_root: &Path) -> Result<String, RunManagerError> {
    let output = StdCommand::new("git")
        .arg("-C")
        .arg(repo_root)
        .args(["rev-parse", "HEAD"])
        .output()?;
    if !output.status.success() {
        return Err(RunManagerError::Io(std::io::Error::other(format!(
            "failed to resolve local HEAD: {}",
            String::from_utf8_lossy(&output.stderr).trim()
        ))));
    }
    Ok(String::from_utf8_lossy(&output.stdout).trim().to_string())
}

fn submodule_commit_at_head(repo_root: &Path, path: &str) -> Result<String, RunManagerError> {
    submodule_commit_at_revision(repo_root, "HEAD", path)
}

fn submodule_commit_at_revision(
    repo_root: &Path,
    revision: &str,
    path: &str,
) -> Result<String, RunManagerError> {
    let output = StdCommand::new("git")
        .arg("-C")
        .arg(repo_root)
        .args(["ls-tree", revision, "--", path])
        .output()?;
    if !output.status.success() {
        return Err(RunManagerError::Io(std::io::Error::other(format!(
            "failed to inspect submodule {path}: {}",
            String::from_utf8_lossy(&output.stderr).trim()
        ))));
    }
    let line = String::from_utf8_lossy(&output.stdout);
    let fields = line.split_whitespace().collect::<Vec<_>>();
    if fields.len() < 4 || fields[0] != "160000" || fields[1] != "commit" {
        return Err(RunManagerError::Io(std::io::Error::other(format!(
            "target mirror path is not a submodule at HEAD: {path}"
        ))));
    }
    Ok(fields[2].to_string())
}

fn submodule_checkout_mismatch(
    repo_root: &Path,
    path: &str,
) -> Result<Option<(String, String)>, RunManagerError> {
    let expected = submodule_commit_at_head(repo_root, path)?;
    let checkout = repo_root.join(path);
    if !checkout.join(".git").exists() {
        return Ok(None);
    }
    let output = StdCommand::new("git")
        .arg("-C")
        .arg(&checkout)
        .args(["rev-parse", "HEAD"])
        .output()?;
    if !output.status.success() {
        return Ok(None);
    }
    let actual = String::from_utf8_lossy(&output.stdout).trim().to_string();
    Ok((actual != expected).then_some((expected, actual)))
}

fn ssh_command(target: &ExecutionTarget) -> Result<Command, RunManagerError> {
    let host = target
        .ssh_host
        .as_deref()
        .ok_or_else(|| RunManagerError::UnknownTarget(target.id.clone()))?;
    let mut command = Command::new("ssh");
    command
        .arg("-T")
        .arg("-o")
        .arg("BatchMode=yes")
        .arg("-o")
        .arg(format!(
            "ConnectTimeout={}",
            target.connect_timeout_secs.unwrap_or(10)
        ))
        .arg("--")
        .arg(host);
    Ok(command)
}

async fn run_remote_command_output(
    target: &ExecutionTarget,
    body: String,
    timeout: Duration,
    action: &str,
) -> Result<std::process::Output, RunManagerError> {
    let mut command = ssh_command(target)?;
    command.arg(body);
    let output = tokio::time::timeout(timeout, command.output())
        .await
        .map_err(|_| {
            RunManagerError::Io(std::io::Error::new(
                std::io::ErrorKind::TimedOut,
                format!("{action} timed out on {}", target.id),
            ))
        })??;
    if output.status.success() {
        return Ok(output);
    }
    Err(RunManagerError::Io(std::io::Error::other(format!(
        "{action} failed on {}: {}",
        target.id,
        String::from_utf8_lossy(&output.stderr).trim()
    ))))
}

fn remote_target_bootstrap_command(target: &ExecutionTarget) -> Result<String, RunManagerError> {
    let source_repo = remote_workspace_source_repo(target)?;
    let source_url = target
        .source_repo_url
        .as_deref()
        .ok_or_else(|| RunManagerError::UnknownTarget(target.id.clone()))?;
    let source_parent = Path::new(source_repo)
        .parent()
        .and_then(Path::to_str)
        .ok_or_else(|| RunManagerError::UnknownTarget(target.id.clone()))?;
    let mut command = format!(
        "set -eu; export PATH=\"$HOME/.cargo/bin:$HOME/.local/bin:$PATH\"; source_repo={source_repo}; source_url={source_url}; if git -C \"$source_repo\" rev-parse --git-dir >/dev/null 2>&1; then printf 'SOURCE_REPO_REUSED\\n'; elif [ -e \"$source_repo\" ]; then printf 'source path exists but is not a Git repository: %s\\n' \"$source_repo\" >&2; exit 1; else mkdir -p {source_parent}; clone_attempt=1; while ! git -c http.version=HTTP/1.1 clone --no-checkout --filter=blob:none \"$source_url\" \"$source_repo\"; do if [ \"$clone_attempt\" -ge 3 ]; then exit 1; fi; rm -rf \"$source_repo\"; clone_attempt=$((clone_attempt + 1)); sleep 2; done; printf 'SOURCE_REPO_CREATED\\n'; fi; ",
        source_repo = shell_quote(source_repo),
        source_url = shell_quote(source_url),
        source_parent = shell_quote(source_parent),
    );
    command.push_str(
        "if [ ! -x \"$HOME/.cargo/bin/cargo\" ]; then case \"$(uname -m)\" in aarch64|arm64) rust_arch=aarch64-unknown-linux-gnu;; x86_64) rust_arch=x86_64-unknown-linux-gnu;; *) printf 'unsupported Rust target architecture: %s\\n' \"$(uname -m)\" >&2; exit 1;; esac; rustup_dir=\"${TMPDIR:-/tmp}/sim-console-rustup.$$\"; rustup_init=\"$rustup_dir/rustup-init\"; cleanup_rustup() { rm -rf \"$rustup_dir\"; }; trap cleanup_rustup EXIT; mkdir -p \"$rustup_dir\"; rust_dist=https://mirrors.huaweicloud.com/rustup; rust_update_root=https://mirrors.huaweicloud.com/rustup/rustup; if ! curl --proto '=https' --tlsv1.2 -fsSL -o \"$rustup_init\" \"$rust_dist/rustup/dist/$rust_arch/rustup-init\"; then printf 'Huawei Cloud Rust mirror unavailable; falling back to the official source\\n' >&2; rust_dist=https://static.rust-lang.org; rust_update_root=https://static.rust-lang.org/rustup; curl --proto '=https' --tlsv1.2 -fsSL -o \"$rustup_init\" \"$rust_dist/rustup/dist/$rust_arch/rustup-init\"; fi; chmod 0700 \"$rustup_init\"; RUSTUP_DIST_SERVER=\"$rust_dist\" RUSTUP_UPDATE_ROOT=\"$rust_update_root\" \"$rustup_init\" -y --profile minimal --default-toolchain stable --no-modify-path; cleanup_rustup; trap - EXIT; printf 'TOOL_INSTALLED\\tcargo\\n'; fi; export PATH=\"$HOME/.cargo/bin:$HOME/.local/bin:$PATH\"; ",
    );
    command.push_str(
        "simpler_cxx_major=missing; if command -v g++-15 >/dev/null 2>&1; then simpler_cxx_major=\"$(g++-15 -dumpfullversion -dumpversion 2>/dev/null | cut -d. -f1 || true)\"; fi; if [ \"$simpler_cxx_major\" != 15 ]; then case \"$(uname -m)\" in aarch64|arm64) mamba_arch=linux-aarch64; gcc_package=gcc_linux-aarch64; gxx_package=gxx_linux-aarch64;; x86_64) mamba_arch=linux-64; gcc_package=gcc_linux-64; gxx_package=gxx_linux-64;; *) printf 'unsupported GCC 15 target architecture: %s\\n' \"$(uname -m)\" >&2; exit 1;; esac; mkdir -p \"$HOME/.local/bin\" \"$HOME/.local/toolchains\"; micromamba=\"$HOME/.local/bin/micromamba\"; if [ ! -x \"$micromamba\" ]; then mamba_dir=\"${TMPDIR:-/tmp}/sim-console-micromamba.$$\"; rm -rf \"$mamba_dir\"; mkdir -p \"$mamba_dir\"; curl --proto '=https' --tlsv1.2 -fsSL \"https://micro.mamba.pm/api/micromamba/$mamba_arch/latest\" | tar -xj -C \"$mamba_dir\" bin/micromamba; install -m 0755 \"$mamba_dir/bin/micromamba\" \"$micromamba\"; rm -rf \"$mamba_dir\"; printf 'TOOL_INSTALLED\\tmicromamba\\n'; fi; gcc_prefix=\"$HOME/.local/toolchains/gcc15\"; \"$micromamba\" create -y --root-prefix \"$HOME/.local/share/mamba\" --prefix \"$gcc_prefix\" --channel conda-forge --override-channels \"$gcc_package=15\" \"$gxx_package=15\"; printf 'TOOL_INSTALLED\\tsimpler-gcc15\\n'; fi; gcc_prefix=\"$HOME/.local/toolchains/gcc15\"; gcc_path=\"$(find \"$gcc_prefix/bin\" -maxdepth 1 -name '*-gcc' -perm -u+x | head -n 1)\"; gxx_path=\"$(find \"$gcc_prefix/bin\" -maxdepth 1 -name '*-g++' -perm -u+x | head -n 1)\"; if [ -z \"$gcc_path\" ] || [ -z \"$gxx_path\" ]; then printf 'Conda GCC 15 compiler entrypoints are missing under %s\\n' \"$gcc_prefix\" >&2; exit 1; fi; for name in cc gcc g++ c++; do link=\"$HOME/.local/bin/$name\"; if [ -L \"$link\" ]; then target=\"$(readlink \"$link\")\"; case \"$target\" in \"$gcc_prefix\"/bin/*) rm -f \"$link\";; esac; fi; done; ln -sfn \"$gcc_path\" \"$HOME/.local/bin/gcc-15\"; ln -sfn \"$gxx_path\" \"$HOME/.local/bin/g++-15\"; simpler_cxx_major=\"$(g++-15 -dumpfullversion -dumpversion 2>/dev/null | cut -d. -f1 || true)\"; if [ \"$simpler_cxx_major\" != 15 ]; then printf 'required simpler compiler is not GCC 15 after preparation: g++-15=%s\\n' \"${simpler_cxx_major:-missing}\" >&2; exit 1; fi; ",
    );
    command.push_str(&format!(
        "if ! command -v ninja >/dev/null 2>&1; then python3 -m pip install --user ninja; printf 'TOOL_INSTALLED\\tninja\\n'; fi; native_deps_ready=1; for tool in {tools}; do command -v \"$tool\" >/dev/null 2>&1 || native_deps_ready=0; done; if command -v pkg-config >/dev/null 2>&1; then for pkg in {pkg_config_deps}; do pkg-config --exists \"$pkg\" || native_deps_ready=0; done; else native_deps_ready=0; fi; if [ \"$native_deps_ready\" -eq 0 ]; then command -v dnf >/dev/null 2>&1 || {{ printf 'native build dependencies are missing and dnf is unavailable\\n' >&2; exit 1; }}; sudo -n dnf -y install {packages}; printf 'TOOL_INSTALLED\\tnative-build-deps\\n'; fi; export PATH=\"$HOME/.cargo/bin:$HOME/.local/bin:$PATH\"; for tool in {tools}; do command -v \"$tool\" >/dev/null 2>&1 || {{ printf 'required tool is still missing after preparation: %s\\n' \"$tool\" >&2; exit 1; }}; done; for pkg in {pkg_config_deps}; do pkg-config --exists \"$pkg\" || {{ printf 'required pkg-config dependency is still missing after preparation: %s\\n' \"$pkg\" >&2; exit 1; }}; done; printf 'TARGET_TOOLS_READY\\n'",
        tools = TARGET_NATIVE_TOOLS,
        pkg_config_deps = TARGET_PKG_CONFIG_DEPS,
        packages = OPENEULER_BUILD_PACKAGES,
    ));
    Ok(command)
}

fn remote_source_mirror_prepare_command(
    mirror: &crate::target::SubmoduleMirror,
    remote_repo: &str,
    expected: &str,
) -> String {
    let parent = Path::new(remote_repo)
        .parent()
        .and_then(Path::to_str)
        .unwrap_or(remote_repo);
    let mirror_ref = format!("refs/sim-console/mirrors/{expected}");
    format!(
        "set -eu; repo={repo}; if git -C \"$repo\" rev-parse --git-dir >/dev/null 2>&1; then :; elif [ -e \"$repo\" ]; then printf 'source mirror path exists but is not a Git repository: %s\\n' \"$repo\" >&2; exit 1; else mkdir -p {parent}; git init \"$repo\"; fi; if ! git -C \"$repo\" cat-file -e {expected}^{{commit}} 2>/dev/null; then fetch_ok=0; if git -c http.version=HTTP/1.1 -c http.lowSpeedLimit=1024 -c http.lowSpeedTime=30 -c fetch.recurseSubmodules=false -C \"$repo\" fetch --filter=blob:none --no-tags {url} {git_ref}:{git_ref}; then fetch_ok=1; fi; if ! git -C \"$repo\" cat-file -e {expected}^{{commit}} 2>/dev/null && [ \"$fetch_ok\" -eq 1 ]; then git -c http.version=HTTP/1.1 -c http.lowSpeedLimit=1024 -c http.lowSpeedTime=30 -c fetch.recurseSubmodules=false -C \"$repo\" fetch --no-tags {url} {expected}:{mirror_ref} || true; fi; fi; if git -C \"$repo\" cat-file -e {expected}^{{commit}} 2>/dev/null; then git -C \"$repo\" update-ref {mirror_ref} {expected}; fi; if GIT_NO_LAZY_FETCH=1 git -C \"$repo\" checkout --detach --force {expected} >/dev/null 2>&1; then printf 'MIRROR_READY\\n'; else printf 'MIRROR_NEEDS_CHECKOUT_PACK\\n'; fi",
        repo = shell_quote(remote_repo),
        parent = shell_quote(parent),
        url = shell_quote(&mirror.fetch_url),
        git_ref = shell_quote(&mirror.git_ref),
        expected = shell_quote(expected),
        mirror_ref = shell_quote(&mirror_ref),
    )
}

fn create_checkout_pack(
    local_repo: &Path,
    expected: &str,
    pack_path: &Path,
) -> Result<(), RunManagerError> {
    if pack_path.is_file() && fs::metadata(pack_path)?.len() > 0 {
        return Ok(());
    }
    run_checked(
        StdCommand::new("git").arg("-C").arg(local_repo).args([
            "cat-file",
            "-e",
            &format!("{expected}^{{commit}}"),
        ]),
        "locate local submodule commit",
    )?;
    if let Some(parent) = pack_path.parent() {
        fs::create_dir_all(parent)?;
    }
    let temporary_pack = pack_path.with_extension(format!("pack.tmp.{}", std::process::id()));
    fs::remove_file(&temporary_pack).ok();

    let root_tree = run_checked_output(
        StdCommand::new("git")
            .arg("-C")
            .arg(local_repo)
            .args(["rev-parse", &format!("{expected}^{{tree}}")]),
        "locate local submodule root tree",
    )?;
    let tree_listing = run_checked_output(
        StdCommand::new("git")
            .arg("-C")
            .arg(local_repo)
            .args(["ls-tree", "-r", "-t", expected]),
        "enumerate local submodule checkout objects",
    )?;
    let mut object_ids = BTreeSet::from([
        expected.to_string(),
        String::from_utf8_lossy(&root_tree.stdout)
            .trim()
            .to_string(),
    ]);
    for line in String::from_utf8_lossy(&tree_listing.stdout).lines() {
        let mut fields = line.split_whitespace();
        let _mode = fields.next();
        let object_type = fields.next();
        let object_id = fields.next();
        if matches!(object_type, Some("tree" | "blob")) {
            if let Some(object_id) = object_id {
                object_ids.insert(object_id.to_string());
            }
        }
    }

    let pack_file = File::create(&temporary_pack)?;
    let mut child = StdCommand::new("git")
        .arg("-C")
        .arg(local_repo)
        .args(["pack-objects", "--stdout", "--delta-base-offset"])
        .stdin(Stdio::piped())
        .stdout(Stdio::from(pack_file))
        .stderr(Stdio::piped())
        .spawn()?;
    {
        let stdin = child.stdin.as_mut().ok_or_else(|| {
            RunManagerError::Io(std::io::Error::other("checkout pack process has no stdin"))
        })?;
        for object_id in object_ids {
            writeln!(stdin, "{object_id}")?;
        }
    }
    let output = child.wait_with_output()?;
    if !output.status.success() {
        fs::remove_file(&temporary_pack).ok();
        return Err(RunManagerError::Io(std::io::Error::other(format!(
            "create target checkout pack failed: {}",
            String::from_utf8_lossy(&output.stderr).trim()
        ))));
    }
    fs::rename(temporary_pack, pack_path)?;
    Ok(())
}

async fn transfer_target_file(
    target: &ExecutionTarget,
    local_file: &Path,
    remote_file: &str,
) -> Result<(), RunManagerError> {
    let parent = Path::new(remote_file)
        .parent()
        .and_then(Path::to_str)
        .ok_or_else(|| RunManagerError::UnknownTarget(target.id.clone()))?;
    let local_size = fs::metadata(local_file)?.len();
    let size_output = run_remote_command_output(
        target,
        remote_file_size_command(remote_file),
        Duration::from_secs(60),
        "inspect partial target preparation file",
    )
    .await?;
    let mut remote_size = String::from_utf8_lossy(&size_output.stdout)
        .trim()
        .parse::<u64>()
        .map_err(|error| {
            RunManagerError::Io(std::io::Error::other(format!(
                "invalid remote preparation file size on {}: {error}",
                target.id
            )))
        })?;
    if remote_size > local_size {
        run_remote_command_output(
            target,
            format!("set -eu; rm -f {}", shell_quote(remote_file)),
            Duration::from_secs(60),
            "discard oversized target preparation file",
        )
        .await?;
        remote_size = 0;
    }
    if remote_size == local_size {
        return Ok(());
    }

    let receive = remote_file_receive_command(parent, remote_file, remote_size, local_size);
    let mut file = File::open(local_file)?;
    file.seek(SeekFrom::Start(remote_size))?;
    let mut command = ssh_command(target)?;
    command
        .arg(receive)
        .stdin(Stdio::from(file))
        .stdout(Stdio::piped())
        .stderr(Stdio::piped());
    let output = tokio::time::timeout(TARGET_PREPARATION_TIMEOUT, command.output())
        .await
        .map_err(|_| {
            RunManagerError::Io(std::io::Error::new(
                std::io::ErrorKind::TimedOut,
                format!(
                    "transfer target preparation file timed out on {}",
                    target.id
                ),
            ))
        })??;
    if output.status.success() {
        return Ok(());
    }
    Err(RunManagerError::Io(std::io::Error::other(format!(
        "transfer target preparation file failed on {}: {}",
        target.id,
        String::from_utf8_lossy(&output.stderr).trim()
    ))))
}

fn remote_file_size_command(remote_file: &str) -> String {
    format!(
        "set -eu; file={}; if [ -f \"$file\" ]; then stat -c %s \"$file\"; else printf '0\\n'; fi",
        shell_quote(remote_file)
    )
}

fn remote_file_receive_command(
    parent: &str,
    remote_file: &str,
    expected_offset: u64,
    expected_size: u64,
) -> String {
    format!(
        "set -eu; mkdir -p {parent}; file={file}; actual=$(stat -c %s \"$file\" 2>/dev/null || printf '0'); [ \"$actual\" -eq {expected_offset} ] || {{ printf 'remote preparation file changed before resume: expected=%s actual=%s\\n' {expected_offset} \"$actual\" >&2; exit 1; }}; cat >> \"$file\"; actual=$(stat -c %s \"$file\"); [ \"$actual\" -eq {expected_size} ] || {{ printf 'remote preparation file has unexpected final size: expected=%s actual=%s\\n' {expected_size} \"$actual\" >&2; exit 1; }}",
        parent = shell_quote(parent),
        file = shell_quote(remote_file),
    )
}

fn remote_source_mirror_import_checkout_pack_command(
    remote_repo: &str,
    remote_pack: &str,
    expected: &str,
) -> String {
    let mirror_ref = format!("refs/sim-console/mirrors/{expected}");
    format!(
        "set -eu; cleanup() {{ rm -f {pack}; }}; trap cleanup EXIT; git -C {repo} index-pack --stdin < {pack}; git -C {repo} cat-file -e {expected}^{{commit}}; git -C {repo} update-ref {mirror_ref} {expected}; GIT_NO_LAZY_FETCH=1 git -C {repo} checkout --detach --force {expected}; printf 'MIRROR_CHECKOUT_READY\\n'",
        repo = shell_quote(remote_repo),
        pack = shell_quote(remote_pack),
        mirror_ref = shell_quote(&mirror_ref),
        expected = shell_quote(expected),
    )
}

fn remote_probe_command(
    target: &ExecutionTarget,
    local_repo_root: &Path,
) -> Result<String, RunManagerError> {
    let source_repo = remote_workspace_source_repo(target)?;
    let repo_root = remote_repo_root(target)?;
    let workspace_parent = Path::new(repo_root)
        .parent()
        .and_then(Path::to_str)
        .ok_or_else(|| RunManagerError::UnknownTarget(target.id.clone()))?;
    let mut command = format!(
        "set -eu; export PATH=\"$HOME/.cargo/bin:$HOME/.local/bin:$PATH\"; source_repo={}; if ! git -C \"$source_repo\" rev-parse --git-dir >/dev/null 2>&1; then printf 'SOURCE_REPO_MISSING\\n'; exit 0; fi; mkdir -p {}; ",
        shell_quote(source_repo),
        shell_quote(workspace_parent),
    );
    command.push_str(&format!(
        "for tool in {tools}; do command -v \"$tool\" >/dev/null 2>&1 || printf 'TOOL_MISSING\\t%s\\n' \"$tool\"; done; if command -v pkg-config >/dev/null 2>&1; then for pkg in {pkg_config_deps}; do pkg-config --exists \"$pkg\" || printf 'TOOL_MISSING\\tpkg-config:%s\\n' \"$pkg\"; done; fi; ",
        tools = TARGET_NATIVE_TOOLS,
        pkg_config_deps = TARGET_PKG_CONFIG_DEPS,
    ));
    command.push_str(
        "simpler_cxx=g++-15; simpler_cxx_major=missing; if command -v \"$simpler_cxx\" >/dev/null 2>&1; then simpler_cxx_major=\"$($simpler_cxx -dumpfullversion -dumpversion 2>/dev/null | cut -d. -f1 || true)\"; fi; if [ \"$simpler_cxx_major\" != 15 ]; then printf 'TOOL_VERSION_MISMATCH\\t%s\\tGCC-major-15\\t%s\\n' \"$simpler_cxx\" \"$simpler_cxx_major\"; fi; ",
    );
    for relative_path in &target.bootstrap_files {
        let remote_file = format!("{source_repo}/{relative_path}");
        command.push_str(&format!(
            "if [ ! -f {path} ]; then printf 'BOOTSTRAP_FILE_MISSING\\t%s\\n' {display_path}; fi; ",
            path = shell_quote(&remote_file),
            display_path = shell_quote(relative_path),
        ));
    }
    for (model_source, path) in &target.model_sources {
        command.push_str(&format!(
            "if [ ! -e {path} ]; then printf 'MODEL_SOURCE_MISSING\\t%s\\t%s\\n' {model_source} {path}; fi; ",
            model_source = shell_quote(model_source),
            path = shell_quote(path),
        ));
    }
    if let Some(path) = &target.open_euler_disk_image {
        command.push_str(&format!(
            "if [ ! -f {path} ]; then printf 'OPEN_EULER_DISK_IMAGE_MISSING\\n'; fi; ",
            path = shell_quote(path),
        ));
    }
    for mirror in &target.submodule_mirrors {
        let expected = submodule_commit_at_head(local_repo_root, &mirror.path)?;
        let source_path = format!("{source_repo}/{}", mirror.path);
        command.push_str(&format!(
            "if ! git -C {path} cat-file -e {expected}^{{commit}} 2>/dev/null || [ \"$(git -C {path} rev-parse HEAD 2>/dev/null || true)\" != {expected} ] || ! GIT_NO_LAZY_FETCH=1 git -C {path} diff-index --quiet {expected} --; then printf 'SUBMODULE_CACHE_MISSING\\t%s\\t%s\\n' {display_path} {display_expected}; fi; ",
            path = shell_quote(&source_path),
            expected = shell_quote(&expected),
            display_path = shell_quote(&mirror.path),
            display_expected = shell_quote(&expected),
        ));
    }
    command.push_str("printf 'PROBE_OK\\n'");
    Ok(command)
}

fn inject_target_model_source(
    demo: &DemoDefinition,
    target: &ExecutionTarget,
    resolved: &mut ResolvedCommand,
) -> Result<(), RunManagerError> {
    let Some(model_source) = &demo.model_source else {
        return Ok(());
    };
    let path = target.model_sources.get(model_source).ok_or_else(|| {
        RunManagerError::MissingRequirement(format!(
            "target {} does not configure model source {model_source}",
            target.id
        ))
    })?;
    resolved.args.push("--model".to_string());
    resolved.args.push(path.clone());
    Ok(())
}

fn inject_target_guest_engine(
    demo: &DemoDefinition,
    target: &ExecutionTarget,
    resolved: &mut ResolvedCommand,
) -> Result<(), RunManagerError> {
    if demo.guest_engine != GuestEngine::OpenEuler {
        return Ok(());
    }
    let path = target.open_euler_disk_image.as_ref().ok_or_else(|| {
        RunManagerError::MissingRequirement(format!(
            "target {} does not configure an openEuler disk image",
            target.id
        ))
    })?;
    resolved
        .args
        .extend(["--open-euler-disk-image".to_string(), path.clone()]);
    Ok(())
}

fn model_source_readiness_issue(
    demo: &DemoDefinition,
    target: &ExecutionTarget,
    missing_model_sources: &BTreeSet<String>,
) -> Option<ReadinessIssue> {
    let model_source = demo.model_source.as_ref()?;
    let Some(path) = target.model_sources.get(model_source) else {
        return Some(ReadinessIssue {
            code: "model_source_unconfigured".to_string(),
            message: format!(
                "Model source {model_source} is not configured for target {}",
                target.id
            ),
            remedy: format!(
                "Configure model_sources.{model_source} for target {}.",
                target.id
            ),
        });
    };
    if missing_model_sources.contains(model_source) {
        return Some(ReadinessIssue {
            code: "model_source_missing".to_string(),
            message: format!(
                "Model source {model_source} is missing on target {}: {path}",
                target.id
            ),
            remedy: format!("Install or synchronize {model_source} at the configured target path."),
        });
    }
    None
}

fn guest_engine_readiness_issue(
    demo: &DemoDefinition,
    target: &ExecutionTarget,
    disk_image_missing: bool,
) -> Option<ReadinessIssue> {
    if demo.guest_engine != GuestEngine::OpenEuler {
        return None;
    }
    let Some(path) = target.open_euler_disk_image.as_deref() else {
        return Some(ReadinessIssue {
            code: "open_euler_disk_image_unconfigured".to_string(),
            message: format!(
                "openEuler disk image is not configured for target {}",
                target.id
            ),
            remedy: format!("Configure open_euler_disk_image for target {}.", target.id),
        });
    };
    if disk_image_missing {
        return Some(ReadinessIssue {
            code: "open_euler_disk_image_missing".to_string(),
            message: format!(
                "openEuler disk image is missing on target {}: {path}",
                target.id
            ),
            remedy: "Install or synchronize the registered openEuler disk image.".to_string(),
        });
    }
    None
}

fn remote_mirror_fetch_command(
    mirror: &crate::target::SubmoduleMirror,
    target_path: &str,
    source_path: &str,
    expected: &str,
) -> String {
    let mirror_ref = format!("refs/sim-console/mirrors/{expected}");
    format!(
        "target_path={target_path}; source_path={source_path}; mirror_ref={mirror_ref}; if git -C \"$source_path\" cat-file -e {expected}^{{commit}} 2>/dev/null && ! git -C \"$target_path\" checkout --detach --force {expected} >/dev/null 2>&1; then rm -rf \"$target_path\"; mkdir -p \"$(dirname \"$target_path\")\"; git clone --shared --no-checkout \"$source_path\" \"$target_path\"; fi; if ! git -C \"$target_path\" cat-file -e {expected}^{{commit}} 2>/dev/null; then git -c fetch.recurseSubmodules=false -C \"$target_path\" fetch --no-tags {fetch_url} {git_ref}:\"$mirror_ref\" || true; fi; if ! git -C \"$target_path\" cat-file -e {expected}^{{commit}} 2>/dev/null; then git -c fetch.recurseSubmodules=false -C \"$target_path\" fetch --no-tags {fetch_url} {expected}:\"$mirror_ref\"; fi; git -C \"$target_path\" cat-file -e {expected}^{{commit}}; git -C \"$target_path\" update-ref \"$mirror_ref\" {expected}; git -C \"$target_path\" checkout --detach --force {expected}; ",
        target_path = shell_quote(target_path),
        source_path = shell_quote(source_path),
        mirror_ref = shell_quote(&mirror_ref),
        expected = shell_quote(expected),
        fetch_url = shell_quote(&mirror.fetch_url),
        git_ref = shell_quote(&mirror.git_ref),
    )
}

fn remote_launch_command(
    target: &ExecutionTarget,
    resolved: &ResolvedCommand,
    run_id: &str,
) -> Result<String, RunManagerError> {
    let repo_root = remote_repo_root(target)?;
    let remote_state = format!("out/sim-console/remote-runs/{run_id}");
    let mut launch = String::from("env");
    for (key, value) in &resolved.environment {
        launch.push(' ');
        launch.push_str(&shell_quote(&format!("{key}={value}")));
    }
    launch.push(' ');
    launch.push_str(&shell_quote(&format!("./{}", resolved.program)));
    for argument in &resolved.args {
        launch.push(' ');
        launch.push_str(&shell_quote(argument));
    }

    Ok(format!(
        "set -u; export PATH=\"$HOME/.cargo/bin:$HOME/.local/bin:$PATH\"; cd {repo}; run_dir={run_dir}; mkdir -p \"$run_dir\"; pid_file=\"$run_dir/process-group.pid\"; child_pid=''; cleanup() {{ rm -f \"$pid_file\"; }}; terminate() {{ trap - HUP INT TERM; if [ -n \"$child_pid\" ]; then kill -TERM -- \"-$child_pid\" 2>/dev/null || true; wait \"$child_pid\" 2>/dev/null || true; fi; exit 143; }}; trap cleanup EXIT; trap terminate HUP INT TERM; printf '[sim-console] target=%s run=%s remote_repo=%s\\n' {target_id} {run} {repo_display}; setsid {launch} & child_pid=$!; printf '%s\\n' \"$child_pid\" > \"$pid_file\"; wait \"$child_pid\"; status=$?; exit \"$status\"",
        repo = shell_quote(repo_root),
        run_dir = shell_quote(&remote_state),
        target_id = shell_quote(&target.id),
        run = shell_quote(run_id),
        repo_display = shell_quote(repo_root),
    ))
}

fn remote_stop_command(target: &ExecutionTarget, run_id: &str) -> Result<String, RunManagerError> {
    let repo_root = remote_repo_root(target)?;
    let prepare_pid = format!("{}/prepare.pid", remote_prepare_state_dir(target, run_id)?);
    let process_pid = format!("{repo_root}/out/sim-console/remote-runs/{run_id}/process-group.pid");
    Ok(format!(
        "set -eu; for pid_file in {} {}; do if [ -f \"$pid_file\" ]; then pid=$(cat \"$pid_file\"); case \"$pid\" in ''|*[!0-9]*) printf 'invalid remote pid file: %s\\n' \"$pid_file\" >&2; exit 1;; esac; kill -TERM -- \"-$pid\" 2>/dev/null || true; fi; done",
        shell_quote(&prepare_pid),
        shell_quote(&process_pid),
    ))
}

fn remote_supervised_command(
    target: &ExecutionTarget,
    run_id: &str,
    pid_name: &str,
    body: &str,
) -> Result<String, RunManagerError> {
    let run_dir = remote_prepare_state_dir(target, run_id)?;
    Ok(format!(
        "set -u; export PATH=\"$HOME/.cargo/bin:$HOME/.local/bin:$PATH\"; run_dir={run_dir}; mkdir -p \"$run_dir\"; pid_file=\"$run_dir/{pid_name}\"; child_pid=''; cleanup() {{ rm -f \"$pid_file\"; }}; terminate() {{ trap - HUP INT TERM; if [ -n \"$child_pid\" ]; then kill -TERM -- \"-$child_pid\" 2>/dev/null || true; wait \"$child_pid\" 2>/dev/null || true; fi; exit 143; }}; trap cleanup EXIT; trap terminate HUP INT TERM; setsid sh -c {body} & child_pid=$!; printf '%s\\n' \"$child_pid\" > \"$pid_file\"; wait \"$child_pid\"; status=$?; exit \"$status\"",
        run_dir = shell_quote(&run_dir),
        pid_name = pid_name,
        body = shell_quote(body),
    ))
}

fn remote_prepare_state_dir(
    target: &ExecutionTarget,
    run_id: &str,
) -> Result<String, RunManagerError> {
    let repo_root = remote_repo_root(target)?;
    let workspace_parent = Path::new(repo_root)
        .parent()
        .and_then(Path::to_str)
        .ok_or_else(|| RunManagerError::UnknownTarget(target.id.clone()))?;
    Ok(format!("{workspace_parent}/run-state/{run_id}"))
}

fn remote_log_archive_command(
    target: &ExecutionTarget,
    run_id: &str,
) -> Result<String, RunManagerError> {
    let repo_root = remote_repo_root(target)?;
    let pattern = format!("*{run_id}*");
    Ok(format!(
        "set -eu; cd {repo}; tmp=$(mktemp -d); cleanup() {{ rm -rf \"$tmp\"; }}; trap cleanup EXIT; find guest-linux/aarch64/logs -type f -path {pattern} -name 'node*_guest.log' -print 2>/dev/null | while IFS= read -r file; do base=$(basename \"$file\"); tail -c 262144 \"$file\" > \"$tmp/$base\"; done; tar -C \"$tmp\" -cf - .",
        repo = shell_quote(repo_root),
        pattern = shell_quote(&pattern),
    ))
}

fn remote_repo_root(target: &ExecutionTarget) -> Result<&str, RunManagerError> {
    target
        .repo_root
        .as_deref()
        .ok_or_else(|| RunManagerError::UnknownTarget(target.id.clone()))
}

fn remote_workspace_source_repo(target: &ExecutionTarget) -> Result<&str, RunManagerError> {
    target
        .workspace_source_repo
        .as_deref()
        .ok_or_else(|| RunManagerError::UnknownTarget(target.id.clone()))
}

fn shell_quote(value: &str) -> String {
    format!("'{}'", value.replace('\'', "'\\''"))
}

fn path_committed_at_head(repo_root: &Path, path: &str) -> bool {
    let output = StdCommand::new("git")
        .arg("-C")
        .arg(repo_root)
        .args(["cat-file", "-e", &format!("HEAD:{path}")])
        .output()
        .ok();
    if output.is_some_and(|output| output.status.success()) {
        return true;
    }
    let Some(top_level) = Path::new(path).components().next() else {
        return false;
    };
    let Some(top_level) = top_level.as_os_str().to_str() else {
        return false;
    };
    StdCommand::new("git")
        .arg("-C")
        .arg(repo_root)
        .args(["ls-tree", "HEAD", "--", top_level])
        .output()
        .ok()
        .filter(|output| output.status.success())
        .is_some_and(|output| String::from_utf8_lossy(&output.stdout).starts_with("160000 commit "))
}

fn load_runs(state_root: &Path) -> Result<BTreeMap<String, RunRecord>, RunManagerError> {
    let mut records = BTreeMap::new();
    for entry in fs::read_dir(state_root)? {
        let entry = entry?;
        let path = entry.path().join("run.json");
        if !path.is_file() {
            continue;
        }
        let mut record: RunRecord = serde_json::from_slice(&fs::read(path)?)?;
        if !record.status.is_terminal() && !record.pid.is_some_and(process_group_is_alive) {
            let stopped_by_user = entry.path().join(STOP_REQUESTED_MARKER).is_file();
            record.status = if stopped_by_user {
                RunStatus::Stopped
            } else {
                RunStatus::Failed
            };
            record.finished_at_ms = Some(now_ms());
            record.message = Some(if stopped_by_user {
                "stopped by user".to_string()
            } else {
                "backend restarted after the owned process group exited".to_string()
            });
            persist_record(&entry.path(), &record)?;
        }
        records.insert(record.id.clone(), record);
    }
    Ok(records)
}

fn process_group_is_alive(pid: u32) -> bool {
    if pid == 0 || pid > i32::MAX as u32 {
        return false;
    }
    let result = unsafe { libc::kill(-(pid as i32), 0) };
    result == 0 || std::io::Error::last_os_error().raw_os_error() == Some(libc::EPERM)
}

fn guest_artifact_readiness(repo_root: &Path) -> Vec<ReadinessIssue> {
    let guest_root = repo_root.join("guest-linux/aarch64");
    let out_root = guest_root.join("out");
    let kernel_root = repo_root.join("guest-linux/kernel_ub");
    let image = out_root.join("Image");
    let initramfs = out_root.join("initramfs.cpio.gz");
    let stamp_path = out_root.join(".kernel_image.kernel_ub_head");
    let remedy = "Prepare guest kernel artifacts for the current kernel_ub revision, then refresh readiness."
        .to_string();
    let mut issues = Vec::new();

    if !image.is_file() || !initramfs.is_file() {
        issues.push(ReadinessIssue {
            code: "guest_artifacts_missing".to_string(),
            message: format!(
                "Guest artifacts are missing: Image={} initramfs={}",
                image.is_file(),
                initramfs.is_file()
            ),
            remedy,
        });
        return issues;
    }

    let signature = match current_kernel_artifact_signature(&kernel_root) {
        Ok(signature) => signature,
        Err(message) => {
            issues.push(ReadinessIssue {
                code: "kernel_signature_unavailable".to_string(),
                message,
                remedy,
            });
            return issues;
        }
    };
    let stamp = match fs::read_to_string(&stamp_path) {
        Ok(stamp) => stamp,
        Err(_) => {
            issues.push(ReadinessIssue {
                code: "guest_artifact_stamp_missing".to_string(),
                message: format!(
                    "Guest artifact freshness stamp is missing: {}",
                    stamp_path.display()
                ),
                remedy,
            });
            return issues;
        }
    };
    if stamp.trim_end() != signature.trim_end() {
        let expected = signature
            .lines()
            .find_map(|line| line.strip_prefix("kernel_head="))
            .unwrap_or("unknown");
        let built = stamp
            .lines()
            .find_map(|line| line.strip_prefix("kernel_head="))
            .unwrap_or("unknown");
        issues.push(ReadinessIssue {
            code: "guest_artifacts_stale".to_string(),
            message: format!(
                "Guest kernel artifacts are stale: current kernel_ub={} built_for={}",
                short_revision(expected),
                short_revision(built)
            ),
            remedy,
        });
    }
    issues
}

fn current_kernel_artifact_signature(kernel_root: &Path) -> Result<String, String> {
    if !kernel_root.is_dir() {
        return Err(format!(
            "kernel_ub submodule is missing: {}",
            kernel_root.display()
        ));
    }
    let head = git_output(kernel_root, &["rev-parse", "HEAD"])?;
    let mut signature = format!(
        "kernel_head={}\nbuild_policy={}\n",
        head.trim(),
        KERNEL_BUILD_POLICY_REV
    );
    for path in KERNEL_SIGNATURE_PATHS {
        signature.push_str(&git_output(
            kernel_root,
            &["status", "--porcelain", "--untracked-files=no", "--", path],
        )?);
        signature.push_str(&git_output(kernel_root, &["diff", "--binary", "--", path])?);
        signature.push_str(&git_output(
            kernel_root,
            &["diff", "--cached", "--binary", "--", path],
        )?);
    }
    Ok(signature)
}

fn git_output(kernel_root: &Path, args: &[&str]) -> Result<String, String> {
    let output = StdCommand::new("git")
        .arg("-c")
        .arg(format!("safe.directory={}", kernel_root.display()))
        .arg("-C")
        .arg(kernel_root)
        .args(args)
        .output()
        .map_err(|error| format!("failed to inspect kernel_ub: {error}"))?;
    if !output.status.success() {
        return Err(format!(
            "failed to inspect kernel_ub: {}",
            String::from_utf8_lossy(&output.stderr).trim()
        ));
    }
    Ok(String::from_utf8_lossy(&output.stdout).into_owned())
}

fn short_revision(revision: &str) -> &str {
    revision.get(..12).unwrap_or(revision)
}

fn persist_record(run_dir: &Path, record: &RunRecord) -> Result<(), RunManagerError> {
    fs::create_dir_all(run_dir)?;
    let target = run_dir.join("run.json");
    let temporary = run_dir.join("run.json.tmp");
    fs::write(&temporary, serde_json::to_vec_pretty(record)?)?;
    fs::rename(temporary, target)?;
    Ok(())
}

fn discover_node_logs(
    repo_root: &Path,
    run_id: &str,
) -> Result<BTreeMap<String, PathBuf>, RunManagerError> {
    let logs_root = repo_root.join("guest-linux/aarch64/logs");
    let mut result = BTreeMap::new();
    if !logs_root.is_dir() {
        return Ok(result);
    }
    for entry in fs::read_dir(logs_root)? {
        let entry = entry?;
        let directory = entry.path();
        if !directory.is_dir()
            || !directory
                .file_name()
                .and_then(|name| name.to_str())
                .is_some_and(|name| name.contains(run_id))
        {
            continue;
        }
        for child in fs::read_dir(directory)? {
            let child = child?;
            let name = child.file_name();
            let Some(name) = name.to_str() else {
                continue;
            };
            if let Some(node_id) = name.strip_suffix("_guest.log") {
                if node_id.starts_with("node") {
                    result.insert(node_id.to_string(), child.path());
                }
            }
        }
    }
    Ok(result)
}

fn discover_mirrored_node_logs(
    run_dir: &Path,
) -> Result<BTreeMap<String, PathBuf>, RunManagerError> {
    let logs_root = run_dir.join("remote-node-logs");
    let mut result = BTreeMap::new();
    if !logs_root.is_dir() {
        return Ok(result);
    }
    for entry in fs::read_dir(logs_root)? {
        let entry = entry?;
        let name = entry.file_name();
        let Some(name) = name.to_str() else {
            continue;
        };
        if let Some(node_id) = name.strip_suffix("_guest.log") {
            if node_id.starts_with("node") && entry.path().is_file() {
                result.insert(node_id.to_string(), entry.path());
            }
        }
    }
    Ok(result)
}

fn classify_node_log(path: &Path) -> Result<NodeStatus, RunManagerError> {
    let mut file = File::open(path)?;
    let length = file.metadata()?.len();
    file.seek(SeekFrom::Start(
        length.saturating_sub(NODE_STATUS_TAIL_BYTES),
    ))?;
    let mut bytes = Vec::new();
    file.read_to_end(&mut bytes)?;
    let source = String::from_utf8_lossy(&bytes);
    let lower = source.to_ascii_lowercase();
    if [
        "verdict=fail",
        "kernel panic",
        "fatal marker",
        "] fail",
        "fail:",
    ]
    .iter()
    .any(|marker| lower.contains(marker))
    {
        return Ok(NodeStatus::Failed);
    }
    if [
        "verdict=pass",
        "[w4_guest] pass",
        "status=pass",
        "worker passed",
    ]
    .iter()
    .any(|marker| lower.contains(marker))
    {
        return Ok(NodeStatus::Passed);
    }
    if lower.contains("] pass") || lower.contains("ready") || lower.contains("run_app") {
        return Ok(NodeStatus::Ready);
    }
    if source.is_empty() {
        Ok(NodeStatus::Unknown)
    } else {
        Ok(NodeStatus::Booting)
    }
}

fn read_log_chunk(path: &Path, cursor: usize) -> Result<(Vec<String>, usize), RunManagerError> {
    if !path.is_file() {
        return Ok((Vec::new(), 0));
    }
    let file = File::open(path)?;
    let length = file.metadata()?.len() as usize;
    let effective_cursor = if cursor > length { 0 } else { cursor };
    let mut reader = BufReader::new(file);
    reader.seek(SeekFrom::Start(effective_cursor as u64))?;
    let mut lines = Vec::new();
    for _ in 0..MAX_LOG_LINES {
        let mut line = String::new();
        if reader.read_line(&mut line)? == 0 {
            break;
        }
        lines.push(line.trim_end_matches(['\r', '\n']).to_string());
    }
    let next_cursor = reader.stream_position()? as usize;
    Ok((lines, next_cursor))
}

fn relative_display(root: &Path, path: &Path) -> String {
    path.strip_prefix(root)
        .unwrap_or(path)
        .to_string_lossy()
        .into_owned()
}

fn now_ms() -> u64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .unwrap_or_default()
        .as_millis() as u64
}

#[cfg(test)]
mod tests {
    use std::collections::BTreeMap;
    use std::os::unix::fs::PermissionsExt;

    use tempfile::TempDir;
    use tokio::io::AsyncReadExt;
    use tokio::net::UnixListener;

    use super::*;
    use crate::domain::{
        CommandDefinition, ControlCapability, DemoDefinition, DemoLifecycle, GuestEngine,
        NodeInputDefinition, NodeInputKind, NodeInputRequest, ParameterDefinition, ParameterKind,
        TopologyKind,
    };

    fn fixture_repo() -> (TempDir, RunManager) {
        let root = TempDir::new().unwrap();
        fs::create_dir_all(root.path().join("crates/sim-console/tests/fixtures")).unwrap();
        let runner = root
            .path()
            .join("crates/sim-console/tests/fixtures/demo_runner.py");
        fs::write(
            &runner,
            "#!/usr/bin/env python3\nimport sys, time\nprint('fixture ready', flush=True)\ntime.sleep(float(sys.argv[1]))\nprint('fixture pass', flush=True)\n",
        )
        .unwrap();
        let mut permissions = fs::metadata(&runner).unwrap().permissions();
        permissions.set_mode(0o755);
        fs::set_permissions(&runner, permissions).unwrap();
        let demo = DemoDefinition {
            id: "fixture".to_string(),
            title: "Fixture".to_string(),
            category: "Test".to_string(),
            summary: "Fixture runner".to_string(),
            node_count: 2,
            topology: TopologyKind::Pair,
            lifecycle: DemoLifecycle::InteractiveShell,
            guest_engine: GuestEngine::Initramfs,
            requires_simpler_toolchain: false,
            model: None,
            model_source: None,
            node_input: Some(NodeInputDefinition {
                kind: NodeInputKind::QemuSerialEnv,
                manifest: "crates/sim-console/tests/fixtures/serial.{run_id}.env".to_string(),
                socket_path_prefix: "/tmp/sim-console-node-input-".to_string(),
            }),
            data_plane: vec![],
            tags: vec![],
            estimated_duration_secs: 1,
            requires_guest_artifacts: false,
            command: CommandDefinition {
                program: "crates/sim-console/tests/fixtures/demo_runner.py".to_string(),
                args: vec!["{delay}".to_string()],
                environment: BTreeMap::new(),
            },
            parameters: vec![ParameterDefinition {
                id: "delay".to_string(),
                label: "Delay".to_string(),
                kind: ParameterKind::Select,
                default: "0.01".to_string(),
                choices: vec!["0.01".to_string(), "10".to_string()],
                min: None,
                max: None,
            }],
            requirements: vec![],
            required_paths: vec!["crates/sim-console/tests/fixtures/demo_runner.py".to_string()],
            controls: vec![
                ControlCapability::Stop,
                ControlCapability::NodeLogs,
                ControlCapability::NodeInput,
            ],
        };
        let manager = RunManager::new(
            root.path(),
            DemoCatalog {
                version: 1,
                demos: vec![demo],
            },
        )
        .unwrap();
        (root, manager)
    }

    #[tokio::test]
    async fn starts_captures_and_finishes_fixture_run() {
        let (_root, manager) = fixture_repo();
        let record = manager
            .start(StartRunRequest {
                demo_id: "fixture".to_string(),
                target_id: None,
                parameters: BTreeMap::new(),
            })
            .await
            .unwrap();
        for _ in 0..300 {
            if manager.get(&record.id).await.unwrap().status.is_terminal() {
                break;
            }
            tokio::time::sleep(Duration::from_millis(10)).await;
        }
        let finished = manager.get(&record.id).await.unwrap();
        assert_eq!(finished.status, RunStatus::Passed);
        let logs = manager.logs(&record.id, None, 0).await.unwrap();
        assert!(logs.lines.iter().any(|line| line.contains("fixture ready")));
        assert!(logs.lines.iter().any(|line| line.contains("fixture pass")));
    }

    #[tokio::test]
    async fn sends_exact_input_to_the_selected_node_serial_socket() {
        let (root, manager) = fixture_repo();
        let record = manager
            .start(StartRunRequest {
                demo_id: "fixture".to_string(),
                target_id: None,
                parameters: BTreeMap::from([("delay".to_string(), "10".to_string())]),
            })
            .await
            .unwrap();
        let runtime_dir = PathBuf::from(format!("/tmp/sim-console-node-input-{}", record.id));
        let serial_dir = runtime_dir.join("serial");
        fs::create_dir_all(&serial_dir).unwrap();
        let socket = serial_dir.join("nodeA.fixture.sock");
        let listener = UnixListener::bind(&socket).unwrap();
        let manifest = root.path().join(format!(
            "crates/sim-console/tests/fixtures/serial.{}.env",
            record.id
        ));
        fs::write(
            manifest,
            format!("export NODEA_SERIAL_SOCKET='{}'\n", socket.display()),
        )
        .unwrap();
        let receiver = tokio::spawn(async move {
            let (mut stream, _) = listener.accept().await.unwrap();
            let mut received = Vec::new();
            stream.read_to_end(&mut received).await.unwrap();
            received
        });

        let secret = "fixture serial command";
        let result = manager
            .send_node_input(
                &record.id,
                "nodeA",
                NodeInputRequest {
                    data: secret.to_string(),
                    append_newline: true,
                },
            )
            .await
            .unwrap();

        assert_eq!(result.node_id, "nodeA");
        assert_eq!(result.bytes_written, secret.len() + 1);
        assert_eq!(receiver.await.unwrap(), b"fixture serial command\n");
        let process_log = fs::read_to_string(
            root.path()
                .join("out/sim-console/runs")
                .join(&record.id)
                .join("process.log"),
        )
        .unwrap();
        assert!(process_log.contains("node_input node=nodeA bytes=23 status=sent"));
        assert!(!process_log.contains(secret));
        manager.stop(&record.id).await.unwrap();
        fs::remove_dir_all(runtime_dir).unwrap();
    }

    #[tokio::test]
    async fn rejects_oversized_or_unknown_node_input_before_socket_access() {
        let (_root, manager) = fixture_repo();
        let record = manager
            .start(StartRunRequest {
                demo_id: "fixture".to_string(),
                target_id: None,
                parameters: BTreeMap::from([("delay".to_string(), "10".to_string())]),
            })
            .await
            .unwrap();

        let oversized = manager
            .send_node_input(
                &record.id,
                "nodeA",
                NodeInputRequest {
                    data: "x".repeat(MAX_NODE_INPUT_BYTES),
                    append_newline: true,
                },
            )
            .await;
        assert!(matches!(
            oversized,
            Err(RunManagerError::InvalidNodeInput(_))
        ));
        let unknown = manager
            .send_node_input(
                &record.id,
                "nodeZ",
                NodeInputRequest {
                    data: "date".to_string(),
                    append_newline: true,
                },
            )
            .await;
        assert!(matches!(unknown, Err(RunManagerError::UnknownNode { .. })));
        manager.stop(&record.id).await.unwrap();
    }

    #[tokio::test]
    async fn concurrent_status_polls_keep_state_file_valid() {
        let (root, manager) = fixture_repo();
        let record = manager
            .start(StartRunRequest {
                demo_id: "fixture".to_string(),
                target_id: None,
                parameters: BTreeMap::new(),
            })
            .await
            .unwrap();
        let mut pollers = Vec::new();
        for _ in 0..8 {
            let manager = manager.clone();
            let run_id = record.id.clone();
            pollers.push(tokio::spawn(async move {
                for _ in 0..40 {
                    manager.get(&run_id).await.unwrap();
                    tokio::task::yield_now().await;
                }
            }));
        }
        for poller in pollers {
            poller.await.unwrap();
        }
        for _ in 0..100 {
            if manager.get(&record.id).await.unwrap().status.is_terminal() {
                break;
            }
            tokio::time::sleep(Duration::from_millis(10)).await;
        }

        let persisted = fs::read(
            root.path()
                .join("out/sim-console/runs")
                .join(&record.id)
                .join("run.json"),
        )
        .unwrap();
        let persisted: RunRecord = serde_json::from_slice(&persisted).unwrap();
        assert_eq!(persisted.status, RunStatus::Passed);
    }

    #[tokio::test]
    async fn stops_the_owned_process_group() {
        let (_root, manager) = fixture_repo();
        let record = manager
            .start(StartRunRequest {
                demo_id: "fixture".to_string(),
                target_id: None,
                parameters: BTreeMap::from([("delay".to_string(), "10".to_string())]),
            })
            .await
            .unwrap();
        manager.stop(&record.id).await.unwrap();
        assert!(manager
            .inner
            .state_root
            .join(&record.id)
            .join(STOP_REQUESTED_MARKER)
            .is_file());
        for _ in 0..100 {
            if manager.get(&record.id).await.unwrap().status.is_terminal() {
                break;
            }
            tokio::time::sleep(Duration::from_millis(10)).await;
        }
        assert_eq!(
            manager.get(&record.id).await.unwrap().status,
            RunStatus::Stopped
        );
    }

    #[tokio::test]
    async fn rejects_a_second_active_run() {
        let (_root, manager) = fixture_repo();
        let first = manager
            .start(StartRunRequest {
                demo_id: "fixture".to_string(),
                target_id: None,
                parameters: BTreeMap::from([("delay".to_string(), "10".to_string())]),
            })
            .await
            .unwrap();
        let second = manager
            .start(StartRunRequest {
                demo_id: "fixture".to_string(),
                target_id: None,
                parameters: BTreeMap::new(),
            })
            .await;
        assert!(matches!(second, Err(RunManagerError::ActiveRun(id)) if id == first.id));
        manager.stop(&first.id).await.unwrap();
    }

    #[tokio::test]
    async fn rejects_a_run_with_missing_guest_artifacts_before_spawn() {
        let (root, manager) = fixture_repo();
        let mut catalog = (*manager.catalog()).clone();
        catalog.demos[0].requires_guest_artifacts = true;
        let manager = RunManager::new(root.path(), catalog).unwrap();
        let result = manager
            .start(StartRunRequest {
                demo_id: "fixture".to_string(),
                target_id: None,
                parameters: BTreeMap::new(),
            })
            .await;
        assert!(matches!(
            result,
            Err(RunManagerError::NotReady { demo, .. }) if demo == "fixture"
        ));
        assert!(manager.list().await.is_empty());
    }

    #[test]
    fn persisted_orphaned_run_fails_closed_after_restart() {
        let (root, manager) = fixture_repo();
        let run_dir = root.path().join("out/sim-console/runs/orphan");
        let record = RunRecord {
            id: "orphan".to_string(),
            demo_id: "fixture".to_string(),
            demo_title: "Fixture".to_string(),
            target_id: "local".to_string(),
            source_revision: None,
            status: RunStatus::Running,
            created_at_ms: now_ms(),
            started_at_ms: Some(now_ms()),
            finished_at_ms: None,
            pid: Some(u32::MAX),
            exit_code: None,
            parameters: BTreeMap::new(),
            nodes: topology_nodes(2),
            process_log_path: "out/sim-console/runs/orphan/process.log".to_string(),
            message: None,
        };
        persist_record(&run_dir, &record).unwrap();
        let restarted = RunManager::new(root.path(), (*manager.catalog()).clone()).unwrap();
        let loaded = restarted.inner.runs.blocking_read()["orphan"].clone();
        assert_eq!(loaded.status, RunStatus::Failed);
        let persisted: RunRecord = serde_json::from_slice(
            &fs::read(root.path().join("out/sim-console/runs/orphan/run.json")).unwrap(),
        )
        .unwrap();
        assert_eq!(persisted.status, RunStatus::Failed);
    }

    #[test]
    fn persisted_live_run_remains_active_for_cross_process_control() {
        let (root, manager) = fixture_repo();
        let mut child = StdCommand::new("sleep")
            .arg("30")
            .process_group(0)
            .spawn()
            .unwrap();
        let pid = child.id();
        let run_dir = root.path().join("out/sim-console/runs/live");
        let record = RunRecord {
            id: "live".to_string(),
            demo_id: "fixture".to_string(),
            demo_title: "Fixture".to_string(),
            target_id: "local".to_string(),
            source_revision: None,
            status: RunStatus::Running,
            created_at_ms: now_ms(),
            started_at_ms: Some(now_ms()),
            finished_at_ms: None,
            pid: Some(pid),
            exit_code: None,
            parameters: BTreeMap::new(),
            nodes: topology_nodes(2),
            process_log_path: "out/sim-console/runs/live/process.log".to_string(),
            message: None,
        };
        persist_record(&run_dir, &record).unwrap();

        let restarted = RunManager::new(root.path(), (*manager.catalog()).clone()).unwrap();
        assert_eq!(
            restarted.inner.runs.blocking_read()["live"].status,
            RunStatus::Running
        );

        unsafe {
            libc::kill(-(pid as i32), libc::SIGTERM);
        }
        child.wait().unwrap();
    }

    #[test]
    fn log_cursor_restarts_after_file_truncation() {
        let root = TempDir::new().unwrap();
        let path = root.path().join("process.log");
        fs::write(&path, "new line\n").unwrap();
        let (lines, next_cursor) = read_log_chunk(&path, 4096).unwrap();
        assert_eq!(lines, vec!["new line"]);
        assert_eq!(next_cursor, 9);
    }

    #[test]
    fn remote_commands_are_run_scoped_and_shell_quoted() {
        let target = ExecutionTarget {
            id: "remote-fixture".to_string(),
            title: "Remote fixture".to_string(),
            kind: ExecutionTargetKind::Ssh,
            description: "Fixture".to_string(),
            ssh_host: Some("remote-fixture".to_string()),
            connect_timeout_secs: Some(5),
            repo_root: Some("/home/test/repo with space".to_string()),
            workspace_source_repo: Some("/home/test/source".to_string()),
            source_repo_url: Some("https://example.invalid/source.git".to_string()),
            submodule_mirrors: vec![],
            bootstrap_files: vec!["guest-linux/aarch64/third_party/input.tar".to_string()],
            model_sources: BTreeMap::new(),
            open_euler_disk_image: None,
        };
        let resolved = ResolvedCommand {
            program: "guest-linux/aarch64/scripts/run_fixture.sh".to_string(),
            args: vec!["--prompt".to_string(), "Huawei's future".to_string()],
            environment: BTreeMap::from([("RUN_ID".to_string(), "sim-console-42".to_string())]),
            parameters: BTreeMap::new(),
        };

        let launch = remote_launch_command(&target, &resolved, "sim-console-42").unwrap();
        assert!(launch.contains("cd '/home/test/repo with space'"));
        assert!(launch.contains("'Huawei'\\''s future'"));
        assert!(launch.contains("out/sim-console/remote-runs/sim-console-42"));
        assert!(launch.contains("setsid env 'RUN_ID=sim-console-42'"));
        let stop = remote_stop_command(&target, "sim-console-42").unwrap();
        assert!(stop.contains("kill -TERM -- \"-$pid\""));
        assert!(stop.contains("process-group.pid"));
        let input = remote_node_input_command(
            &target,
            &NodeInputDefinition {
                kind: NodeInputKind::QemuSerialEnv,
                manifest: "out/serial.{run_id}.env".to_string(),
                socket_path_prefix: "/tmp/ubqe_".to_string(),
            },
            Path::new("out/serial.sim-console-42.env"),
            "nodeA",
        )
        .unwrap();
        assert!(input.contains("/home/test/repo with space/out/serial.sim-console-42.env"));
        assert!(input.contains("/tmp/ubqe_"));
        assert!(input.contains("nodeA"));
        assert!(input.contains("sys.stdin.buffer.read"));
        assert!(input.contains("time.sleep(0.2)"));
        let prepare = remote_target_bootstrap_command(&target).unwrap();
        assert!(prepare.contains("SOURCE_REPO_REUSED"));
        assert!(prepare.contains("SOURCE_REPO_CREATED"));
        assert!(prepare.contains("https://example.invalid/source.git"));
        assert!(prepare.contains("TOOL_INSTALLED\\tcargo"));
        assert!(prepare.contains("TOOL_INSTALLED\\tninja"));
        assert!(prepare.contains("TOOL_INSTALLED\\tnative-build-deps"));
        assert!(prepare.contains("https://micro.mamba.pm/api/micromamba/"));
        assert!(prepare.contains("gxx_linux-aarch64"));
        assert!(prepare.contains("TOOL_INSTALLED\\tsimpler-gcc15"));
        assert!(prepare.contains("g++-15 -dumpfullversion -dumpversion"));
        assert!(prepare.contains("for name in cc gcc g++ c++"));
        assert!(prepare.contains("ln -sfn \"$gcc_path\" \"$HOME/.local/bin/gcc-15\""));
        assert!(prepare.contains("ln -sfn \"$gxx_path\" \"$HOME/.local/bin/g++-15\""));
        assert!(!prepare.contains("ln -sfn \"$gcc_path\" \"$HOME/.local/bin/cc\""));
        assert!(!prepare.contains("ln -sfn \"$gcc_path\" \"$HOME/.local/bin/gcc\""));
        assert!(!prepare.contains("ln -sfn \"$gxx_path\" \"$HOME/.local/bin/g++\""));
        assert!(!prepare.contains("ln -sfn \"$gxx_path\" \"$HOME/.local/bin/c++\""));
        assert!(prepare.contains("http.version=HTTP/1.1"));
        assert!(prepare.contains("clone_attempt"));
        assert!(prepare.contains("https://mirrors.huaweicloud.com/rustup"));
        assert!(prepare.contains("https://static.rust-lang.org"));
        assert!(prepare.contains("RUSTUP_DIST_SERVER"));
        assert!(prepare.contains("RUSTUP_UPDATE_ROOT"));
        assert!(prepare.contains("$rustup_dir/rustup-init"));
        assert!(prepare.contains("sudo -n dnf -y install bc bison cmake cpio"));
        assert!(prepare.contains("glib2-devel"));
        assert!(prepare.contains("pixman-devel"));
        assert!(prepare.contains("pkg-config --exists"));
        let probe = remote_probe_command(&target, Path::new(".")).unwrap();
        assert!(probe.contains("simpler_cxx=g++-15"));
        assert!(probe.contains("TOOL_VERSION_MISMATCH\\t%s\\tGCC-major-15"));
        assert!(probe.contains("BOOTSTRAP_FILE_MISSING"));
        assert!(probe.contains("guest-linux/aarch64/third_party/input.tar"));
        let mirror = remote_source_mirror_prepare_command(
            &crate::target::SubmoduleMirror {
                path: "vendor/example".to_string(),
                fetch_url: "https://example.invalid/example.git".to_string(),
                git_ref: "refs/heads/main".to_string(),
            },
            "/home/test/source/vendor/example",
            "0123456789abcdef0123456789abcdef01234567",
        );
        assert!(mirror.contains("http.lowSpeedLimit=1024"));
        assert!(mirror.contains("http.lowSpeedTime=30"));
        assert!(mirror.contains("fetch_ok=0"));
    }

    #[test]
    fn target_model_source_becomes_a_model_cli_override() {
        let catalog = DemoCatalog::load_default().unwrap();
        let targets = TargetRegistry::load_default().unwrap();
        let target = targets.find("n4-910c").unwrap();
        for (demo_id, expected_path) in [
            ("w5-qwen-8", "/home/ll/models/Qwen3-0.6B"),
            (
                "w5-deepseek-v4-flash-8",
                "/home/ll/models/DeepSeek-V4-Flash/\
                 DeepSeek-V4-Flash-IQ2XXS-w2Q2K-AProjQ8-SExpQ8-OutQ8-chat-v2-imatrix.gguf",
            ),
        ] {
            let demo = catalog.find(demo_id).unwrap();
            let mut resolved = demo
                .resolve_command(
                    "sim-console-42",
                    &BTreeMap::from([("steps".to_string(), "2".to_string())]),
                )
                .unwrap();

            inject_target_model_source(demo, target, &mut resolved).unwrap();
            inject_target_guest_engine(demo, target, &mut resolved).unwrap();

            assert!(resolved
                .args
                .windows(2)
                .any(|pair| pair == ["--model", expected_path]));
            if demo.guest_engine == GuestEngine::OpenEuler {
                assert!(resolved.args.windows(2).any(|pair| {
                    pair == [
                        "--open-euler-disk-image",
                        "/home/ll/models/openEuler-2403/rootfs.qcow2",
                    ]
                }));
            }
        }
        let repo_root = Path::new(env!("CARGO_MANIFEST_DIR")).join("../..");
        let probe = remote_probe_command(target, &repo_root).unwrap();
        assert!(probe.contains("MODEL_SOURCE_MISSING"));
        assert!(probe.contains("/home/ll/models/Qwen3-0.6B"));
        assert!(probe.contains("DeepSeek-V4-Flash-IQ2XXS"));
        assert!(probe.contains("OPEN_EULER_DISK_IMAGE_MISSING"));
        assert!(probe.contains("SUBMODULE_CACHE_MISSING"));
    }

    #[test]
    fn remote_mirror_rebuilds_from_local_source_before_network_fallback() {
        let mirror = crate::target::SubmoduleMirror {
            path: "vendor/qemu_8.2.0_ub".to_string(),
            fetch_url: "https://example.invalid/qemu.git".to_string(),
            git_ref: "refs/heads/ub_sim".to_string(),
        };
        let command = remote_mirror_fetch_command(
            &mirror,
            "/managed/repo/vendor/qemu_8.2.0_ub",
            "/source/repo/vendor/qemu_8.2.0_ub",
            "0123456789abcdef",
        );
        assert!(command.contains("rm -rf \"$target_path\""));
        assert!(
            command.contains("git clone --shared --no-checkout \"$source_path\" \"$target_path\"")
        );
        assert!(command.contains(
            "! git -C \"$target_path\" checkout --detach --force \
             '0123456789abcdef' >/dev/null 2>&1"
        ));
        assert!(command.contains("mirror_ref='refs/sim-console/mirrors/0123456789abcdef'"));
        assert!(command.contains("update-ref \"$mirror_ref\" '0123456789abcdef'"));
        assert!(command.contains("checkout --detach --force '0123456789abcdef'"));
        assert!(command.contains(
            "fetch --no-tags 'https://example.invalid/qemu.git' \
             'refs/heads/ub_sim':\"$mirror_ref\" || true"
        ));
        assert!(command.contains(
            "fetch --no-tags 'https://example.invalid/qemu.git' \
             '0123456789abcdef':\"$mirror_ref\""
        ));
    }

    #[test]
    fn checkout_pack_materializes_a_complete_tree_without_history() {
        let source = TempDir::new().unwrap();
        run_checked(
            StdCommand::new("git")
                .arg("-C")
                .arg(source.path())
                .arg("init"),
            "initialize checkout pack source",
        )
        .unwrap();
        fs::create_dir_all(source.path().join("nested")).unwrap();
        fs::write(source.path().join("root.txt"), "root payload\n").unwrap();
        fs::write(source.path().join("nested/value.txt"), "nested payload\n").unwrap();
        run_checked(
            StdCommand::new("git")
                .arg("-C")
                .arg(source.path())
                .args(["add", "."]),
            "stage checkout pack source",
        )
        .unwrap();
        run_checked(
            StdCommand::new("git")
                .arg("-C")
                .arg(source.path())
                .args(["-c", "user.name=Sim Console", "-c"])
                .arg("user.email=sim-console@example.invalid")
                .args(["commit", "-m", "fixture"]),
            "commit checkout pack source",
        )
        .unwrap();
        fs::write(source.path().join("root.txt"), "current root payload\n").unwrap();
        run_checked(
            StdCommand::new("git")
                .arg("-C")
                .arg(source.path())
                .args(["add", "root.txt"]),
            "stage current checkout pack source",
        )
        .unwrap();
        run_checked(
            StdCommand::new("git")
                .arg("-C")
                .arg(source.path())
                .args(["-c", "user.name=Sim Console", "-c"])
                .arg("user.email=sim-console@example.invalid")
                .args(["commit", "-m", "current fixture"]),
            "commit current checkout pack source",
        )
        .unwrap();
        let expected = git_head(source.path()).unwrap();
        let pack_path = source.path().join("checkout.pack");
        create_checkout_pack(source.path(), &expected, &pack_path).unwrap();
        let original_pack = fs::read(&pack_path).unwrap();
        create_checkout_pack(source.path(), &expected, &pack_path).unwrap();
        assert_eq!(fs::read(&pack_path).unwrap(), original_pack);

        let target = TempDir::new().unwrap();
        run_checked(
            StdCommand::new("git")
                .arg("-C")
                .arg(target.path())
                .arg("init"),
            "initialize checkout pack target",
        )
        .unwrap();
        run_checked(
            StdCommand::new("git")
                .arg("-C")
                .arg(target.path())
                .args(["index-pack", "--stdin"])
                .stdin(Stdio::from(File::open(&pack_path).unwrap())),
            "import checkout pack",
        )
        .unwrap();
        run_checked(
            StdCommand::new("git")
                .arg("-C")
                .arg(target.path())
                .args(["checkout", "--detach", "--force", &expected]),
            "checkout imported tree",
        )
        .unwrap();
        assert_eq!(
            fs::read_to_string(target.path().join("root.txt")).unwrap(),
            "current root payload\n"
        );
        assert_eq!(
            fs::read_to_string(target.path().join("nested/value.txt")).unwrap(),
            "nested payload\n"
        );

        let receive =
            remote_file_receive_command("/remote/cache", "/remote/cache/checkout.pack", 4096, 8192);
        assert!(receive.contains("cat >> \"$file\""));
        assert!(receive.contains("-eq 4096"));
        assert!(receive.contains("-eq 8192"));
    }

    #[test]
    fn node_status_requires_a_worker_terminal_marker_for_passed() {
        let root = TempDir::new().unwrap();
        let log = root.path().join("nodeA_guest.log");

        fs::write(&log, "[ub_obmm_pool] pass\nrun_app ready\n").unwrap();
        assert_eq!(classify_node_log(&log).unwrap(), NodeStatus::Ready);

        fs::write(&log, "[ub_obmm_pool] pass\n[w4_guest] pass\n").unwrap();
        assert_eq!(classify_node_log(&log).unwrap(), NodeStatus::Passed);

        fs::write(&log, "[ub_obmm_pool] pass\n[w4_guest] fail\n").unwrap();
        assert_eq!(classify_node_log(&log).unwrap(), NodeStatus::Failed);
    }
}
