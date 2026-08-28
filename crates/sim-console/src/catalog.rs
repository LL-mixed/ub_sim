use std::collections::BTreeSet;
use std::fs;
use std::path::{Component, Path, PathBuf};

use thiserror::Error;

use crate::domain::{ControlCapability, DemoCatalog, DemoDefinition, DemoLifecycle};

const DEFAULT_CATALOG: &str = include_str!("../catalog/demos.yaml");

#[derive(Debug, Error)]
pub enum CatalogError {
    #[error("failed to read catalog {path}: {source}")]
    Read {
        path: PathBuf,
        source: std::io::Error,
    },
    #[error("failed to parse catalog: {0}")]
    Parse(#[from] serde_yaml::Error),
    #[error("unsupported catalog version: {0}")]
    Version(u32),
    #[error("duplicate demo id: {0}")]
    DuplicateId(String),
    #[error("invalid demo {demo}: {reason}")]
    InvalidDemo { demo: String, reason: String },
}

impl DemoCatalog {
    pub fn load_default() -> Result<Self, CatalogError> {
        Self::parse(DEFAULT_CATALOG)
    }

    pub fn load_path(path: &Path) -> Result<Self, CatalogError> {
        let source = fs::read_to_string(path).map_err(|source| CatalogError::Read {
            path: path.to_path_buf(),
            source,
        })?;
        Self::parse(&source)
    }

    pub fn parse(source: &str) -> Result<Self, CatalogError> {
        let catalog: Self = serde_yaml::from_str(source)?;
        catalog.validate()?;
        Ok(catalog)
    }

    pub fn validate(&self) -> Result<(), CatalogError> {
        if self.version != 1 {
            return Err(CatalogError::Version(self.version));
        }
        let mut ids = BTreeSet::new();
        for demo in &self.demos {
            if !ids.insert(demo.id.as_str()) {
                return Err(CatalogError::DuplicateId(demo.id.clone()));
            }
            validate_demo(demo)?;
        }
        Ok(())
    }

