use std::collections::{BTreeMap, BTreeSet};
use std::fs;
use std::path::{Path, PathBuf};

use serde::{Deserialize, Serialize};
use thiserror::Error;

const DEFAULT_TARGETS: &str = include_str!("../config/targets.yaml");

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct TargetRegistry {
    pub version: u32,
    pub default_target: String,
    pub targets: Vec<ExecutionTarget>,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct ExecutionTarget {
    pub id: String,
    pub title: String,
    pub kind: ExecutionTargetKind,
    pub description: String,
    #[serde(default)]
    pub ssh_host: Option<String>,
    #[serde(default)]
    pub connect_timeout_secs: Option<u64>,
    #[serde(default)]
    pub repo_root: Option<String>,
    #[serde(default)]
    pub workspace_source_repo: Option<String>,
    #[serde(default)]
    pub source_repo_url: Option<String>,
    #[serde(default)]
    pub submodule_mirrors: Vec<SubmoduleMirror>,
    #[serde(default)]
    pub bootstrap_files: Vec<String>,
    #[serde(default)]
    pub model_sources: BTreeMap<String, String>,
    #[serde(default)]
    pub open_euler_disk_image: Option<String>,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct SubmoduleMirror {
    pub path: String,
    pub fetch_url: String,
    pub git_ref: String,
}

#[derive(Debug, Clone, Copy, Serialize, Deserialize, PartialEq, Eq)]
#[serde(rename_all = "snake_case")]
pub enum ExecutionTargetKind {
    Local,
    Ssh,
}

#[derive(Debug, Error)]
pub enum TargetRegistryError {
    #[error("failed to read target registry {path}: {source}")]
    Read {
        path: PathBuf,
        source: std::io::Error,
    },
    #[error("failed to parse target registry: {0}")]
    Parse(#[from] serde_yaml::Error),
    #[error("unsupported target registry version: {0}")]
    Version(u32),
    #[error("duplicate target id: {0}")]
    DuplicateId(String),
    #[error("default target is not registered: {0}")]
    UnknownDefault(String),
    #[error("invalid target {target}: {reason}")]
    InvalidTarget { target: String, reason: String },
}

impl TargetRegistry {
    pub fn load_default() -> Result<Self, TargetRegistryError> {
        Self::parse(DEFAULT_TARGETS)
    }

    pub fn load_path(path: &Path) -> Result<Self, TargetRegistryError> {
        let source = fs::read_to_string(path).map_err(|source| TargetRegistryError::Read {
            path: path.to_path_buf(),
            source,
        })?;
        Self::parse(&source)
    }

    pub fn local_only() -> Self {
        Self {
            version: 1,
            default_target: "local".to_string(),
            targets: vec![ExecutionTarget {
                id: "local".to_string(),
                title: "Local host".to_string(),
                kind: ExecutionTargetKind::Local,
                description: "Build and run on this host.".to_string(),
                ssh_host: None,
                connect_timeout_secs: None,
                repo_root: None,
                workspace_source_repo: None,
                source_repo_url: None,
                submodule_mirrors: vec![],
                bootstrap_files: vec![],
                model_sources: BTreeMap::new(),
                open_euler_disk_image: None,
            }],
        }
    }

    pub fn parse(source: &str) -> Result<Self, TargetRegistryError> {
        let registry: Self = serde_yaml::from_str(source)?;
        registry.validate()?;
        Ok(registry)
    }

    pub fn validate(&self) -> Result<(), TargetRegistryError> {
        if self.version != 1 {
            return Err(TargetRegistryError::Version(self.version));
        }
        let mut ids = BTreeSet::new();
        for target in &self.targets {
            if !ids.insert(target.id.as_str()) {
                return Err(TargetRegistryError::DuplicateId(target.id.clone()));
            }
            validate_target(target)?;
        }
        if !ids.contains(self.default_target.as_str()) {
            return Err(TargetRegistryError::UnknownDefault(
                self.default_target.clone(),
            ));
        }
        Ok(())
    }

    pub fn find(&self, id: &str) -> Option<&ExecutionTarget> {
        self.targets.iter().find(|target| target.id == id)
    }

    pub fn resolve(&self, requested: Option<&str>) -> Option<&ExecutionTarget> {
        self.find(requested.unwrap_or(&self.default_target))
    }
}

fn validate_target(target: &ExecutionTarget) -> Result<(), TargetRegistryError> {
    let fail = |reason: &str| TargetRegistryError::InvalidTarget {
        target: target.id.clone(),
        reason: reason.to_string(),
    };
    if target.id.is_empty()
        || !target
            .id
            .bytes()
            .all(|byte| byte.is_ascii_lowercase() || byte.is_ascii_digit() || byte == b'-')
    {
        return Err(fail(
            "id must contain lowercase ASCII letters, digits, or dashes",
        ));
    }
    if target.title.trim().is_empty() || target.description.trim().is_empty() {
        return Err(fail("title and description are required"));
    }
    for (model_source, path) in &target.model_sources {
        if model_source.is_empty()
            || !model_source.bytes().all(|byte| {
                byte.is_ascii_lowercase() || byte.is_ascii_digit() || byte == b'-' || byte == b'.'
            })
        {
            return Err(fail(
                "model source ids must contain lowercase ASCII letters, digits, dots, or dashes",
            ));
        }
        if !Path::new(path).is_absolute() {
            return Err(fail("model source paths must be absolute"));
        }
    }
    if let Some(path) = &target.open_euler_disk_image {
        if !Path::new(path).is_absolute() {
            return Err(fail("openEuler disk image path must be absolute"));
        }
    }
    match target.kind {
        ExecutionTargetKind::Local => {
            if target.ssh_host.is_some()
                || target.connect_timeout_secs.is_some()
                || target.repo_root.is_some()
                || target.workspace_source_repo.is_some()
                || target.source_repo_url.is_some()
                || !target.submodule_mirrors.is_empty()
                || !target.bootstrap_files.is_empty()
            {
                return Err(fail(
                    "local targets cannot declare SSH or remote workspace fields",
                ));
            }
        }
        ExecutionTargetKind::Ssh => {
            let host = target
                .ssh_host
                .as_deref()
                .filter(|value| !value.trim().is_empty())
                .ok_or_else(|| fail("ssh targets require ssh_host"))?;
            if host.starts_with('-') || host.bytes().any(|byte| byte.is_ascii_whitespace()) {
                return Err(fail(
                    "ssh_host cannot start with a dash or contain whitespace",
                ));
            }
            if !matches!(target.connect_timeout_secs, Some(1..=60)) {
                return Err(fail(
                    "ssh targets require connect_timeout_secs between 1 and 60",
                ));
            }
            let repo_root = target
                .repo_root
                .as_deref()
                .filter(|value| !value.trim().is_empty())
                .ok_or_else(|| fail("ssh targets require repo_root"))?;
            if !Path::new(repo_root).is_absolute() {
                return Err(fail("repo_root must be absolute"));
            }
            let source_repo = target
                .workspace_source_repo
                .as_deref()
                .filter(|value| !value.trim().is_empty())
                .ok_or_else(|| fail("ssh targets require workspace_source_repo"))?;
            if !Path::new(source_repo).is_absolute() {
                return Err(fail("workspace_source_repo must be absolute"));
            }
            if source_repo == repo_root {
                return Err(fail(
                    "managed repo_root must differ from workspace_source_repo",
                ));
            }
            let source_repo_url = target
                .source_repo_url
                .as_deref()
                .filter(|value| !value.trim().is_empty())
                .ok_or_else(|| fail("ssh targets require source_repo_url"))?;
            if source_repo_url.starts_with('-')
                || source_repo_url
                    .bytes()
                    .any(|byte| byte.is_ascii_whitespace())
            {
                return Err(fail(
                    "source_repo_url cannot start with a dash or contain whitespace",
                ));
            }
            let mut mirror_paths = BTreeSet::new();
            for mirror in &target.submodule_mirrors {
                if mirror.path.is_empty()
                    || Path::new(&mirror.path).is_absolute()
                    || Path::new(&mirror.path)
                        .components()
                        .any(|component| matches!(component, std::path::Component::ParentDir))
                {
                    return Err(fail("submodule mirror paths must be repository-relative"));
                }
                if !mirror_paths.insert(mirror.path.as_str()) {
                    return Err(fail("submodule mirror paths must be unique"));
                }
                if mirror.fetch_url.trim().is_empty() || mirror.git_ref.trim().is_empty() {
                    return Err(fail("submodule mirrors require fetch_url and git_ref"));
                }
            }
            let mut bootstrap_files = BTreeSet::new();
            for file in &target.bootstrap_files {
                if file.is_empty()
                    || Path::new(file).is_absolute()
                    || Path::new(file)
                        .components()
                        .any(|component| matches!(component, std::path::Component::ParentDir))
                {
                    return Err(fail("bootstrap files must be repository-relative"));
                }
                if !bootstrap_files.insert(file.as_str()) {
                    return Err(fail("bootstrap files must be unique"));
                }
            }
        }
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn default_registry_contains_local_and_n4_targets() {
        let registry = TargetRegistry::load_default().unwrap();
        assert_eq!(registry.default_target, "n4-910c1");
        assert_eq!(
            registry.find("local").unwrap().kind,
            ExecutionTargetKind::Local
        );
        assert_eq!(
            registry.find("n4-910c").unwrap().ssh_host.as_deref(),
            Some("n4-910c")
        );
        assert_eq!(
            registry.find("n4-910c1").unwrap().repo_root.as_deref(),
            Some("/home/ll/sim-console/ub_sim")
        );
        let n4 = registry.find("n4-910c").unwrap();
        assert_eq!(n4.connect_timeout_secs, Some(15));
        assert_eq!(n4.workspace_source_repo.as_deref(), Some("/home/ll/ub_sim"));
        assert_eq!(
            n4.source_repo_url.as_deref(),
            Some("https://github.com/LL-mixed/ub_sim.git")
        );
        assert_eq!(
            n4.model_sources.get("qwen3-0.6b").map(String::as_str),
            Some("/home/ll/models/Qwen3-0.6B")
        );
        assert_eq!(
            n4.model_sources
                .get("deepseek-v4-flash-iq2xxs")
                .map(String::as_str),
            Some(
                "/home/ll/models/DeepSeek-V4-Flash/\
                 DeepSeek-V4-Flash-IQ2XXS-w2Q2K-AProjQ8-SExpQ8-OutQ8-chat-v2-imatrix.gguf"
            )
        );
        assert_eq!(
            n4.open_euler_disk_image.as_deref(),
            Some("/home/ll/models/openEuler-2403/rootfs.qcow2")
        );
        assert_eq!(n4.submodule_mirrors.len(), 8);
        assert_eq!(
            n4.bootstrap_files,
            vec!["guest-linux/aarch64/third_party/busybox-1.36.1.tar.bz2"]
        );
        assert!(n4
            .submodule_mirrors
            .iter()
            .any(|mirror| mirror.path == "mem_service"));
        assert!(n4
            .submodule_mirrors
            .iter()
            .any(|mirror| mirror.path == "vendor/qemu_8.2.0_ub"));
    }

    #[test]
    fn registry_rejects_unreviewed_target_shapes() {
        let invalid = r#"
version: 1
default_target: remote
targets:
  - id: remote
    title: Remote
    description: Remote
    kind: ssh
    ssh_host: --proxy-command
    repo_root: relative/repo
"#;
        assert!(matches!(
            TargetRegistry::parse(invalid),
            Err(TargetRegistryError::InvalidTarget { .. })
        ));

        let relative_model = r#"
version: 1
default_target: local
targets:
  - id: local
    title: Local
    description: Local
    kind: local
    model_sources:
      qwen3-0.6b: relative/Qwen3-0.6B
"#;
        assert!(matches!(
            TargetRegistry::parse(relative_model),
            Err(TargetRegistryError::InvalidTarget { .. })
        ));
    }
}
