use axum::extract::{Path, Query, State};
use axum::http::{header, StatusCode};
use axum::response::{Html, IntoResponse, Response};
use axum::routing::{get, post};
use axum::{Json, Router};
use serde::Serialize;

use crate::domain::{
    LogChunk, NodeInputRequest, NodeInputResult, RunRecord, StartRunRequest,
    TargetPreparationResult,
};
use crate::{RunManager, RunManagerError};

const INDEX_HTML: &str = include_str!("../web/index.html");
const APP_JS: &str = include_str!("../web/app.js");
const STYLES_CSS: &str = include_str!("../web/styles.css");

#[derive(Clone)]
struct AppState {
    manager: RunManager,
}

#[derive(Debug, Serialize)]
struct HealthResponse {
    status: &'static str,
    api_version: &'static str,
}

#[derive(Debug, Serialize)]
struct ErrorResponse {
    error: String,
}

#[derive(Debug)]
struct ApiError {
    status: StatusCode,
    message: String,
}

#[derive(Debug, serde::Deserialize)]
struct LogQuery {
    #[serde(default)]
    cursor: usize,
    #[serde(default)]
    node: Option<String>,
}

#[derive(Debug, serde::Deserialize)]
struct ReadinessQuery {
    #[serde(default)]
    target: Option<String>,
}

pub fn router(manager: RunManager) -> Router {
    Router::new()
        .route("/", get(index))
        .route("/app.js", get(app_js))
        .route("/styles.css", get(styles_css))
        .route("/api/v1/health", get(health))
        .route("/api/v1/catalog", get(catalog))
        .route("/api/v1/targets", get(targets))
        .route("/api/v1/targets/{target_id}/prepare", post(prepare_target))
        .route("/api/v1/readiness", get(readiness))
        .route("/api/v1/runs", get(list_runs).post(start_run))
        .route("/api/v1/runs/{run_id}", get(get_run))
        .route("/api/v1/runs/{run_id}/logs", get(get_logs))
        .route(
            "/api/v1/runs/{run_id}/nodes/{node_id}/input",
            post(send_node_input),
        )
        .route("/api/v1/runs/{run_id}/stop", post(stop_run))
        .with_state(AppState { manager })
}

async fn index() -> Html<&'static str> {
    Html(INDEX_HTML)
}

async fn app_js() -> impl IntoResponse {
    (
        [(header::CONTENT_TYPE, "text/javascript; charset=utf-8")],
        APP_JS,
    )
}

async fn styles_css() -> impl IntoResponse {
    (
        [(header::CONTENT_TYPE, "text/css; charset=utf-8")],
        STYLES_CSS,
    )
}

async fn health() -> Json<HealthResponse> {
    Json(HealthResponse {
        status: "ok",
        api_version: "v1",
    })
}

async fn catalog(State(state): State<AppState>) -> Json<crate::domain::DemoCatalog> {
    Json((*state.manager.catalog()).clone())
}

async fn targets(State(state): State<AppState>) -> Json<crate::target::TargetRegistry> {
    Json((*state.manager.targets()).clone())
}

async fn prepare_target(
    State(state): State<AppState>,
    Path(target_id): Path<String>,
) -> Result<Json<TargetPreparationResult>, ApiError> {
    Ok(Json(state.manager.prepare_target(&target_id).await?))
}

async fn readiness(
    State(state): State<AppState>,
    Query(query): Query<ReadinessQuery>,
) -> Result<Json<Vec<crate::domain::DemoReadiness>>, ApiError> {
    Ok(Json(
        state.manager.readiness(query.target.as_deref()).await?,
    ))
}

async fn list_runs(State(state): State<AppState>) -> Json<Vec<RunRecord>> {
    Json(state.manager.list().await)
}

async fn start_run(
    State(state): State<AppState>,
    Json(request): Json<StartRunRequest>,
) -> Result<(StatusCode, Json<RunRecord>), ApiError> {
    let record = state.manager.start(request).await?;
    Ok((StatusCode::CREATED, Json(record)))
}

async fn get_run(
    State(state): State<AppState>,
    Path(run_id): Path<String>,
) -> Result<Json<RunRecord>, ApiError> {
    Ok(Json(state.manager.get(&run_id).await?))
}

async fn get_logs(
    State(state): State<AppState>,
    Path(run_id): Path<String>,
    Query(query): Query<LogQuery>,
) -> Result<Json<LogChunk>, ApiError> {
    Ok(Json(
        state
            .manager
            .logs(&run_id, query.node.as_deref(), query.cursor)
            .await?,
    ))
}

async fn stop_run(
    State(state): State<AppState>,
    Path(run_id): Path<String>,
) -> Result<Json<RunRecord>, ApiError> {
    Ok(Json(state.manager.stop(&run_id).await?))
}

async fn send_node_input(
    State(state): State<AppState>,
    Path((run_id, node_id)): Path<(String, String)>,
    Json(request): Json<NodeInputRequest>,
) -> Result<Json<NodeInputResult>, ApiError> {
    Ok(Json(
        state
            .manager
            .send_node_input(&run_id, &node_id, request)
            .await?,
    ))
}

impl From<RunManagerError> for ApiError {
    fn from(error: RunManagerError) -> Self {
        let status = match error {
            RunManagerError::UnknownDemo(_)
            | RunManagerError::UnknownTarget(_)
            | RunManagerError::UnknownRun(_)
            | RunManagerError::UnknownNode { .. } => StatusCode::NOT_FOUND,
            RunManagerError::TerminalRun(_)
            | RunManagerError::NotReady { .. }
            | RunManagerError::MissingRequirement(_)
            | RunManagerError::UnsafePath(_)
            | RunManagerError::InvalidNodeInput(_)
            | RunManagerError::TargetPreparationUnavailable(_)
            | RunManagerError::Domain(_) => StatusCode::BAD_REQUEST,
            RunManagerError::ActiveRun(_) | RunManagerError::NodeInputUnavailable(_) => {
                StatusCode::CONFLICT
            }
            RunManagerError::Io(_) | RunManagerError::Json(_) => StatusCode::INTERNAL_SERVER_ERROR,
        };
        Self {
            status,
            message: error.to_string(),
        }
    }
}

