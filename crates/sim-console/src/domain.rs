use std::collections::{BTreeMap, BTreeSet};

use serde::{Deserialize, Serialize};
use thiserror::Error;

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct DemoCatalog {
    pub version: u32,
    pub demos: Vec<DemoDefinition>,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct DemoDefinition {
    pub id: String,
    pub title: String,
    pub category: String,
    pub summary: String,
    pub node_count: u8,
    pub topology: TopologyKind,
    #[serde(default)]
    pub model: Option<String>,
    #[serde(default)]
    pub model_source: Option<String>,
    #[serde(default)]
    pub data_plane: Vec<String>,
    #[serde(default)]
    pub tags: Vec<String>,
    pub estimated_duration_secs: u64,
    #[serde(default = "default_requires_guest_artifacts")]
    pub requires_guest_artifacts: bool,
    pub command: CommandDefinition,
    #[serde(default)]
    pub parameters: Vec<ParameterDefinition>,
    #[serde(default)]
    pub requirements: Vec<String>,
    #[serde(default)]
    pub required_paths: Vec<String>,
    #[serde(default)]
    pub controls: Vec<ControlCapability>,
}

#[derive(Debug, Clone, Copy, Serialize, Deserialize, PartialEq, Eq)]
#[serde(rename_all = "snake_case")]
pub enum TopologyKind {
    Pair,
    Mesh,
    Pipeline,
    Service,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct CommandDefinition {
    pub program: String,
    #[serde(default)]
    pub args: Vec<String>,
    #[serde(default)]
    pub environment: BTreeMap<String, String>,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct ParameterDefinition {
    pub id: String,
    pub label: String,
    pub kind: ParameterKind,
    pub default: String,
    #[serde(default)]
    pub choices: Vec<String>,
    #[serde(default)]
    pub min: Option<i64>,
    #[serde(default)]
    pub max: Option<i64>,
}

#[derive(Debug, Clone, Copy, Serialize, Deserialize, PartialEq, Eq)]
#[serde(rename_all = "snake_case")]
pub enum ParameterKind {
    Select,
    Integer,
}

#[derive(Debug, Clone, Copy, Serialize, Deserialize, PartialEq, Eq)]
#[serde(rename_all = "snake_case")]
pub enum ControlCapability {
    Stop,
    NodeLogs,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct ResolvedCommand {
    pub program: String,
    pub args: Vec<String>,
    pub environment: BTreeMap<String, String>,
    pub parameters: BTreeMap<String, String>,
}

#[derive(Debug, Clone, Serialize, Deserialize, Default)]
pub struct StartRunRequest {
    pub demo_id: String,
    #[serde(default)]
    pub target_id: Option<String>,
    #[serde(default)]
    pub parameters: BTreeMap<String, String>,
}

#[derive(Debug, Clone, Copy, Serialize, Deserialize, PartialEq, Eq)]
#[serde(rename_all = "snake_case")]
pub enum RunStatus {
    Queued,
    Starting,
    Running,
    Passed,
    Failed,
    Stopped,
}

impl RunStatus {
    pub fn is_terminal(self) -> bool {
        matches!(self, Self::Passed | Self::Failed | Self::Stopped)
    }
}

#[derive(Debug, Clone, Copy, Serialize, Deserialize, PartialEq, Eq)]
#[serde(rename_all = "snake_case")]
pub enum NodeStatus {
    Unknown,
    Booting,
    Ready,
    Passed,
    Failed,
    Stopped,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct NodeRecord {
    pub id: String,
    pub label: String,
    pub status: NodeStatus,
    #[serde(default)]
    pub log_path: Option<String>,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct RunRecord {
    pub id: String,
    pub demo_id: String,
    pub demo_title: String,
    #[serde(default = "default_local_target_id")]
    pub target_id: String,
    #[serde(default)]
    pub source_revision: Option<String>,
    pub status: RunStatus,
    pub created_at_ms: u64,
    #[serde(default)]
    pub started_at_ms: Option<u64>,
    #[serde(default)]
    pub finished_at_ms: Option<u64>,
    #[serde(default)]
    pub pid: Option<u32>,
    #[serde(default)]
    pub exit_code: Option<i32>,
    pub parameters: BTreeMap<String, String>,
    pub nodes: Vec<NodeRecord>,
    pub process_log_path: String,
    #[serde(default)]
    pub message: Option<String>,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct LogChunk {
    pub run_id: String,
    #[serde(default)]
    pub node: Option<String>,
    pub cursor: usize,
    pub next_cursor: usize,
    pub complete: bool,
    pub lines: Vec<String>,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct DemoReadiness {
    pub demo_id: String,
    pub target_id: String,
    pub ready: bool,
    pub issues: Vec<ReadinessIssue>,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct ReadinessIssue {
    pub code: String,
    pub message: String,
    pub remedy: String,
}

#[derive(Debug, Error)]
pub enum DomainError {
    #[error("unknown parameter: {0}")]
    UnknownParameter(String),
    #[error("parameter {parameter} has invalid value {value}: {reason}")]
    InvalidParameter {
        parameter: String,
        value: String,
        reason: String,
    },
    #[error("unresolved command placeholder: {0}")]
    UnresolvedPlaceholder(String),
}

fn default_requires_guest_artifacts() -> bool {
    true
}

fn default_local_target_id() -> String {
    "local".to_string()
}

impl DemoDefinition {
    pub fn resolve_command(
        &self,
        run_id: &str,
        requested: &BTreeMap<String, String>,
    ) -> Result<ResolvedCommand, DomainError> {
        let known: BTreeSet<&str> = self
            .parameters
            .iter()
            .map(|item| item.id.as_str())
            .collect();
        if let Some(unknown) = requested.keys().find(|key| !known.contains(key.as_str())) {
            return Err(DomainError::UnknownParameter(unknown.clone()));
        }

        let mut values = BTreeMap::new();
        for parameter in &self.parameters {
            let value = requested
                .get(&parameter.id)
                .cloned()
                .unwrap_or_else(|| parameter.default.clone());
            parameter.validate(&value)?;
            values.insert(parameter.id.clone(), value);
        }

        let replace = |template: &str| -> Result<String, DomainError> {
            let mut resolved = template.replace("{run_id}", run_id);
            for (key, value) in &values {
                resolved = resolved.replace(&format!("{{{key}}}"), value);
            }
            if resolved.contains('{') || resolved.contains('}') {
                return Err(DomainError::UnresolvedPlaceholder(resolved));
            }
            Ok(resolved)
        };

        let args = self
            .command
            .args
            .iter()
            .map(|arg| replace(arg))
            .collect::<Result<Vec<_>, _>>()?;
        let environment = self
            .command
            .environment
            .iter()
            .map(|(key, value)| Ok((key.clone(), replace(value)?)))
            .collect::<Result<BTreeMap<_, _>, DomainError>>()?;

        Ok(ResolvedCommand {
            program: replace(&self.command.program)?,
            args,
            environment,
            parameters: values,
        })
    }
}

impl ParameterDefinition {
    fn validate(&self, value: &str) -> Result<(), DomainError> {
        let invalid = |reason: String| DomainError::InvalidParameter {
            parameter: self.id.clone(),
            value: value.to_string(),
            reason,
        };

        match self.kind {
            ParameterKind::Select => {
                if !self.choices.iter().any(|choice| choice == value) {
                    return Err(invalid(format!(
                        "expected one of {}",
                        self.choices.join(", ")
                    )));
                }
            }
            ParameterKind::Integer => {
                let parsed = value
                    .parse::<i64>()
                    .map_err(|_| invalid("expected an integer".to_string()))?;
                if self.min.is_some_and(|min| parsed < min) {
                    return Err(invalid(format!("must be at least {}", self.min.unwrap())));
                }
                if self.max.is_some_and(|max| parsed > max) {
                    return Err(invalid(format!("must be at most {}", self.max.unwrap())));
                }
            }
        }
        Ok(())
    }
}

pub fn topology_nodes(node_count: u8) -> Vec<NodeRecord> {
    (0..node_count)
        .map(|index| NodeRecord {
            id: format!("node{}", (b'A' + index) as char),
            label: format!("Node {}", (b'A' + index) as char),
            status: NodeStatus::Unknown,
            log_path: None,
        })
        .collect()
}

#[cfg(test)]
mod tests {
    use super::*;

    fn demo() -> DemoDefinition {
        DemoDefinition {
            id: "fixture".to_string(),
            title: "Fixture".to_string(),
            category: "test".to_string(),
            summary: "fixture".to_string(),
            node_count: 2,
            topology: TopologyKind::Pair,
            model: None,
            model_source: None,
            data_plane: vec![],
            tags: vec![],
            estimated_duration_secs: 1,
            requires_guest_artifacts: false,
            command: CommandDefinition {
                program: "fixture".to_string(),
                args: vec!["--steps".to_string(), "{steps}".to_string()],
                environment: BTreeMap::from([("RUN_ID".to_string(), "{run_id}".to_string())]),
            },
            parameters: vec![ParameterDefinition {
                id: "steps".to_string(),
                label: "Steps".to_string(),
                kind: ParameterKind::Integer,
                default: "2".to_string(),
                choices: vec![],
                min: Some(1),
                max: Some(8),
            }],
            requirements: vec![],
            required_paths: vec![],
            controls: vec![ControlCapability::Stop],
        }
    }

    #[test]
    fn resolves_declared_parameters_and_run_id() {
        let command = demo()
            .resolve_command(
                "run-42",
                &BTreeMap::from([("steps".to_string(), "4".to_string())]),
            )
            .unwrap();
        assert_eq!(command.args, vec!["--steps", "4"]);
        assert_eq!(command.environment["RUN_ID"], "run-42");
    }

    #[test]
    fn rejects_unknown_and_out_of_range_parameters() {
        assert!(matches!(
            demo().resolve_command(
                "run-1",
                &BTreeMap::from([("command".to_string(), "rm".to_string())])
            ),
            Err(DomainError::UnknownParameter(_))
        ));
        assert!(matches!(
            demo().resolve_command(
                "run-1",
                &BTreeMap::from([("steps".to_string(), "99".to_string())])
            ),
            Err(DomainError::InvalidParameter { .. })
        ));
    }

    #[test]
    fn generates_stable_node_ids() {
        let nodes = topology_nodes(4);
        assert_eq!(nodes[0].id, "nodeA");
        assert_eq!(nodes[3].id, "nodeD");
    }
}
