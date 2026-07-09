//! DeepSeek V4 Flash MoE semantics and expert cache simulator (stage 2).
//!
//! This module models the layer-internal MoE path that distinguishes Flash
//! from dense Qwen3: per-token routing decisions, expert weight on-demand
//! access through the object store, and a node-side LRU expert cache whose
//! hit/miss/eviction statistics feed the latency model.
//!
//! Scope: per the plan (section 3.2 / 3.3), all of this is *layer-internal*
//! — it does not change the cross-layer handoff interface (hidden range +
//! KV state). The cross-layer pipeline mechanics from stage 0/1 are reused
//! unchanged. The expert cache is a node-side optimization layer over
//! objects resolved from mem_service, not a new handoff flow.
//!
//! Reference: DwarfStar ds4_ssd.c and ds4_streaming_hotlist.inc for the
//! LRU + hotlist + hit-statistics semantics.

use std::collections::{BTreeMap, BTreeSet, HashMap};
use std::fs;
use std::path::{Path, PathBuf};

use crate::deepseek_v4_flash::DEEPSEEK_V4_FLASH_PROFILE;

/// A single routing decision for one token at one layer: which routed
/// experts are active for this token. Stage 2 models the *selection*
/// result; the real indexer + sinkhorn computation lives in the inference
/// engine (ds4), not the simulator.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct ExpertRouteDecision {
    pub step_index: u64,
    pub layer_id: u64,
    pub token_index: u64,
    /// Active routed expert ids (top-6 for Flash). Sorted ascending.
    pub active_experts: Vec<u32>,
}

pub const EXPERT_ROUTE_TRACE_SOURCE_FIXTURE: &str = "fixture";
pub const EXPERT_ROUTE_TRACE_SOURCE_DS4_MEASURED: &str = "ds4-measured";
pub const EXPERT_WEIGHT_CATALOG_DEFAULT_PATH_TEMPLATE: &str =
    "layer{layer}/expert{expert}.{quant}.bin";

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct ExpertRouteTraceManifest {
    pub source_kind: String,
    pub model_key: String,
    pub trace_path: String,
    pub trace_checksum: u64,
    pub step_count: u64,
    pub total_layers: u64,
    pub tokens_per_step: u64,
    pub top_k: u64,
}

/// A resolved expert weight-tile reference.
///
/// This is the model-side provider contract used by the simulator: routing
/// chooses `(layer, expert)`, the provider resolves that to a stable
/// object-store key plus accounting metadata. Payload bytes are not materialized
/// here; mem_service/object-service owns placement and lifecycle.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct ExpertWeightTileRef {
    pub model_key: String,
    pub layer_id: u64,
    pub expert_id: u32,
    pub quant: String,
    pub object_key: String,
    pub payload_bytes: u64,
    pub payload_checksum: u64,
}

/// Compute the object-store key for one expert weight tile.
///
/// Addressing follows the plan (section 3.3): `(model, layer, expert_id,
/// quant)`. The quant tag reflects ds4's mixed-precision recipe (routed
/// experts IQ2_XXS gate/up, Q2_K down).
pub fn expert_weight_tile_key(
    model_key: &str,
    layer_id: u64,
    expert_id: u32,
    quant: &str,
) -> String {
    format!("weights/{model_key}/layer{layer_id}/expert{expert_id}/{quant}")
}

fn stable_hash_bytes(bytes: &[u8]) -> u64 {
    let mut hash = 0xcbf29ce484222325u64;
    for &byte in bytes {
        hash ^= u64::from(byte);
        hash = hash.wrapping_mul(0x100000001b3);
    }
    hash
}

pub fn expert_weight_provider_payload_checksum(bytes: &[u8]) -> u64 {
    stable_hash_bytes(bytes)
}

pub fn expert_route_trace_checksum(bytes: &[u8]) -> u64 {
    stable_hash_bytes(bytes)
}

pub const EXPERT_CACHE_DEFAULT_COMPUTE_US_PER_TOUCH: u64 = 2;
pub const EXPERT_CACHE_DEFAULT_LOAD_BYTES_PER_US: u64 = 4096;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct ExpertCacheLatencyEstimate {
    pub compute_time_us: u64,
    pub miss_load_time_us: u64,
    pub estimated_latency_us: u64,
    pub compute_us_per_touch: u64,
    pub load_bytes_per_us: u64,
}

pub fn estimate_expert_cache_latency(
    hits: u64,
    misses: u64,
    pread_bytes: u64,
) -> ExpertCacheLatencyEstimate {
    let compute_us_per_touch = EXPERT_CACHE_DEFAULT_COMPUTE_US_PER_TOUCH;
    let load_bytes_per_us = EXPERT_CACHE_DEFAULT_LOAD_BYTES_PER_US;
    let touch_count = hits.saturating_add(misses);
    let compute_time_us = touch_count.saturating_mul(compute_us_per_touch);
    let miss_load_time_us = if pread_bytes == 0 {
        0
    } else {
        pread_bytes
            .saturating_add(load_bytes_per_us - 1)
            .saturating_div(load_bytes_per_us)
    };
    ExpertCacheLatencyEstimate {
        compute_time_us,
        miss_load_time_us,
        estimated_latency_us: compute_time_us.max(miss_load_time_us),
        compute_us_per_touch,
        load_bytes_per_us,
    }
}

fn stable_checksum_words(words: &[u64]) -> u64 {
    let mut bytes = Vec::with_capacity(words.len() * std::mem::size_of::<u64>());
    for word in words {
        bytes.extend_from_slice(&word.to_le_bytes());
    }
    stable_hash_bytes(&bytes)
}

/// Resolve one routed expert into a deterministic weight-tile object ref.
///
/// Stage 2 does not load the 81GB Flash weights; this provider bridge proves
/// the identity/accounting path that a real provider must satisfy before an
/// 8-node W5 run: stable key, payload byte budget, and checksum.
pub fn deterministic_expert_weight_tile_ref(
    model_key: &str,
    layer_id: u64,
    expert_id: u32,
    quant: &str,
    payload_bytes: u64,
) -> Result<ExpertWeightTileRef, String> {
    if model_key.is_empty() {
        return Err("expert weight tile model key must be non-empty".to_string());
    }
    if layer_id >= DEEPSEEK_V4_FLASH_PROFILE.num_hidden_layers {
        return Err(format!(
            "expert weight tile layer out of range: layer={layer_id} layers={}",
            DEEPSEEK_V4_FLASH_PROFILE.num_hidden_layers
        ));
    }
    if u64::from(expert_id) >= DEEPSEEK_V4_FLASH_PROFILE.num_experts {
        return Err(format!(
            "expert weight tile expert out of range: expert={expert_id} experts={}",
            DEEPSEEK_V4_FLASH_PROFILE.num_experts
        ));
    }
    if quant.is_empty() {
        return Err("expert weight tile quant must be non-empty".to_string());
    }
    if payload_bytes == 0 {
        return Err("expert weight tile payload bytes must be > 0".to_string());
    }
    let object_key = expert_weight_tile_key(model_key, layer_id, expert_id, quant);
    let payload_checksum = stable_checksum_words(&[
        stable_hash_bytes(model_key.as_bytes()),
        layer_id,
        u64::from(expert_id),
        stable_hash_bytes(quant.as_bytes()),
        payload_bytes,
    ]);
    Ok(ExpertWeightTileRef {
        model_key: model_key.to_string(),
        layer_id,
        expert_id,
        quant: quant.to_string(),
        object_key,
        payload_bytes,
        payload_checksum,
    })
}