impl IntoResponse for ApiError {
    fn into_response(self) -> Response {
        (
            self.status,
            Json(ErrorResponse {
                error: self.message,
            }),
        )
            .into_response()
    }
}

#[cfg(test)]
mod tests {
    use std::collections::BTreeMap;
    use std::fs;

    use axum::body::Body;
    use axum::http::Request;
    use http_body_util::BodyExt;
    use tempfile::TempDir;
    use tower::ServiceExt;

    use super::*;
    use crate::domain::{
        CommandDefinition, DemoCatalog, DemoDefinition, DemoLifecycle, GuestEngine, TopologyKind,
    };

    fn test_app() -> (TempDir, Router) {
        let root = TempDir::new().unwrap();
        fs::create_dir_all(root.path().join("fixtures")).unwrap();
        fs::write(root.path().join("fixtures/runner"), "fixture").unwrap();
        let catalog = DemoCatalog {
            version: 1,
            demos: vec![DemoDefinition {
                id: "fixture".to_string(),
                title: "Fixture".to_string(),
                category: "Test".to_string(),
                summary: "Fixture".to_string(),
                node_count: 2,
                topology: TopologyKind::Pair,
                lifecycle: DemoLifecycle::Automatic,
                guest_engine: GuestEngine::Initramfs,
                requires_simpler_toolchain: false,
                model: None,
                model_source: None,
                node_input: None,
                data_plane: vec![],
                tags: vec![],
                estimated_duration_secs: 1,
                requires_guest_artifacts: false,
                command: CommandDefinition {
                    program: "fixtures/runner".to_string(),
                    args: vec![],
                    environment: BTreeMap::new(),
                },
                parameters: vec![],
                requirements: vec![],
                required_paths: vec!["fixtures/runner".to_string()],
                controls: vec![],
            }],
        };
        let manager = RunManager::new(root.path(), catalog).unwrap();
        (root, router(manager))
    }

    #[tokio::test]
    async fn serves_health_catalog_and_web_assets() {
        let (_root, app) = test_app();
        for path in [
            "/",
            "/app.js",
            "/styles.css",
            "/api/v1/health",
            "/api/v1/targets",
            "/api/v1/readiness",
        ] {
            let response = app
                .clone()
                .oneshot(Request::builder().uri(path).body(Body::empty()).unwrap())
                .await
                .unwrap();
            assert_eq!(response.status(), StatusCode::OK, "path {path}");
        }
        let response = app
            .clone()
            .oneshot(
                Request::builder()
                    .uri("/api/v1/catalog")
                    .body(Body::empty())
                    .unwrap(),
            )
            .await
            .unwrap();
        let body = response.into_body().collect().await.unwrap().to_bytes();
        assert!(String::from_utf8_lossy(&body).contains("fixture"));

        let response = app
            .clone()
            .oneshot(
                Request::builder()
                    .uri("/api/v1/targets")
                    .body(Body::empty())
                    .unwrap(),
            )
            .await
            .unwrap();
        let body = response.into_body().collect().await.unwrap().to_bytes();
        let registry: crate::target::TargetRegistry = serde_json::from_slice(&body).unwrap();
        assert_eq!(registry.default_target, "local");

        let response = app
            .oneshot(
                Request::builder()
                    .uri("/api/v1/readiness?target=missing")
                    .body(Body::empty())
                    .unwrap(),
            )
            .await
            .unwrap();
        assert_eq!(response.status(), StatusCode::NOT_FOUND);
    }

    #[tokio::test]
    async fn rejects_unknown_demo_without_executing_a_command() {
        let (_root, app) = test_app();
        let response = app
            .oneshot(
                Request::builder()
                    .method("POST")
                    .uri("/api/v1/runs")
                    .header(header::CONTENT_TYPE, "application/json")
                    .body(Body::from(r#"{"demo_id":"arbitrary"}"#))
                    .unwrap(),
            )
            .await
            .unwrap();
        assert_eq!(response.status(), StatusCode::NOT_FOUND);
    }

    #[tokio::test]
    async fn rejects_target_preparation_for_the_local_target() {
        let (_root, app) = test_app();
        let response = app
            .oneshot(
                Request::builder()
                    .method("POST")
                    .uri("/api/v1/targets/local/prepare")
                    .body(Body::empty())
                    .unwrap(),
            )
            .await
            .unwrap();
        assert_eq!(response.status(), StatusCode::BAD_REQUEST);
        let body = response.into_body().collect().await.unwrap().to_bytes();
        assert!(String::from_utf8_lossy(&body).contains("does not need farm preparation"));
    }

    #[tokio::test]
    async fn exposes_node_input_only_for_a_known_run_and_node() {
        let (_root, app) = test_app();
        let response = app
            .oneshot(
                Request::builder()
                    .method("POST")
                    .uri("/api/v1/runs/missing/nodes/nodeA/input")
                    .header(header::CONTENT_TYPE, "application/json")
                    .body(Body::from(r#"{"data":"echo ready","append_newline":true}"#))
                    .unwrap(),
            )
            .await
            .unwrap();
        assert_eq!(response.status(), StatusCode::NOT_FOUND);
    }
}
