use std::collections::BTreeSet;
use std::fs;
use std::path::{Component, Path, PathBuf};

use thiserror::Error;

use crate::domain::{ControlCapability, DemoCatalog, DemoDefinition};

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
    match &demo.node_input {
        Some(adapter) => {
            if !demo.controls.contains(&ControlCapability::NodeInput) {
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
    controls: [node_input]
"#;
        assert!(matches!(
            DemoCatalog::parse(unbound_node_input),
            Err(CatalogError::InvalidDemo { .. })
        ));
    }
}