pub const EXPERT_WEIGHT_PROVIDER_CHECKSUM_ALGORITHM: &str = "deterministic-v1";

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct ExpertWeightProviderSpec {
    pub model_key: String,
    pub quant: String,
    pub payload_bytes: u64,
    pub checksum_algorithm: String,
    pub payload_path: Option<String>,
    pub payload_checksum: Option<u64>,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct ExpertWeightCatalogEntry {
    pub layer_id: u64,
    pub expert_id: u32,
    pub quant: String,
    pub payload_bytes: u64,
    pub payload_checksum: u64,
    pub payload_path: Option<String>,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct ExpertWeightCatalog {
    pub source_kind: String,
    pub model_key: String,
    pub total_layers: u64,
    pub experts_per_layer: u64,
    pub checksum_algorithm: String,
    pub entries: BTreeMap<(u64, u32), ExpertWeightCatalogEntry>,
}

impl ExpertWeightProviderSpec {
    pub fn deterministic(model_key: &str, quant: &str, payload_bytes: u64) -> Result<Self, String> {
        let spec = ExpertWeightProviderSpec {
            model_key: model_key.to_string(),
            quant: quant.to_string(),
            payload_bytes,
            checksum_algorithm: EXPERT_WEIGHT_PROVIDER_CHECKSUM_ALGORITHM.to_string(),
            payload_path: None,
            payload_checksum: None,
        };
        validate_expert_weight_provider_spec(&spec)?;
        Ok(spec)
    }
}

fn set_provider_field(
    slot: &mut Option<String>,
    key: &str,
    value: &str,
    line_no: usize,
) -> Result<(), String> {
    if slot.is_some() {
        return Err(format!("duplicate provider field on line {line_no}: {key}"));
    }
    if value.is_empty() {
        return Err(format!("empty provider field on line {line_no}: {key}"));
    }
    *slot = Some(value.to_string());
    Ok(())
}

fn parse_provider_u64_field(value: &str, key: &str, line_no: usize) -> Result<u64, String> {
    let normalized = value.replace('_', "");
    if let Some(hex) = normalized
        .strip_prefix("0x")
        .or_else(|| normalized.strip_prefix("0X"))
    {
        u64::from_str_radix(hex, 16)
            .map_err(|_| format!("invalid provider {key} on line {line_no}: {value}"))
    } else {
        normalized
            .parse::<u64>()
            .map_err(|_| format!("invalid provider {key} on line {line_no}: {value}"))
    }
}

pub fn validate_expert_weight_provider_spec(spec: &ExpertWeightProviderSpec) -> Result<(), String> {
    if spec.model_key != crate::deepseek_v4_flash::DEEPSEEK_V4_FLASH_MODEL_KEY {
        return Err(format!(
            "expert weight provider model mismatch: got={} expected={}",
            spec.model_key,
            crate::deepseek_v4_flash::DEEPSEEK_V4_FLASH_MODEL_KEY
        ));
    }
    if spec.quant.is_empty() {
        return Err("expert weight provider quant must be non-empty".to_string());
    }
    if spec.payload_bytes == 0 {
        return Err("expert weight provider payload_bytes must be > 0".to_string());
    }
    if spec.checksum_algorithm != EXPERT_WEIGHT_PROVIDER_CHECKSUM_ALGORITHM {
        return Err(format!(
            "unsupported expert weight provider checksum algorithm: {}",
            spec.checksum_algorithm
        ));
    }
    if spec.payload_path.as_deref() == Some("") {
        return Err("expert weight provider payload_path must be non-empty".to_string());
    }
    if spec.payload_path.is_some() && spec.payload_checksum.is_none() {
        return Err("expert weight provider payload_path requires payload_checksum".to_string());
    }
    if spec.payload_path.is_none() && spec.payload_checksum.is_some() {
        return Err("expert weight provider payload_checksum requires payload_path".to_string());
    }
    deterministic_expert_weight_tile_ref(&spec.model_key, 0, 0, &spec.quant, spec.payload_bytes)
        .map(|_| ())
}

fn provider_payload_path(
    spec: &ExpertWeightProviderSpec,
    base_dir: Option<&Path>,
) -> Option<PathBuf> {
    let path = PathBuf::from(spec.payload_path.as_ref()?);
    if path.is_absolute() {
        Some(path)
    } else {
        Some(base_dir.unwrap_or_else(|| Path::new(".")).join(path))
    }
}

pub fn validate_expert_weight_provider_payload_bytes(
    spec: &ExpertWeightProviderSpec,
    payload: &[u8],
) -> Result<(), String> {
    validate_expert_weight_provider_spec(spec)?;
    let Some(expected_checksum) = spec.payload_checksum else {
        return Ok(());
    };
    if payload.len() as u64 != spec.payload_bytes {
        return Err(format!(
            "expert weight provider payload size mismatch: got={} expected={}",
            payload.len(),
            spec.payload_bytes
        ));
    }
    let actual_checksum = expert_weight_provider_payload_checksum(payload);
    if actual_checksum != expected_checksum {
        return Err(format!(
            "expert weight provider payload checksum mismatch: got=0x{actual_checksum:016x} expected=0x{expected_checksum:016x}"
        ));
    }
    Ok(())
}

pub fn validate_expert_weight_provider_payload_file(
    spec: &ExpertWeightProviderSpec,
    base_dir: Option<&Path>,
) -> Result<Option<PathBuf>, String> {
    validate_expert_weight_provider_spec(spec)?;
    let Some(path) = provider_payload_path(spec, base_dir) else {
        return Ok(None);
    };
    let payload = fs::read(&path).map_err(|err| {
        format!(
            "failed to read expert weight provider payload {}: {err}",
            path.display()
        )
    })?;
    validate_expert_weight_provider_payload_bytes(spec, &payload)?;
    Ok(Some(path))
}

/// Parse a compact weight provider fixture.
///
/// Format: whitespace-separated `key=value` fields, comments starting with
/// `#`, and exactly one value for `model_key`, `quant`, `payload_bytes`, and
/// `checksum_algorithm`. This is intentionally strict so readiness checks fail
/// closed when provider metadata drifts.
pub fn parse_expert_weight_provider_spec(text: &str) -> Result<ExpertWeightProviderSpec, String> {
    let mut model_key = None;
    let mut quant = None;
    let mut payload_bytes = None;
    let mut checksum_algorithm = None;
    let mut payload_path = None;
    let mut payload_checksum = None;
    for (index, raw_line) in text.lines().enumerate() {
        let line_no = index + 1;
        let line = raw_line.split('#').next().unwrap_or("").trim();
        if line.is_empty() {
            continue;
        }
        for field in line.split_whitespace() {
            let (key, value) = field
                .split_once('=')
                .ok_or_else(|| format!("invalid provider field on line {line_no}: {field}"))?;
            match key {
                "model" | "model_key" => set_provider_field(&mut model_key, key, value, line_no)?,
                "quant" => set_provider_field(&mut quant, key, value, line_no)?,
                "payload_bytes" | "expert_bytes" => {
                    if payload_bytes.is_some() {
                        return Err(format!("duplicate provider field on line {line_no}: {key}"));
                    }
                    let bytes = parse_provider_u64_field(value, key, line_no)?;
                    payload_bytes = Some(bytes);
                }
                "checksum" | "checksum_algorithm" => {
                    set_provider_field(&mut checksum_algorithm, key, value, line_no)?
                }
                "payload_path" | "payload_file" => {
                    set_provider_field(&mut payload_path, key, value, line_no)?
                }
                "payload_checksum" | "payload_checksum64" => {
                    if payload_checksum.is_some() {
                        return Err(format!("duplicate provider field on line {line_no}: {key}"));
                    }
                    payload_checksum = Some(parse_provider_u64_field(value, key, line_no)?);
                }
                other => {
                    return Err(format!("unknown provider field on line {line_no}: {other}"));
                }
            }
        }
    }
    let spec = ExpertWeightProviderSpec {
        model_key: model_key.ok_or_else(|| "missing provider model_key".to_string())?,
        quant: quant.ok_or_else(|| "missing provider quant".to_string())?,
        payload_bytes: payload_bytes.ok_or_else(|| "missing provider payload_bytes".to_string())?,
        checksum_algorithm: checksum_algorithm
            .ok_or_else(|| "missing provider checksum_algorithm".to_string())?,
        payload_path,
        payload_checksum,
    };
    validate_expert_weight_provider_spec(&spec)?;
    Ok(spec)
}

pub fn parse_expert_weight_provider_spec_from_file(
    provider_path: &Path,
) -> Result<ExpertWeightProviderSpec, String> {
    let provider_text = fs::read_to_string(provider_path).map_err(|err| {
        format!(
            "failed to read expert weight provider {}: {err}",
            provider_path.display()
        )
    })?;
    let spec = parse_expert_weight_provider_spec(&provider_text)?;
    validate_expert_weight_provider_payload_file(&spec, provider_path.parent())?;
    Ok(spec)
}

fn parse_expert_weight_catalog_tile_field(
    entry: &mut ExpertWeightCatalogEntry,
    key: &str,
    value: &str,
    line_no: usize,
) -> Result<(), String> {
    match key {
        "layer" | "layer_id" => {
            entry.layer_id = parse_provider_u64_field(value, key, line_no)?;
        }
        "expert" | "expert_id" => {
            let expert = parse_provider_u64_field(value, key, line_no)?;
            entry.expert_id = u32::try_from(expert)
                .map_err(|_| format!("expert catalog expert_id overflow on line {line_no}"))?;
        }
        "quant" => entry.quant = value.to_string(),
        "payload_bytes" | "expert_bytes" => {
            entry.payload_bytes = parse_provider_u64_field(value, key, line_no)?;
        }
        "payload_checksum" | "payload_checksum64" | "checksum" => {
            entry.payload_checksum = parse_provider_u64_field(value, key, line_no)?;
        }
        "payload_path" | "payload_file" => entry.payload_path = Some(value.to_string()),
        other => {
            return Err(format!(
                "unknown expert catalog tile field on line {line_no}: {other}"
            ))
        }
    }
    Ok(())
}

fn expert_weight_catalog_payload_path(
    entry: &ExpertWeightCatalogEntry,
    base_dir: Option<&Path>,
) -> Option<PathBuf> {
    let path = PathBuf::from(entry.payload_path.as_ref()?);
    if path.is_absolute() {
        Some(path)
    } else {
        Some(base_dir.unwrap_or_else(|| Path::new(".")).join(path))
    }
}

pub fn validate_expert_weight_catalog(catalog: &ExpertWeightCatalog) -> Result<(), String> {
    if catalog.model_key != crate::deepseek_v4_flash::DEEPSEEK_V4_FLASH_MODEL_KEY {
        return Err(format!(
            "expert weight catalog model mismatch: got={} expected={}",
            catalog.model_key,
            crate::deepseek_v4_flash::DEEPSEEK_V4_FLASH_MODEL_KEY
        ));
    }
    if catalog.source_kind.is_empty() {
        return Err("expert weight catalog source_kind must be non-empty".to_string());
    }
    if catalog.total_layers != DEEPSEEK_V4_FLASH_PROFILE.num_hidden_layers {
        return Err(format!(
            "expert weight catalog layer mismatch: got={} expected={}",
            catalog.total_layers, DEEPSEEK_V4_FLASH_PROFILE.num_hidden_layers
        ));
    }
    if catalog.experts_per_layer != DEEPSEEK_V4_FLASH_PROFILE.num_experts {
        return Err(format!(
            "expert weight catalog expert count mismatch: got={} expected={}",
            catalog.experts_per_layer, DEEPSEEK_V4_FLASH_PROFILE.num_experts
        ));
    }
    if catalog.checksum_algorithm != EXPERT_WEIGHT_PROVIDER_CHECKSUM_ALGORITHM {
        return Err(format!(
            "unsupported expert weight catalog checksum algorithm: {}",
            catalog.checksum_algorithm
        ));
    }
    let expected_entries = catalog
        .total_layers
        .checked_mul(catalog.experts_per_layer)
        .ok_or_else(|| "expert weight catalog expected entry count overflow".to_string())?;
    if catalog.entries.len() as u64 != expected_entries {
        return Err(format!(
            "expert weight catalog coverage mismatch: got={} expected={expected_entries}",
            catalog.entries.len()
        ));
    }
    for layer_id in 0..catalog.total_layers {
        for expert_id in 0..catalog.experts_per_layer {
            let expert_id = u32::try_from(expert_id)
                .map_err(|_| "expert weight catalog expert id overflow".to_string())?;
            let entry = catalog.entries.get(&(layer_id, expert_id)).ok_or_else(|| {
                format!("expert weight catalog missing tile: layer={layer_id} expert={expert_id}")
            })?;
            if entry.layer_id != layer_id || entry.expert_id != expert_id {
                return Err(format!(
                    "expert weight catalog tile key mismatch: key=({layer_id},{expert_id}) entry=({},{})",
                    entry.layer_id, entry.expert_id
                ));
            }
            if entry.quant.is_empty() {
                return Err(format!(
                    "expert weight catalog tile quant must be non-empty: layer={layer_id} expert={expert_id}"
                ));
            }
            if entry.payload_bytes == 0 {
                return Err(format!(
                    "expert weight catalog tile payload_bytes must be > 0: layer={layer_id} expert={expert_id}"
                ));
            }
            if entry.payload_checksum == 0 {
                return Err(format!(
                    "expert weight catalog tile payload_checksum must be non-zero: layer={layer_id} expert={expert_id}"
                ));
            }
            if entry.payload_path.as_deref() == Some("") {
                return Err(format!(
                    "expert weight catalog tile payload_path must be non-empty: layer={layer_id} expert={expert_id}"
                ));
            }
        }
    }
    Ok(())
}

pub fn validate_expert_weight_catalog_payload_files(
    catalog: &ExpertWeightCatalog,
    base_dir: Option<&Path>,
) -> Result<(), String> {
    validate_expert_weight_catalog(catalog)?;
    for entry in catalog.entries.values() {
        let Some(path) = expert_weight_catalog_payload_path(entry, base_dir) else {
            continue;
        };
        let payload = fs::read(&path).map_err(|err| {
            format!(
                "failed to read expert weight catalog payload {}: {err}",
                path.display()
            )
        })?;
        if payload.len() as u64 != entry.payload_bytes {
            return Err(format!(
                "expert weight catalog payload size mismatch: path={} got={} expected={}",
                path.display(),
                payload.len(),
                entry.payload_bytes
            ));
        }
        let actual = expert_weight_provider_payload_checksum(&payload);
        if actual != entry.payload_checksum {
            return Err(format!(
                "expert weight catalog payload checksum mismatch: path={} got=0x{actual:016x} expected=0x{:016x}",
                path.display(),
                entry.payload_checksum
            ));
        }
    }
    Ok(())
}

fn reject_catalog_payload_path(path: &str) -> Result<(), String> {
    if path.is_empty() {
        return Err("expert weight catalog payload path must be non-empty".to_string());
    }
    if path.split_whitespace().count() != 1 {
        return Err(format!(
            "expert weight catalog payload path must not contain whitespace: {path}"
        ));
    }
    let path = Path::new(path);
    if path.is_absolute() {
        return Err("expert weight catalog payload path must be relative".to_string());
    }
    if path
        .components()
        .any(|component| matches!(component, std::path::Component::ParentDir))
    {
        return Err("expert weight catalog payload path must not contain ..".to_string());
    }
    Ok(())
}

pub fn render_expert_weight_payload_path_template(
    template: &str,
    layer_id: u64,
    expert_id: u32,
    quant: &str,
) -> Result<String, String> {
    if template.is_empty() {
        return Err("expert weight catalog payload template must be non-empty".to_string());
    }
    if !template.contains("{layer}") || !template.contains("{expert}") {
        return Err(
            "expert weight catalog payload template must contain {layer} and {expert}".to_string(),
        );
    }
    let rendered = template
        .replace("{layer}", &layer_id.to_string())
        .replace("{expert}", &expert_id.to_string())
        .replace("{quant}", quant);
    reject_catalog_payload_path(&rendered)?;
    Ok(rendered)
}

pub fn expert_weight_catalog_from_provider_spec(
    source_kind: &str,
    provider: &ExpertWeightProviderSpec,
) -> Result<ExpertWeightCatalog, String> {
    validate_expert_weight_provider_spec(provider)?;
    if source_kind.is_empty() {
        return Err("expert weight catalog source_kind must be non-empty".to_string());
    }
    let mut entries = BTreeMap::new();
    for layer_id in 0..DEEPSEEK_V4_FLASH_PROFILE.num_hidden_layers {
        for expert_id in 0..DEEPSEEK_V4_FLASH_PROFILE.num_experts {
            let expert_id = u32::try_from(expert_id)
                .map_err(|_| "expert id overflow while building catalog".to_string())?;
            let tile = deterministic_expert_weight_tile_ref(
                &provider.model_key,
                layer_id,
                expert_id,
                &provider.quant,
                provider.payload_bytes,
            )?;
            entries.insert(
                (layer_id, expert_id),
                ExpertWeightCatalogEntry {
                    layer_id,
                    expert_id,
                    quant: tile.quant,
                    payload_bytes: tile.payload_bytes,
                    payload_checksum: tile.payload_checksum,
                    payload_path: None,
                },
            );
        }
    }
    let catalog = ExpertWeightCatalog {
        source_kind: source_kind.to_string(),
        model_key: provider.model_key.clone(),
        total_layers: DEEPSEEK_V4_FLASH_PROFILE.num_hidden_layers,
        experts_per_layer: DEEPSEEK_V4_FLASH_PROFILE.num_experts,
        checksum_algorithm: EXPERT_WEIGHT_PROVIDER_CHECKSUM_ALGORITHM.to_string(),
        entries,
    };
    validate_expert_weight_catalog(&catalog)?;
    Ok(catalog)
}

pub fn expert_weight_catalog_from_payload_dir(
    source_kind: &str,
    payload_dir: &Path,
    path_template: &str,
    quant: &str,
) -> Result<ExpertWeightCatalog, String> {
    if source_kind.is_empty() {
        return Err("expert weight catalog source_kind must be non-empty".to_string());
    }
    if quant.is_empty() {
        return Err("expert weight catalog quant must be non-empty".to_string());
    }
    if !payload_dir.is_dir() {
        return Err(format!(
            "expert weight catalog payload dir is missing: {}",
            payload_dir.display()
        ));
    }

    let mut entries = BTreeMap::new();
    for layer_id in 0..DEEPSEEK_V4_FLASH_PROFILE.num_hidden_layers {
        for expert_id in 0..DEEPSEEK_V4_FLASH_PROFILE.num_experts {
            let expert_id = u32::try_from(expert_id)
                .map_err(|_| "expert id overflow while building catalog".to_string())?;
            let rel_path = render_expert_weight_payload_path_template(
                path_template,
                layer_id,
                expert_id,
                quant,
            )?;
            let payload_path = payload_dir.join(&rel_path);
            let payload = fs::read(&payload_path).map_err(|err| {
                format!(
                    "failed to read expert weight payload {}: {err}",
                    payload_path.display()
                )
            })?;
            if payload.is_empty() {
                return Err(format!(
                    "expert weight payload is empty: {}",
                    payload_path.display()
                ));
            }
            entries.insert(
                (layer_id, expert_id),
                ExpertWeightCatalogEntry {
                    layer_id,
                    expert_id,
                    quant: quant.to_string(),
                    payload_bytes: payload.len() as u64,
                    payload_checksum: expert_weight_provider_payload_checksum(&payload),
                    payload_path: Some(rel_path),
                },
            );
        }
    }
    let catalog = ExpertWeightCatalog {
        source_kind: source_kind.to_string(),
        model_key: crate::deepseek_v4_flash::DEEPSEEK_V4_FLASH_MODEL_KEY.to_string(),
        total_layers: DEEPSEEK_V4_FLASH_PROFILE.num_hidden_layers,
        experts_per_layer: DEEPSEEK_V4_FLASH_PROFILE.num_experts,
        checksum_algorithm: EXPERT_WEIGHT_PROVIDER_CHECKSUM_ALGORITHM.to_string(),
        entries,
    };
    validate_expert_weight_catalog_payload_files(&catalog, Some(payload_dir))?;
    Ok(catalog)
}

pub fn format_expert_weight_catalog(catalog: &ExpertWeightCatalog) -> Result<String, String> {
    validate_expert_weight_catalog(catalog)?;
    let mut text = format!(
        "source_kind={} model_key={} total_layers={} experts_per_layer={} checksum_algorithm={}\n",
        catalog.source_kind,
        catalog.model_key,
        catalog.total_layers,
        catalog.experts_per_layer,
        catalog.checksum_algorithm
    );
    for entry in catalog.entries.values() {
        text.push_str(&format!(
            "tile layer={} expert={} quant={} payload_bytes={} payload_checksum=0x{:016x}",
            entry.layer_id,
            entry.expert_id,
            entry.quant,
            entry.payload_bytes,
            entry.payload_checksum
        ));
        if let Some(path) = &entry.payload_path {
            reject_catalog_payload_path(path)?;
            text.push_str(" payload_path=");
            text.push_str(path);
        }
        text.push('\n');
    }
    Ok(text)
}

pub fn expert_weight_catalog_common_payload_bytes(
    catalog: &ExpertWeightCatalog,
) -> Result<u64, String> {
    validate_expert_weight_catalog(catalog)?;
    let mut bytes = None;
    for entry in catalog.entries.values() {
        match bytes {
            None => bytes = Some(entry.payload_bytes),
            Some(expected) if expected == entry.payload_bytes => {}
            Some(expected) => {
                return Err(format!(
                    "expert weight catalog mixed payload_bytes unsupported by stage2 cache model: got={} expected={expected}",
                    entry.payload_bytes
                ));
            }
        }
    }
    bytes.ok_or_else(|| "expert weight catalog contains no entries".to_string())
}

pub fn parse_expert_weight_catalog(text: &str) -> Result<ExpertWeightCatalog, String> {
    let mut source_kind = None;
    let mut model_key = None;
    let mut total_layers = None;
    let mut experts_per_layer = None;
    let mut checksum_algorithm = None;
    let mut entries = BTreeMap::new();

    for (index, raw_line) in text.lines().enumerate() {
        let line_no = index + 1;
        let line = raw_line.split('#').next().unwrap_or("").trim();
        if line.is_empty() {
            continue;
        }
        let mut fields = line.split_whitespace();
        let first = fields
            .next()
            .ok_or_else(|| format!("invalid expert weight catalog line {line_no}"))?;
        if first == "tile" {
            let mut entry = ExpertWeightCatalogEntry {
                layer_id: u64::MAX,
                expert_id: u32::MAX,
                quant: String::new(),
                payload_bytes: 0,
                payload_checksum: 0,
                payload_path: None,
            };
            for field in fields {
                let (key, value) = field.split_once('=').ok_or_else(|| {
                    format!("invalid expert weight catalog tile field on line {line_no}: {field}")
                })?;
                if value.is_empty() {
                    return Err(format!(
                        "empty expert weight catalog tile field on line {line_no}: {key}"
                    ));
                }
                parse_expert_weight_catalog_tile_field(&mut entry, key, value, line_no)?;
            }
            if entry.layer_id == u64::MAX {
                return Err(format!(
                    "missing expert weight catalog tile layer on line {line_no}"
                ));
            }
            if entry.expert_id == u32::MAX {
                return Err(format!(
                    "missing expert weight catalog tile expert on line {line_no}"
                ));
            }
            let key = (entry.layer_id, entry.expert_id);
            if entries.insert(key, entry).is_some() {
                return Err(format!(
                    "duplicate expert weight catalog tile on line {line_no}: layer={} expert={}",
                    key.0, key.1
                ));
            }
            continue;
        }

        let mut all_fields = Vec::new();
        all_fields.push(first);
        all_fields.extend(fields);
        for field in all_fields {
            let (key, value) = field.split_once('=').ok_or_else(|| {
                format!("invalid expert weight catalog field on line {line_no}: {field}")
            })?;
            match key {
                "source_kind" | "source" => {
                    set_provider_field(&mut source_kind, key, value, line_no)?
                }
                "model" | "model_key" => set_provider_field(&mut model_key, key, value, line_no)?,
                "total_layers" | "layers" => {
                    if total_layers.is_some() {
                        return Err(format!(
                            "duplicate expert weight catalog field on line {line_no}: {key}"
                        ));
                    }
                    total_layers = Some(parse_provider_u64_field(value, key, line_no)?);
                }
                "experts_per_layer" | "experts" => {
                    if experts_per_layer.is_some() {
                        return Err(format!(
                            "duplicate expert weight catalog field on line {line_no}: {key}"
                        ));
                    }
                    experts_per_layer = Some(parse_provider_u64_field(value, key, line_no)?);
                }
                "checksum_algorithm" | "checksum" => {
                    set_provider_field(&mut checksum_algorithm, key, value, line_no)?
                }
                other => {
                    return Err(format!(
                        "unknown expert weight catalog field on line {line_no}: {other}"
                    ));
                }
            }
        }
    }

    let catalog = ExpertWeightCatalog {
        source_kind: source_kind
            .ok_or_else(|| "missing expert weight catalog source_kind".to_string())?,
        model_key: model_key
            .ok_or_else(|| "missing expert weight catalog model_key".to_string())?,
        total_layers: total_layers
            .ok_or_else(|| "missing expert weight catalog total_layers".to_string())?,
        experts_per_layer: experts_per_layer
            .ok_or_else(|| "missing expert weight catalog experts_per_layer".to_string())?,
        checksum_algorithm: checksum_algorithm
            .ok_or_else(|| "missing expert weight catalog checksum_algorithm".to_string())?,
        entries,
    };
    validate_expert_weight_catalog(&catalog)?;
    Ok(catalog)
}

pub fn parse_expert_weight_catalog_from_file(
    catalog_path: &Path,
) -> Result<ExpertWeightCatalog, String> {
    let catalog_text = fs::read_to_string(catalog_path).map_err(|err| {
        format!(
            "failed to read expert weight catalog {}: {err}",
            catalog_path.display()
        )
    })?;
    let catalog = parse_expert_weight_catalog(&catalog_text)?;
    validate_expert_weight_catalog_payload_files(&catalog, catalog_path.parent())?;
    Ok(catalog)
}

pub fn validate_expert_route_trace_manifest(
    manifest: &ExpertRouteTraceManifest,
) -> Result<(), String> {
    if manifest.model_key != crate::deepseek_v4_flash::DEEPSEEK_V4_FLASH_MODEL_KEY {
        return Err(format!(
            "route trace manifest model mismatch: got={} expected={}",
            manifest.model_key,
            crate::deepseek_v4_flash::DEEPSEEK_V4_FLASH_MODEL_KEY
        ));
    }
    match manifest.source_kind.as_str() {
        EXPERT_ROUTE_TRACE_SOURCE_FIXTURE | EXPERT_ROUTE_TRACE_SOURCE_DS4_MEASURED => {}
        other => return Err(format!("unsupported route trace source_kind: {other}")),
    }
    if manifest.trace_path.is_empty() {
        return Err("route trace manifest trace_path must be non-empty".to_string());
    }
    if manifest.trace_checksum == 0 {
        return Err("route trace manifest trace_checksum must be non-zero".to_string());
    }
    if manifest.step_count == 0 {
        return Err("route trace manifest step_count must be > 0".to_string());
    }
    if manifest.tokens_per_step == 0 {
        return Err("route trace manifest tokens_per_step must be > 0".to_string());
    }
    if manifest.total_layers != DEEPSEEK_V4_FLASH_PROFILE.num_hidden_layers {
        return Err(format!(
            "route trace manifest layer mismatch: got={} expected={}",
            manifest.total_layers, DEEPSEEK_V4_FLASH_PROFILE.num_hidden_layers
        ));
    }
    if manifest.top_k != DEEPSEEK_V4_FLASH_PROFILE.num_experts_used {
        return Err(format!(
            "route trace manifest top_k mismatch: got={} expected={}",
            manifest.top_k, DEEPSEEK_V4_FLASH_PROFILE.num_experts_used
        ));
    }
    Ok(())
}

pub fn validate_expert_route_trace_coverage(
    trace: &[ExpertRouteDecision],
    manifest: &ExpertRouteTraceManifest,
) -> Result<(), String> {
    validate_expert_route_trace_manifest(manifest)?;
    let expected_count = manifest
        .step_count
        .checked_mul(manifest.tokens_per_step)
        .and_then(|value| value.checked_mul(manifest.total_layers))
        .ok_or_else(|| "route trace manifest expected count overflow".to_string())?;
    if trace.len() as u64 != expected_count {
        return Err(format!(
            "route trace coverage mismatch: got={} expected={expected_count}",
            trace.len()
        ));
    }
    let mut seen = BTreeSet::new();
    for decision in trace {
        validate_expert_route_decision(decision)?;
        if decision.step_index >= manifest.step_count {
            return Err(format!(
                "route trace step out of manifest range: step={} steps={}",
                decision.step_index, manifest.step_count
            ));
        }
        if decision.token_index >= manifest.tokens_per_step {
            return Err(format!(
                "route trace token out of manifest range: token={} tokens_per_step={}",
                decision.token_index, manifest.tokens_per_step
            ));
        }
        if decision.layer_id >= manifest.total_layers {
            return Err(format!(
                "route trace layer out of manifest range: layer={} layers={}",
                decision.layer_id, manifest.total_layers
            ));
        }
        if !seen.insert((decision.step_index, decision.token_index, decision.layer_id)) {
            return Err(format!(
                "duplicate route trace decision: step={} token={} layer={}",
                decision.step_index, decision.token_index, decision.layer_id
            ));
        }
    }
    Ok(())
}

pub fn parse_expert_route_trace_manifest(text: &str) -> Result<ExpertRouteTraceManifest, String> {
    let mut source_kind = None;
    let mut model_key = None;
    let mut trace_path = None;
    let mut trace_checksum = None;
    let mut step_count = None;
    let mut total_layers = None;
    let mut tokens_per_step = None;
    let mut top_k = None;

    for (index, raw_line) in text.lines().enumerate() {
        let line_no = index + 1;
        let line = raw_line.split('#').next().unwrap_or("").trim();
        if line.is_empty() {
            continue;
        }
        for field in line.split_whitespace() {
            let (key, value) = field.split_once('=').ok_or_else(|| {
                format!("invalid route trace manifest field on line {line_no}: {field}")
            })?;
            match key {
                "source_kind" | "source" => {
                    set_provider_field(&mut source_kind, key, value, line_no)?
                }
                "model" | "model_key" => set_provider_field(&mut model_key, key, value, line_no)?,
                "trace_path" | "route_trace" => {
                    set_provider_field(&mut trace_path, key, value, line_no)?
                }
                "trace_checksum" | "trace_checksum64" => {
                    if trace_checksum.is_some() {
                        return Err(format!(
                            "duplicate route trace manifest field on line {line_no}: {key}"
                        ));
                    }
                    trace_checksum = Some(parse_provider_u64_field(value, key, line_no)?);
                }
                "step_count" | "steps" => {
                    if step_count.is_some() {
                        return Err(format!(
                            "duplicate route trace manifest field on line {line_no}: {key}"
                        ));
                    }
                    step_count = Some(parse_provider_u64_field(value, key, line_no)?);
                }
                "total_layers" | "layers" => {
                    if total_layers.is_some() {
                        return Err(format!(
                            "duplicate route trace manifest field on line {line_no}: {key}"
                        ));
                    }
                    total_layers = Some(parse_provider_u64_field(value, key, line_no)?);
                }
                "tokens_per_step" => {
                    if tokens_per_step.is_some() {
                        return Err(format!(
                            "duplicate route trace manifest field on line {line_no}: {key}"
                        ));
                    }
                    tokens_per_step = Some(parse_provider_u64_field(value, key, line_no)?);
                }
                "top_k" => {
                    if top_k.is_some() {
                        return Err(format!(
                            "duplicate route trace manifest field on line {line_no}: {key}"
                        ));
                    }
                    top_k = Some(parse_provider_u64_field(value, key, line_no)?);
                }
                other => {
                    return Err(format!(
                        "unknown route trace manifest field on line {line_no}: {other}"
                    ));
                }
            }
        }
    }

    let manifest = ExpertRouteTraceManifest {
        source_kind: source_kind
            .ok_or_else(|| "missing route trace manifest source_kind".to_string())?,
        model_key: model_key.ok_or_else(|| "missing route trace manifest model_key".to_string())?,
        trace_path: trace_path
            .ok_or_else(|| "missing route trace manifest trace_path".to_string())?,
        trace_checksum: trace_checksum
            .ok_or_else(|| "missing route trace manifest trace_checksum".to_string())?,
        step_count: step_count
            .ok_or_else(|| "missing route trace manifest step_count".to_string())?,
        total_layers: total_layers
            .ok_or_else(|| "missing route trace manifest total_layers".to_string())?,
        tokens_per_step: tokens_per_step
            .ok_or_else(|| "missing route trace manifest tokens_per_step".to_string())?,
        top_k: top_k.ok_or_else(|| "missing route trace manifest top_k".to_string())?,
    };
    validate_expert_route_trace_manifest(&manifest)?;
    Ok(manifest)
}

fn manifest_trace_path(manifest: &ExpertRouteTraceManifest, base_dir: Option<&Path>) -> PathBuf {
    let path = PathBuf::from(&manifest.trace_path);
    if path.is_absolute() {
        path
    } else {
        base_dir.unwrap_or_else(|| Path::new(".")).join(path)
    }
}

pub fn parse_expert_route_trace_from_manifest_file(
    manifest_path: &Path,
) -> Result<(ExpertRouteTraceManifest, Vec<ExpertRouteDecision>), String> {
    let manifest_text = fs::read_to_string(manifest_path).map_err(|err| {
        format!(
            "failed to read route trace manifest {}: {err}",
            manifest_path.display()
        )
    })?;
    let manifest = parse_expert_route_trace_manifest(&manifest_text)?;
    let trace_path = manifest_trace_path(&manifest, manifest_path.parent());
    let trace_bytes = fs::read(&trace_path).map_err(|err| {
        format!(
            "failed to read route trace {} from manifest {}: {err}",
            trace_path.display(),
            manifest_path.display()
        )
    })?;
    let trace_checksum = expert_route_trace_checksum(&trace_bytes);
    if trace_checksum != manifest.trace_checksum {
        return Err(format!(
            "route trace checksum mismatch: got=0x{trace_checksum:016x} expected=0x{:016x}",
            manifest.trace_checksum
        ));
    }
    let trace_text = std::str::from_utf8(&trace_bytes).map_err(|err| {
        format!(
            "route trace {} is not valid UTF-8: {err}",
            trace_path.display()
        )
    })?;
    let trace = parse_expert_route_trace(trace_text)?;
    validate_expert_route_trace_coverage(&trace, &manifest)?;
    Ok((manifest, trace))
}

pub fn infer_expert_route_trace_manifest(
    trace: &[ExpertRouteDecision],
    source_kind: &str,
    trace_path: &str,
    trace_checksum: u64,
) -> Result<ExpertRouteTraceManifest, String> {
    if trace.is_empty() {
        return Err("route trace contains no decisions".to_string());
    }
    let step_count = trace
        .iter()
        .map(|decision| decision.step_index)
        .max()
        .and_then(|value| value.checked_add(1))
        .ok_or_else(|| "route trace step_count overflow".to_string())?;
    let tokens_per_step = trace
        .iter()
        .map(|decision| decision.token_index)
        .max()
        .and_then(|value| value.checked_add(1))
        .ok_or_else(|| "route trace tokens_per_step overflow".to_string())?;
    let manifest = ExpertRouteTraceManifest {
        source_kind: source_kind.to_string(),
        model_key: crate::deepseek_v4_flash::DEEPSEEK_V4_FLASH_MODEL_KEY.to_string(),
        trace_path: trace_path.to_string(),
        trace_checksum,
        step_count,
        total_layers: DEEPSEEK_V4_FLASH_PROFILE.num_hidden_layers,
        tokens_per_step,
        top_k: DEEPSEEK_V4_FLASH_PROFILE.num_experts_used,
    };
    validate_expert_route_trace_coverage(trace, &manifest)?;
    Ok(manifest)
}

pub fn format_expert_route_trace_manifest(manifest: &ExpertRouteTraceManifest) -> String {
    format!(
        "\
source_kind={}\n\
model_key={}\n\
trace_path={}\n\
trace_checksum=0x{:016x}\n\
step_count={}\n\
total_layers={}\n\
tokens_per_step={}\n\
top_k={}\n",
        manifest.source_kind,
        manifest.model_key,
        manifest.trace_path,
        manifest.trace_checksum,
        manifest.step_count,
        manifest.total_layers,
        manifest.tokens_per_step,
        manifest.top_k
    )
}

pub fn expert_weight_tile_ref_from_provider(
    provider: &ExpertWeightProviderSpec,
    layer_id: u64,
    expert_id: u32,
) -> Result<ExpertWeightTileRef, String> {
    validate_expert_weight_provider_spec(provider)?;
    deterministic_expert_weight_tile_ref(
        &provider.model_key,
        layer_id,
        expert_id,
        &provider.quant,
        provider.payload_bytes,
    )
}

pub fn expert_weight_tile_ref_from_catalog(
    catalog: &ExpertWeightCatalog,
    layer_id: u64,
    expert_id: u32,
) -> Result<ExpertWeightTileRef, String> {
    validate_expert_weight_catalog(catalog)?;
    let entry = catalog.entries.get(&(layer_id, expert_id)).ok_or_else(|| {
        format!("expert weight catalog missing tile: layer={layer_id} expert={expert_id}")
    })?;
    Ok(ExpertWeightTileRef {
        model_key: catalog.model_key.clone(),
        layer_id,
        expert_id,
        quant: entry.quant.clone(),
        object_key: expert_weight_tile_key(&catalog.model_key, layer_id, expert_id, &entry.quant),
        payload_bytes: entry.payload_bytes,
        payload_checksum: entry.payload_checksum,
    })
}

pub fn validate_expert_route_decision(decision: &ExpertRouteDecision) -> Result<(), String> {
    if decision.layer_id >= DEEPSEEK_V4_FLASH_PROFILE.num_hidden_layers {
        return Err(format!(
            "route layer out of range: layer={} layers={}",
            decision.layer_id, DEEPSEEK_V4_FLASH_PROFILE.num_hidden_layers
        ));
    }
    let top_k = DEEPSEEK_V4_FLASH_PROFILE.num_experts_used as usize;
    if decision.active_experts.len() != top_k {
        return Err(format!(
            "route expert count mismatch: got={} expected={top_k}",
            decision.active_experts.len()
        ));
    }
    let mut sorted = decision.active_experts.clone();
    sorted.sort_unstable();
    sorted.dedup();
    if sorted.len() != decision.active_experts.len() {
        return Err("route experts must be unique".to_string());
    }
    if decision
        .active_experts
        .iter()
        .any(|&expert| u64::from(expert) >= DEEPSEEK_V4_FLASH_PROFILE.num_experts)
    {
        return Err(format!(
            "route expert out of range: experts={}",
            DEEPSEEK_V4_FLASH_PROFILE.num_experts
        ));
    }
    Ok(())
}

fn parse_u64_field(label: &str, value: &str, line_no: usize) -> Result<u64, String> {
    value
        .parse::<u64>()
        .map_err(|_| format!("invalid {label} on trace line {line_no}: {value}"))
}

fn parse_expert_list(value: &str, line_no: usize) -> Result<Vec<u32>, String> {
    let mut experts = Vec::new();
    for raw in value.split(',') {
        let expert = raw
            .trim()
            .parse::<u32>()
            .map_err(|_| format!("invalid expert id on trace line {line_no}: {raw}"))?;
        experts.push(expert);
    }
    experts.sort_unstable();
    let decision = ExpertRouteDecision {
        step_index: 0,
        layer_id: 0,
        token_index: 0,
        active_experts: experts.clone(),
    };
    validate_expert_route_decision(&decision)
        .map_err(|err| format!("{err} on trace line {line_no}"))?;
    Ok(experts)
}

/// Parse a ds4-style Flash route trace.
///
/// Stable text format, one decision per line:
/// `step=0 token=0 layer=7 experts=1,2,3,4,5,6`
///
/// Empty lines and `#` comments are ignored. The parser is deliberately strict:
/// malformed lines, duplicate experts, wrong top-k, and out-of-range layer or
/// expert ids are rejected fail-closed.
pub fn parse_expert_route_trace(text: &str) -> Result<Vec<ExpertRouteDecision>, String> {
    let mut decisions = Vec::new();
    for (index, raw_line) in text.lines().enumerate() {
        let line_no = index + 1;
        let line = raw_line.split('#').next().unwrap_or("").trim();
        if line.is_empty() {
            continue;
        }
        let mut step_index = None;
        let mut token_index = None;
        let mut layer_id = None;
        let mut active_experts = None;
        for field in line.split_whitespace() {
            let (key, value) = field
                .split_once('=')
                .ok_or_else(|| format!("invalid trace field on line {line_no}: {field}"))?;
            match key {
                "step" => step_index = Some(parse_u64_field("step", value, line_no)?),
                "token" => token_index = Some(parse_u64_field("token", value, line_no)?),
                "layer" => layer_id = Some(parse_u64_field("layer", value, line_no)?),
                "experts" | "active_experts" => {
                    active_experts = Some(parse_expert_list(value, line_no)?)
                }
                "model" | "model_key" => {}
                other => {
                    return Err(format!("unknown trace field on line {line_no}: {other}"));
                }
            }
        }
        let decision = ExpertRouteDecision {
            step_index: step_index
                .ok_or_else(|| format!("missing step on trace line {line_no}"))?,
            token_index: token_index
                .ok_or_else(|| format!("missing token on trace line {line_no}"))?,
            layer_id: layer_id.ok_or_else(|| format!("missing layer on trace line {line_no}"))?,
            active_experts: active_experts
                .ok_or_else(|| format!("missing experts on trace line {line_no}"))?,
        };
        validate_expert_route_decision(&decision)
            .map_err(|err| format!("{err} on trace line {line_no}"))?;
        decisions.push(decision);
    }
    if decisions.is_empty() {
        return Err("route trace contains no decisions".to_string());
    }
    Ok(decisions)
}

/// Generate a deterministic synthetic routing trace for a decode stream.
///
/// Stage 2 modeling input: given a token count and layer count, produce a
/// route decision per (layer, token). The selection uses a seeded
/// pseudo-random top-6 over 256 experts so the cache simulator has a
/// repeatable access pattern. The plan (section 7.2) recommends driving
/// this from ds4 traces for trustworthy numbers; this synthetic generator
/// is the stage-2 fallback used until real traces are wired in.
pub fn synthetic_route_trace(
    token_count: u64,
    layer_count: u64,
    seed: u64,
) -> Vec<ExpertRouteDecision> {
    synthetic_route_trace_for_step(token_count, layer_count, seed, seed)
}

pub fn synthetic_route_trace_for_step(
    token_count: u64,
    layer_count: u64,
    step_index: u64,
    seed: u64,
) -> Vec<ExpertRouteDecision> {
    let num_experts = DEEPSEEK_V4_FLASH_PROFILE.num_experts as u32;
    let top_k = DEEPSEEK_V4_FLASH_PROFILE.num_experts_used as usize;
    let mut decisions = Vec::with_capacity((token_count * layer_count) as usize);
    for layer_id in 0..layer_count {
        for token_index in 0..token_count {
            // Simple LCG seeded by (layer, token, seed) for repeatability.
            let mut state = seed
                .wrapping_mul(6364136223846793005)
                .wrapping_add(layer_id.wrapping_mul(1442695040888963407))
                .wrapping_add(token_index.wrapping_mul(2246822519));
            let mut picks: Vec<u32> = Vec::with_capacity(top_k);
            while picks.len() < top_k {
                state = state
                    .wrapping_mul(6364136223846793005)
                    .wrapping_add(1442695040888963407);
                let candidate = (state >> 33) as u32 % num_experts;
                if !picks.contains(&candidate) {
                    picks.push(candidate);
                }
            }
            picks.sort_unstable();
            decisions.push(ExpertRouteDecision {
                step_index,
                layer_id,
                token_index,
                active_experts: picks,
            });
        }
    }
    decisions
}

/// Node-side expert cache simulator.
///
/// Models an LRU cache of expert weight tiles with a fixed slot budget and
/// optional hotlist preload (mirroring ds4_streaming_hotlist.inc). As the
/// decode stream touches experts per the routing trace, the simulator
/// records hits, misses, evictions, and the pread byte budget — the inputs
/// to the latency model `max(compute_time, miss_load_time)` (plan §5 stage
/// 2.3). The cache does not hold payload bytes; it only tracks residency.
#[derive(Debug)]
pub struct ExpertCacheSimulator {
    capacity_slots: usize,
    expert_bytes: u64,
    resident: Vec<u64>,                // LRU order: front = least recently used
    resident_lookup: HashMap<u64, ()>, // key = layer*1000+expert for residency check
    pub hits: u64,
    pub misses: u64,
    pub evictions: u64,
    pub pread_bytes: u64,
}

impl ExpertCacheSimulator {
    /// Build a cache with `capacity_slots` slots, where each expert tile is
    /// `expert_bytes` (per-expert FFN weight size for Flash = n_ff_exp *
    /// quant bytes; stage 2 uses a placeholder constant).
    pub fn new(capacity_slots: usize, expert_bytes: u64) -> Self {
        ExpertCacheSimulator {
            capacity_slots,
            expert_bytes,
            resident: Vec::with_capacity(capacity_slots),
            resident_lookup: HashMap::new(),
            hits: 0,
            misses: 0,
            evictions: 0,
            pread_bytes: 0,
        }
    }

    fn key(layer_id: u64, expert_id: u32) -> u64 {
        layer_id * 1000 + expert_id as u64
    }

    /// Preload a hotlist of (layer, expert) pairs at startup (no misses).
    pub fn preload(&mut self, hotlist: &[(u64, u32)]) {
        for &(layer_id, expert_id) in hotlist {
            let key = Self::key(layer_id, expert_id);
            if !self.resident_lookup.contains_key(&key) && self.resident.len() < self.capacity_slots
            {
                self.resident.push(key);
                self.resident_lookup.insert(key, ());
            }
        }
    }

    /// Touch one expert for one layer. Records a hit or a miss (+pread +
    /// possible eviction) and updates LRU order.
    pub fn touch(&mut self, layer_id: u64, expert_id: u32) {
        let key = Self::key(layer_id, expert_id);
        if self.resident_lookup.contains_key(&key) {
            self.hits += 1;
            self.resident.retain(|&k| k != key);
            self.resident.push(key);
            return;
        }
        self.misses += 1;
        self.pread_bytes += self.expert_bytes;
        if self.resident.len() >= self.capacity_slots {
            if let Some(evicted) = self.resident.first().copied() {
                self.resident.remove(0);
                self.resident_lookup.remove(&evicted);
                self.evictions += 1;
            }
        }
        self.resident.push(key);
        self.resident_lookup.insert(key, ());
    }

    /// Replay a full routing trace through the cache.
    pub fn replay(&mut self, trace: &[ExpertRouteDecision]) {
        for decision in trace {
            for &expert_id in &decision.active_experts {
                self.touch(decision.layer_id, expert_id);
            }
        }
    }

    pub fn hit_rate(&self) -> f64 {
        let total = self.hits + self.misses;
        if total == 0 {
            return 0.0;
        }
        self.hits as f64 / total as f64
    }

    pub fn latency_estimate(&self) -> ExpertCacheLatencyEstimate {
        estimate_expert_cache_latency(self.hits, self.misses, self.pread_bytes)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn complete_weight_catalog_text(skip: Option<(u64, u32)>) -> String {
        let mut text = String::from(
            "source_kind=fixture model_key=deepseek-v4-flash total_layers=43 experts_per_layer=256 checksum_algorithm=deterministic-v1\n",
        );
        for layer_id in 0..DEEPSEEK_V4_FLASH_PROFILE.num_hidden_layers {
            for expert_id in 0..DEEPSEEK_V4_FLASH_PROFILE.num_experts {
                let expert_id = expert_id as u32;
                if skip == Some((layer_id, expert_id)) {
                    continue;
                }
                let tile = deterministic_expert_weight_tile_ref(
                    "deepseek-v4-flash",
                    layer_id,
                    expert_id,
                    "iq2_xxs",
                    2048,
                )
                .expect("tile ref");
                text.push_str(&format!(
                    "tile layer={} expert={} quant={} payload_bytes={} payload_checksum=0x{:016x}\n",
                    layer_id, expert_id, tile.quant, tile.payload_bytes, tile.payload_checksum
                ));
            }
        }
        text
    }

    #[test]
    fn expert_weight_tile_key_format() {
        let key = expert_weight_tile_key("deepseek-v4-flash", 7, 42, "iq2_xxs");
        assert_eq!(key, "weights/deepseek-v4-flash/layer7/expert42/iq2_xxs");
    }

    #[test]
    fn synthetic_trace_selects_top6_of_256() {
        let trace = synthetic_route_trace(1, 1, 42);
        assert_eq!(trace.len(), 1);
        let d = &trace[0];
        assert_eq!(d.step_index, 42);
        assert_eq!(d.active_experts.len(), 6, "Flash top-6");
        assert!(d.active_experts.iter().all(|&e| e < 256));
        // sorted ascending and unique
        let mut deduped = d.active_experts.clone();
        deduped.sort_unstable();
        deduped.dedup();
        assert_eq!(deduped.len(), 6, "all 6 selected experts must be unique");
        for window in d.active_experts.windows(2) {
            assert!(window[0] <= window[1], "active experts must be sorted");
        }
    }

    #[test]
    fn cache_cold_miss_then_hit() {
        let mut cache = ExpertCacheSimulator::new(4, 2048 * 1024);
        cache.touch(0, 5); // miss
        assert_eq!(cache.misses, 1);
        assert_eq!(cache.hits, 0);
        cache.touch(0, 5); // hit
        assert_eq!(cache.hits, 1);
        assert_eq!(cache.misses, 1);
    }

    #[test]
    fn cache_evicts_lru_at_capacity() {
        let mut cache = ExpertCacheSimulator::new(2, 1024);
        cache.touch(0, 1); // miss, resident=[1]
        cache.touch(0, 2); // miss, resident=[1,2]
        cache.touch(0, 3); // miss, evict 1, resident=[2,3]
        assert_eq!(cache.evictions, 1);
        assert_eq!(cache.misses, 3);
        cache.touch(0, 1); // miss again (was evicted)
        assert_eq!(cache.misses, 4);
        assert_eq!(cache.evictions, 2);
    }

    #[test]
    fn cache_lru_promotes_on_hit() {
        let mut cache = ExpertCacheSimulator::new(2, 1024);
        cache.touch(0, 1); // resident=[1]
        cache.touch(0, 2); // resident=[1,2]
        cache.touch(0, 1); // hit, promotes 1: resident=[2,1]
        cache.touch(0, 3); // miss, evict 2 (LRU), resident=[1,3]
        assert_eq!(cache.evictions, 1);
        cache.touch(0, 2); // miss (2 was evicted)
        assert_eq!(cache.misses, 4);
    }

    #[test]
    fn cache_hotlist_preload_avoids_misses() {
        let mut cache = ExpertCacheSimulator::new(8, 1024);
        cache.preload(&[(0, 1), (0, 2), (0, 3)]);
        cache.touch(0, 1); // hit (preloaded)
        cache.touch(0, 2); // hit
        assert_eq!(cache.hits, 2);
        assert_eq!(cache.misses, 0);
    }

    #[test]
    fn cache_replay_trace_records_stats() {
        let trace = synthetic_route_trace(4, 3, 7); // 12 decisions, 6 experts each
        let mut cache = ExpertCacheSimulator::new(16, 2048 * 1024);
        cache.replay(&trace);
        let total = cache.hits + cache.misses;
        assert_eq!(total, 12 * 6, "every active expert is one touch");
        assert!(cache.hit_rate() >= 0.0 && cache.hit_rate() <= 1.0);
        assert_eq!(cache.pread_bytes, cache.misses * 2048 * 1024);
    }

    #[test]
    fn cache_latency_estimate_uses_compute_load_max() {
        let latency = estimate_expert_cache_latency(10, 2, 8193);
        assert_eq!(latency.compute_us_per_touch, 2);
        assert_eq!(latency.load_bytes_per_us, 4096);
        assert_eq!(latency.compute_time_us, 24);
        assert_eq!(latency.miss_load_time_us, 3);
        assert_eq!(latency.estimated_latency_us, 24);

        let load_bound = estimate_expert_cache_latency(1, 1, 4096 * 100);
        assert_eq!(load_bound.compute_time_us, 4);
        assert_eq!(load_bound.miss_load_time_us, 100);
        assert_eq!(load_bound.estimated_latency_us, 100);
    }

    #[test]
    fn parses_complete_expert_weight_catalog_and_resolves_tiles() {
        let catalog = parse_expert_weight_catalog(&complete_weight_catalog_text(None))
            .expect("parse complete catalog");
        assert_eq!(catalog.entries.len(), 43 * 256);
        let tile =
            expert_weight_tile_ref_from_catalog(&catalog, 42, 255).expect("resolve catalog tile");
        assert_eq!(
            tile.object_key,
            "weights/deepseek-v4-flash/layer42/expert255/iq2_xxs"
        );
        assert_eq!(tile.payload_bytes, 2048);
        assert_ne!(tile.payload_checksum, 0);
    }

    #[test]
    fn builds_and_formats_complete_catalog_from_provider_spec() {
        let provider =
            ExpertWeightProviderSpec::deterministic("deepseek-v4-flash", "iq2_xxs", 2048)
                .expect("provider");
        let catalog = expert_weight_catalog_from_provider_spec("fixture", &provider)
            .expect("catalog from provider");
        assert_eq!(catalog.entries.len(), 43 * 256);
        assert_eq!(
            expert_weight_catalog_common_payload_bytes(&catalog),
            Ok(2048)
        );

        let text = format_expert_weight_catalog(&catalog).expect("format catalog");
        let reparsed = parse_expert_weight_catalog(&text).expect("parse formatted catalog");
        assert_eq!(reparsed.entries.len(), 43 * 256);
        let tile = expert_weight_tile_ref_from_catalog(&reparsed, 0, 0)
            .expect("resolve first formatted tile");
        assert_eq!(
            tile.object_key,
            "weights/deepseek-v4-flash/layer0/expert0/iq2_xxs"
        );
        assert_eq!(tile.payload_bytes, 2048);
        assert_ne!(tile.payload_checksum, 0);
    }

    #[test]
    fn builds_complete_catalog_from_payload_dir() {
        let dir = std::env::temp_dir().join(format!(
            "deepseek_v4_flash_payload_catalog_{}",
            std::process::id()
        ));
        let _ = fs::remove_dir_all(&dir);
        for layer_id in 0..DEEPSEEK_V4_FLASH_PROFILE.num_hidden_layers {
            let layer_dir = dir.join(format!("layer{layer_id}"));
            fs::create_dir_all(&layer_dir).expect("create layer dir");
            for expert_id in 0..DEEPSEEK_V4_FLASH_PROFILE.num_experts {
                let path = layer_dir.join(format!("expert{expert_id}.iq2_xxs.bin"));
                fs::write(
                    path,
                    [
                        layer_id as u8,
                        expert_id as u8,
                        (expert_id >> 8) as u8,
                        0x5a,
                    ],
                )
                .expect("write expert payload");
            }
        }

        let catalog = expert_weight_catalog_from_payload_dir(
            "ds4-measured",
            &dir,
            EXPERT_WEIGHT_CATALOG_DEFAULT_PATH_TEMPLATE,
            "iq2_xxs",
        )
        .expect("catalog from payload dir");
        assert_eq!(catalog.entries.len(), 43 * 256);
        let entry = catalog.entries.get(&(42, 255)).expect("last entry");
        assert_eq!(entry.payload_bytes, 4);
        assert_eq!(
            entry.payload_path.as_deref(),
            Some("layer42/expert255.iq2_xxs.bin")
        );
        let text = format_expert_weight_catalog(&catalog).expect("format payload catalog");
        let reparsed = parse_expert_weight_catalog(&text).expect("parse payload catalog");
        validate_expert_weight_catalog_payload_files(&reparsed, Some(&dir))
            .expect("validate payload files");

        fs::remove_dir_all(&dir).expect("remove payload dir");
    }

    #[test]
    fn rejects_incomplete_expert_weight_catalog() {
        let err = parse_expert_weight_catalog(&complete_weight_catalog_text(Some((42, 255))))
            .expect_err("missing catalog tile must fail");
        assert!(err.contains("coverage mismatch"), "{err}");
    }

    #[test]
    fn parses_ds4_style_route_trace() {
        let trace = parse_expert_route_trace(
            "\
            # step token layer experts\n\
            step=0 token=0 layer=7 experts=1,2,3,4,5,6\n\
            step=1 token=0 layer=8 active_experts=6,5,4,3,2,1 model=deepseek-v4-flash\n",
        )
        .expect("parse route trace");
        assert_eq!(trace.len(), 2);
        assert_eq!(trace[0].step_index, 0);
        assert_eq!(trace[0].layer_id, 7);
        assert_eq!(trace[1].step_index, 1);
        assert_eq!(trace[1].active_experts, vec![1, 2, 3, 4, 5, 6]);
    }

    #[test]
    fn parses_checked_in_ds4_style_route_trace_fixture() {
        let trace_text = include_str!("../fixtures/deepseek_v4_flash_route_trace.ds4.txt");
        let trace = parse_expert_route_trace(trace_text).expect("parse fixture");
        assert_eq!(
            trace.len(),
            2 * DEEPSEEK_V4_FLASH_PROFILE.num_hidden_layers as usize
        );
        assert_eq!(trace[0].step_index, 0);
        assert_eq!(trace[0].layer_id, 0);
        assert_eq!(trace[42].layer_id, 42);
        assert_eq!(trace[42].active_experts, vec![0, 1, 252, 253, 254, 255]);
        assert_eq!(trace[43].step_index, 1);
        assert_eq!(trace[43].layer_id, 0);
        assert_eq!(trace[85].step_index, 1);
        assert_eq!(trace[85].layer_id, 42);
        for decision in &trace {
            validate_expert_route_decision(decision).expect("valid fixture decision");
        }
    }

    #[test]
    fn parses_checked_in_route_trace_manifest_fixture() {
        let (manifest, trace) = parse_expert_route_trace_from_manifest_file(Path::new(
            "fixtures/deepseek_v4_flash_route_trace.manifest.txt",
        ))
        .expect("parse route trace manifest fixture");
        assert_eq!(manifest.source_kind, EXPERT_ROUTE_TRACE_SOURCE_FIXTURE);
        assert_eq!(manifest.model_key, "deepseek-v4-flash");
        assert_eq!(manifest.step_count, 2);
        assert_eq!(manifest.total_layers, 43);
        assert_eq!(manifest.tokens_per_step, 1);
        assert_eq!(manifest.top_k, 6);
        assert_eq!(trace.len(), 2 * 43);
        validate_expert_route_trace_coverage(&trace, &manifest).expect("coverage");
    }

    #[test]
    fn infers_and_formats_ds4_measured_route_trace_manifest() {
        let trace_text = include_str!("../fixtures/deepseek_v4_flash_route_trace.ds4.txt");
        let trace_bytes = trace_text.as_bytes();
        let trace = parse_expert_route_trace(trace_text).expect("parse fixture trace");
        let manifest = infer_expert_route_trace_manifest(
            &trace,
            EXPERT_ROUTE_TRACE_SOURCE_DS4_MEASURED,
            "deepseek_v4_flash_route_trace.ds4.txt",
            expert_route_trace_checksum(trace_bytes),
        )
        .expect("infer measured manifest");
        assert_eq!(manifest.source_kind, EXPERT_ROUTE_TRACE_SOURCE_DS4_MEASURED);
        assert_eq!(manifest.step_count, 2);
        assert_eq!(manifest.tokens_per_step, 1);
        assert_eq!(manifest.total_layers, 43);
        assert_eq!(manifest.top_k, 6);

        let formatted = format_expert_route_trace_manifest(&manifest);
        assert!(formatted.contains("source_kind=ds4-measured"));
        let reparsed = parse_expert_route_trace_manifest(&formatted).expect("parse formatted");
        assert_eq!(reparsed, manifest);
    }

    #[test]
    fn infer_route_trace_manifest_requires_full_layer_coverage() {
        let trace = parse_expert_route_trace("step=0 token=0 layer=0 experts=1,2,3,4,5,6\n")
            .expect("parse incomplete trace");
        let err = infer_expert_route_trace_manifest(
            &trace,
            EXPERT_ROUTE_TRACE_SOURCE_DS4_MEASURED,
            "incomplete.ds4.txt",
            0x1234,
        )
        .expect_err("incomplete trace must fail");
        assert!(err.contains("coverage mismatch"), "{err}");
    }

    #[test]
    fn route_trace_manifest_fail_closed_guards() {
        let err = parse_expert_route_trace_manifest(
            "source_kind=unknown model_key=deepseek-v4-flash trace_path=x trace_checksum=0x1 step_count=1 total_layers=43 tokens_per_step=1 top_k=6",
        )
        .expect_err("unknown source kind must fail");
        assert!(err.contains("unsupported route trace source_kind"), "{err}");
        let err = parse_expert_route_trace_manifest(
            "source_kind=fixture model_key=deepseek-v4-flash trace_path=x trace_checksum=0x0 step_count=1 total_layers=43 tokens_per_step=1 top_k=6",
        )
        .expect_err("zero checksum must fail");
        assert!(err.contains("trace_checksum must be non-zero"), "{err}");

        let manifest = parse_expert_route_trace_manifest(
            "source_kind=fixture model_key=deepseek-v4-flash trace_path=x trace_checksum=0x1 step_count=1 total_layers=43 tokens_per_step=1 top_k=6",
        )
        .expect("manifest");
        let trace = vec![ExpertRouteDecision {
            step_index: 0,
            token_index: 0,
            layer_id: 0,
            active_experts: vec![0, 1, 2, 3, 4, 5],
        }];
        let err = validate_expert_route_trace_coverage(&trace, &manifest)
            .expect_err("incomplete coverage must fail");
        assert!(err.contains("coverage mismatch"), "{err}");
    }

    #[test]
    fn rejects_bad_route_trace_fail_closed() {
        let err = parse_expert_route_trace("step=0 token=0 layer=7 experts=1,2,3")
            .expect_err("wrong top-k must fail");
        assert!(err.contains("expert count mismatch"), "{err}");
        let err = parse_expert_route_trace("step=0 token=0 layer=99 experts=1,2,3,4,5,6")
            .expect_err("bad layer must fail");
        assert!(err.contains("layer out of range"), "{err}");
    }

    #[test]
    fn deterministic_provider_resolves_weight_tile_ref() {
        let tile = deterministic_expert_weight_tile_ref(
            "deepseek-v4-flash",
            7,
            42,
            "iq2_xxs",
            2048 * 1024,
        )
        .expect("resolve tile");
        assert_eq!(
            tile.object_key,
            "weights/deepseek-v4-flash/layer7/expert42/iq2_xxs"
        );
        assert_eq!(tile.payload_bytes, 2048 * 1024);
        assert_eq!(tile.payload_checksum, 0x22b4d5a1fd527586);
    }

    #[test]
    fn parses_checked_in_weight_provider_fixture() {
        let provider_text =
            include_str!("../fixtures/deepseek_v4_flash_weight_provider.fixture.txt");
        let provider =
            parse_expert_weight_provider_spec(provider_text).expect("parse provider fixture");
        assert_eq!(provider.model_key, "deepseek-v4-flash");
        assert_eq!(provider.quant, "iq2_xxs");
        assert_eq!(provider.payload_bytes, 2048 * 1024);
        assert_eq!(provider.payload_path, None);
        assert_eq!(provider.payload_checksum, None);
        let tile = expert_weight_tile_ref_from_provider(&provider, 7, 42)
            .expect("resolve tile from provider");
        assert_eq!(
            tile.object_key,
            "weights/deepseek-v4-flash/layer7/expert42/iq2_xxs"
        );
        assert_eq!(tile.payload_bytes, provider.payload_bytes);
        assert_eq!(tile.payload_checksum, 0x22b4d5a1fd527586);
    }

    #[test]
    fn parses_checked_in_file_backed_weight_provider_fixture() {
        let provider_path =
            Path::new("fixtures/deepseek_v4_flash_weight_provider.file.fixture.txt");
        let provider = parse_expert_weight_provider_spec_from_file(provider_path)
            .expect("parse file-backed provider fixture");
        assert_eq!(provider.model_key, "deepseek-v4-flash");
        assert_eq!(provider.quant, "iq2_xxs");
        assert_eq!(provider.payload_bytes, 142);
        assert_eq!(
            provider.payload_path.as_deref(),
            Some("deepseek_v4_flash_weight_tile_payload.fixture.txt")
        );
        assert_eq!(provider.payload_checksum, Some(0x5fc28acab010912b));
        let payload =
            include_bytes!("../fixtures/deepseek_v4_flash_weight_tile_payload.fixture.txt");
        validate_expert_weight_provider_payload_bytes(&provider, payload)
            .expect("fixture payload validates");
        let tile = expert_weight_tile_ref_from_provider(&provider, 7, 42)
            .expect("resolve tile from provider");
        assert_eq!(tile.payload_bytes, provider.payload_bytes);
        assert_ne!(tile.payload_checksum, 0);
    }

    #[test]
    fn rejects_bad_weight_provider_fixture_fail_closed() {
        let err = parse_expert_weight_provider_spec(
            "model_key=deepseek-v4-flash quant=iq2_xxs payload_bytes=0 checksum_algorithm=deterministic-v1",
        )
        .expect_err("zero payload must fail");
        assert!(err.contains("payload_bytes must be > 0"), "{err}");
        let err = parse_expert_weight_provider_spec(
            "model_key=qwen3 quant=iq2_xxs payload_bytes=1024 checksum_algorithm=deterministic-v1",
        )
        .expect_err("wrong model must fail");
        assert!(err.contains("model mismatch"), "{err}");
        let err = parse_expert_weight_provider_spec(
            "model_key=deepseek-v4-flash quant=iq2_xxs payload_bytes=1024 checksum_algorithm=deterministic-v1 payload_path=tile.bin",
        )
        .expect_err("path without payload checksum must fail");
        assert!(err.contains("requires payload_checksum"), "{err}");
        let provider = parse_expert_weight_provider_spec(
            "model_key=deepseek-v4-flash quant=iq2_xxs payload_bytes=4 checksum_algorithm=deterministic-v1 payload_path=tile.bin payload_checksum=0x1",
        )
        .expect("parse bad checksum provider");
        let err = validate_expert_weight_provider_payload_bytes(&provider, b"tile")
            .expect_err("bad payload checksum must fail");
        assert!(err.contains("payload checksum mismatch"), "{err}");
    }
}