    pub fn find(&self, id: &str) -> Option<&DemoDefinition> {
        self.demos.iter().find(|demo| demo.id == id)
    }
}

fn validate_demo(demo: &DemoDefinition) -> Result<(), CatalogError> {
    let fail = |reason: &str| CatalogError::InvalidDemo {
        demo: demo.id.clone(),
        reason: reason.to_string(),
    };

    if demo.id.is_empty()
        || !demo
            .id
            .bytes()
            .all(|byte| byte.is_ascii_lowercase() || byte.is_ascii_digit() || byte == b'-')
    {
        return Err(fail(
            "id must contain lowercase ASCII letters, digits, or dashes",
        ));
    }
    if demo.title.trim().is_empty() || demo.summary.trim().is_empty() {
        return Err(fail("title and summary are required"));
    }
    if let Some(model_source) = &demo.model_source {
        if model_source.is_empty()
            || !model_source.bytes().all(|byte| {
                byte.is_ascii_lowercase() || byte.is_ascii_digit() || byte == b'-' || byte == b'.'
            })
        {
            return Err(fail(
                "model_source must contain lowercase ASCII letters, digits, dots, or dashes",
            ));
        }
    }
    if !(1..=8).contains(&demo.node_count) {
        return Err(fail("node_count must be between 1 and 8"));
    }
    validate_relative_path(&demo.command.program).map_err(|reason| fail(&reason))?;
    for path in &demo.required_paths {
        validate_relative_path(path).map_err(|reason| fail(&reason))?;
    }
    let has_node_input_control = demo.controls.contains(&ControlCapability::NodeInput);
    match demo.lifecycle {
        DemoLifecycle::Automatic if demo.node_input.is_some() || has_node_input_control => {
            return Err(fail("automatic demos must not declare node_input"));
        }
        DemoLifecycle::InteractiveShell if demo.node_input.is_none() || !has_node_input_control => {
            return Err(fail(
                "interactive_shell demos require a node_input adapter and control",
            ));
        }
        DemoLifecycle::Automatic | DemoLifecycle::InteractiveShell => {}
    }
    match &demo.node_input {
        Some(adapter) => {
            if !has_node_input_control {
                return Err(fail("node_input adapter requires the node_input control"));
            }
            validate_relative_path(&adapter.manifest).map_err(|reason| fail(&reason))?;
            if !adapter.manifest.contains("{run_id}") {
                return Err(fail("node_input manifest must contain {run_id}"));
            }
            let unresolved = adapter.manifest.replace("{run_id}", "");
            if unresolved.contains('{') || unresolved.contains('}') {
                return Err(fail("node_input manifest has an unsupported placeholder"));
            }
            if !Path::new(&adapter.socket_path_prefix).is_absolute() {
                return Err(fail("node_input socket_path_prefix must be absolute"));
            }
        }
        None if demo.controls.contains(&ControlCapability::NodeInput) => {
            return Err(fail("node_input control requires a node_input adapter"));
        }
        None => {}
    }

    let mut parameter_ids = BTreeSet::new();
    for parameter in &demo.parameters {
        if !parameter_ids.insert(parameter.id.as_str()) {
            return Err(fail("parameter ids must be unique"));
        }
        if parameter.id.is_empty()
            || !parameter
                .id
                .bytes()
                .all(|byte| byte.is_ascii_lowercase() || byte.is_ascii_digit() || byte == b'_')
        {
            return Err(fail("parameter ids must be lowercase ASCII identifiers"));
        }
    }
    demo.resolve_command("catalog-check", &Default::default())
        .map_err(|error| fail(&error.to_string()))?;
    Ok(())
}

fn validate_relative_path(value: &str) -> Result<(), String> {
    let path = Path::new(value);
    if path.is_absolute() {
        return Err(format!("path must be repository-relative: {value}"));
    }
    if path.components().any(|component| {
        matches!(
            component,
            Component::ParentDir | Component::RootDir | Component::Prefix(_)
        )
    }) {
        return Err(format!("path escapes repository root: {value}"));
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn default_catalog_is_valid_and_covers_requested_families() {
        let catalog = DemoCatalog::load_default().unwrap();
        let categories: BTreeSet<_> = catalog
            .demos
            .iter()
            .map(|demo| demo.category.as_str())
            .collect();
        for expected in [
            "OBMM",
            "URMA and RPC",
            "Memory Service",
            "GVA and GSVA",
            "UB-SSD",
            "Upcall",
            "W5 Inference",
        ] {
            assert!(categories.contains(expected), "missing category {expected}");
        }
    }

    #[test]
    fn default_catalog_exposes_input_only_for_live_interactive_shells() {
        let catalog = DemoCatalog::load_default().unwrap();
        let interactive: Vec<_> = catalog
            .demos
            .iter()
            .filter(|demo| demo.lifecycle == DemoLifecycle::InteractiveShell)
            .map(|demo| demo.id.as_str())
            .collect();

        assert_eq!(interactive, vec!["urma-rpc-2"]);
        for demo in &catalog.demos {
            let has_input =
                demo.node_input.is_some() && demo.controls.contains(&ControlCapability::NodeInput);
            assert_eq!(
                has_input,
                demo.lifecycle == DemoLifecycle::InteractiveShell,
                "node input lifecycle mismatch for {}",
                demo.id
            );
        }
    }

    #[test]
    fn every_w5_model_demo_declares_a_logical_model_source() {
        let catalog = DemoCatalog::load_default().unwrap();

        for demo in catalog
            .demos
            .iter()
            .filter(|demo| demo.category == "W5 Inference")
        {
            assert!(
                demo.model_source.is_some(),
                "W5 demo {} has no target model source",
                demo.id
            );
        }
    }

    #[test]
    fn only_w5_model_demos_require_the_simpler_toolchain() {
        let catalog = DemoCatalog::load_default().unwrap();
        let simpler_demos: Vec<_> = catalog
            .demos
            .iter()
            .filter(|demo| demo.requires_simpler_toolchain)
            .map(|demo| demo.id.as_str())
            .collect();

        assert_eq!(
            simpler_demos,
            vec![
                "w5-qwen-2",
                "w5-qwen-4",
                "w5-qwen-8",
                "w5-deepseek-v4-flash-2",
                "w5-deepseek-v4-flash-4",
                "w5-deepseek-v4-flash-8",
            ]
        );
    }

    #[test]
    fn default_catalog_references_existing_repository_entrypoints() {
        let repo_root = Path::new(env!("CARGO_MANIFEST_DIR"))
            .parent()
            .and_then(Path::parent)
            .unwrap();
        let catalog = DemoCatalog::load_default().unwrap();
        for demo in catalog.demos {
            assert!(
                repo_root.join(&demo.command.program).is_file(),
                "missing program for {}: {}",
                demo.id,
                demo.command.program
            );
            for required in demo.required_paths {
                assert!(
                    repo_root.join(&required).exists(),
                    "missing requirement for {}: {}",
                    demo.id,
                    required
                );
            }
        }
    }

    #[test]
    fn catalog_rejects_duplicate_ids_and_escaping_paths() {
        let duplicate = r#"
version: 1
demos:
  - &demo
    id: duplicate
    title: Demo
    category: Test
    summary: Test
    node_count: 1
    topology: service
    estimated_duration_secs: 1
    command: { program: fixture }
  - *demo
"#;
        assert!(matches!(
            DemoCatalog::parse(duplicate),
            Err(CatalogError::DuplicateId(_))
        ));

        let escaping = duplicate
            .replace("- *demo", "")
            .replace("fixture", "../fixture");
        assert!(matches!(
            DemoCatalog::parse(&escaping),
            Err(CatalogError::InvalidDemo { .. })
        ));

        let unbound_node_input = r#"
version: 1
demos:
  - id: fixture
    title: Fixture
    category: Test
    summary: Test
    node_count: 1
    topology: service
    estimated_duration_secs: 1
    command: { program: fixture }
    lifecycle: interactive_shell
    controls: [node_input]
"#;
        assert!(matches!(
            DemoCatalog::parse(unbound_node_input),
            Err(CatalogError::InvalidDemo { .. })
        ));

        let automatic_node_input = r#"
version: 1
demos:
  - id: fixture
    title: Fixture
    category: Test
    summary: Test
    node_count: 1
    topology: service
    estimated_duration_secs: 1
    command: { program: fixture }
    node_input:
      kind: qemu_serial_env
      manifest: out/serial.{run_id}.env
      socket_path_prefix: /tmp/fixture-
    controls: [node_input]
"#;
        assert!(matches!(
            DemoCatalog::parse(automatic_node_input),
            Err(CatalogError::InvalidDemo { .. })
        ));
    }
}
