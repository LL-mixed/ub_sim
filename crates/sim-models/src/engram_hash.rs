use serde::{Deserialize, Serialize};
use std::collections::HashMap;

pub const ENGRAM_HASH_OFFSET_BASIS: u64 = 0xcbf2_9ce4_8422_2325;
pub const ENGRAM_HASH_PRIME: u64 = 0x0000_0100_0000_01b3;
pub const ENGRAM_HASH_ALGORITHM_VERSION: &str = "fnv1a-x64+length-prefix";

#[derive(Clone, Debug, Eq, PartialEq, Serialize, Deserialize)]
pub struct Qwen3DenseReferenceEngramHashConfig {
    pub version: u64,
    pub projection_checksum: u64,
    pub orders: Vec<u8>,
    pub heads_per_order: usize,
    pub table_rows: u64,
    pub seed: u64,
    pub algorithm: String,
    #[serde(default = "default_engram_hash_offset_basis")]
    pub fnv1a_offset_basis: u64,
    #[serde(default = "default_engram_hash_prime")]
    pub fnv1a_prime: u64,
    #[serde(default)]
    pub table_specs: Vec<Qwen3DenseReferenceEngramHashTableSpec>,
}

#[derive(Clone, Debug, Eq, PartialEq, Serialize, Deserialize)]
pub struct Qwen3DenseReferenceEngramHashTableSpec {
    pub order: u8,
    pub head: u16,
    pub table_rows: u64,
    pub seed: u64,
}

#[derive(Clone, Debug, Eq, PartialEq, Serialize, Deserialize)]
pub struct Qwen3DenseReferenceEngramRuntimeDescriptor {
    pub version: u64,
    pub projection_checksum: u64,
    pub algorithm: String,
    #[serde(default = "default_engram_hash_offset_basis")]
    pub fnv1a_offset_basis: u64,
    #[serde(default = "default_engram_hash_prime")]
    pub fnv1a_prime: u64,
    pub table_specs: Vec<Qwen3DenseReferenceEngramHashTableSpec>,
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct Qwen3DenseReferenceCanonicalNgram {
    pub order: u8,
    pub step_index: u64,
    pub tokens: Vec<u64>,
}

#[derive(Clone, Debug, Eq, PartialEq, Serialize, Deserialize)]
pub struct Qwen3DenseReferenceEngramLookupRequest {
    pub step_index: u64,
    pub order: u8,
    pub head: u16,
    pub row: u64,
    pub exact_key: u64,
}

#[derive(Clone, Debug, Eq, PartialEq, Serialize, Deserialize)]
pub struct Qwen3DenseReferenceEngramLookupPlan {
    pub step_index: u64,
    pub requests: Vec<Qwen3DenseReferenceEngramLookupRequest>,
}

pub fn build_default_engram_hash_config(
    projection_checksum: u64,
    heads_per_order: usize,
    table_rows: u64,
    seed: u64,
) -> Qwen3DenseReferenceEngramHashConfig {
    build_engram_hash_config(
        projection_checksum,
        vec![2, 3],
        heads_per_order,
        table_rows,
        seed,
    )
}

pub fn build_engram_hash_config(
    projection_checksum: u64,
    orders: Vec<u8>,
    heads_per_order: usize,
    table_rows: u64,
    seed: u64,
) -> Qwen3DenseReferenceEngramHashConfig {
    Qwen3DenseReferenceEngramHashConfig {
        version: 1,
        projection_checksum,
        table_specs: default_engram_hash_table_specs(&orders, heads_per_order, table_rows, seed),
        orders,
        heads_per_order,
        table_rows,
        seed,
        algorithm: ENGRAM_HASH_ALGORITHM_VERSION.to_string(),
        fnv1a_offset_basis: ENGRAM_HASH_OFFSET_BASIS,
        fnv1a_prime: ENGRAM_HASH_PRIME,
    }
}

pub fn default_engram_hash_offset_basis() -> u64 {
    ENGRAM_HASH_OFFSET_BASIS
}

pub fn default_engram_hash_prime() -> u64 {
    ENGRAM_HASH_PRIME
}

pub fn validate_engram_hash_config(
    config: &Qwen3DenseReferenceEngramHashConfig,
) -> Result<(), String> {
    if config.algorithm != ENGRAM_HASH_ALGORITHM_VERSION {
        return Err(format!(
            "engram_hash_config_algorithm_unsupported:{}",
            config.algorithm
        ));
    }
    if config.fnv1a_offset_basis != ENGRAM_HASH_OFFSET_BASIS {
        return Err(format!(
            "engram_hash_config_offset_basis_unsupported:{:#x}",
            config.fnv1a_offset_basis
        ));
    }
    if config.fnv1a_prime != ENGRAM_HASH_PRIME {
        return Err(format!(
            "engram_hash_config_prime_unsupported:{:#x}",
            config.fnv1a_prime
        ));
    }
    if config.orders.is_empty() {
        return Err("engram_hash_config_orders_empty".to_string());
    }
    if config.heads_per_order == 0 {
        return Err("engram_hash_config_heads_must_be_positive".to_string());
    }
    if config.heads_per_order > usize::from(u16::MAX) + 1 {
        return Err("engram_hash_config_heads_exceed_u16".to_string());
    }
    if config.table_rows == 0 {
        return Err("engram_hash_config_table_rows_must_be_positive".to_string());
    }
    if config.orders.iter().any(|&order| order == 0) {
        return Err("engram_hash_config_order_must_be_positive".to_string());
    }
    if config.orders.len()
        != config
            .orders
            .iter()
            .collect::<std::collections::BTreeSet<_>>()
            .len()
    {
        return Err("engram_hash_config_orders_must_be_unique".to_string());
    }
    if !config.table_specs.is_empty() {
        let mut seen = std::collections::BTreeSet::new();
        for spec in &config.table_specs {
            if !config.orders.contains(&spec.order) {
                return Err(format!(
                    "engram_hash_config_table_spec_order_unsupported:{}",
                    spec.order
                ));
            }
            if usize::from(spec.head) >= config.heads_per_order {
                return Err(format!(
                    "engram_hash_config_table_spec_head_unsupported:{}",
                    spec.head
                ));
            }
            if spec.table_rows == 0 {
                return Err("engram_hash_config_table_spec_rows_must_be_positive".to_string());
            }
            if !seen.insert((spec.order, spec.head)) {
                return Err("engram_hash_config_table_specs_must_be_unique".to_string());
            }
        }
        for &order in &config.orders {
            for head in 0..config.heads_per_order {
                if !seen.contains(&(order, head as u16)) {
                    return Err(format!(
                        "engram_hash_config_table_spec_missing:{order}:{head}"
                    ));
                }
            }
        }
    }
    Ok(())
}

pub fn build_engram_runtime_descriptor(
    config: &Qwen3DenseReferenceEngramHashConfig,
) -> Result<Qwen3DenseReferenceEngramRuntimeDescriptor, String> {
    Ok(Qwen3DenseReferenceEngramRuntimeDescriptor {
        version: config.version,
        projection_checksum: config.projection_checksum,
        algorithm: config.algorithm.clone(),
        fnv1a_offset_basis: config.fnv1a_offset_basis,
        fnv1a_prime: config.fnv1a_prime,
        table_specs: engram_hash_table_specs(config)?,
    })
}

pub fn engram_hash_table_specs(
    config: &Qwen3DenseReferenceEngramHashConfig,
) -> Result<Vec<Qwen3DenseReferenceEngramHashTableSpec>, String> {
    validate_engram_hash_config(config)?;
    if config.table_specs.is_empty() {
        return Ok(default_engram_hash_table_specs(
            &config.orders,
            config.heads_per_order,
            config.table_rows,
            config.seed,
        ));
    }
    let mut specs = config.table_specs.clone();
    specs.sort_by(|left, right| {
        left.order
            .cmp(&right.order)
            .then_with(|| left.head.cmp(&right.head))
    });
    Ok(specs)
}

fn default_engram_hash_table_specs(
    orders: &[u8],
    heads_per_order: usize,
    table_rows: u64,
    seed: u64,
) -> Vec<Qwen3DenseReferenceEngramHashTableSpec> {
    let mut specs = Vec::new();
    for &order in orders {
        for head in 0..heads_per_order {
            specs.push(Qwen3DenseReferenceEngramHashTableSpec {
                order,
                head: head as u16,
                table_rows,
                seed,
            });
        }
    }
    specs
}

fn engram_hash_table_spec(
    config: &Qwen3DenseReferenceEngramHashConfig,
    order: u8,
    head: u16,
) -> Result<Qwen3DenseReferenceEngramHashTableSpec, String> {
    engram_hash_table_specs(config)?
        .into_iter()
        .find(|spec| spec.order == order && spec.head == head)
        .ok_or_else(|| format!("engram_hash_table_spec_missing:{order}:{head}"))
}

pub fn generate_canonical_suffix_ngrams(
    canonical_history: &[u64],
    orders: &[u8],
) -> Vec<Qwen3DenseReferenceCanonicalNgram> {
    if canonical_history.is_empty() {
        return Vec::new();
    }
    let mut suffixes = Vec::new();
    for step_index in 0..canonical_history.len() {
        for &order in orders {
            let needed = usize::from(order);
            if step_index + 1 < needed {
                continue;
            }
            let start = step_index + 1 - needed;
            let tokens = canonical_history[start..=step_index].to_vec();
            suffixes.push(Qwen3DenseReferenceCanonicalNgram {
                order,
                step_index: step_index as u64,
                tokens,
            });
        }
    }
    suffixes
}

pub fn generate_canonical_suffix_ngrams_from(
    canonical_history: &[u64],
    orders: &[u8],
    from_step: usize,
) -> Vec<Qwen3DenseReferenceCanonicalNgram> {
    if canonical_history.is_empty() || from_step >= canonical_history.len() {
        return Vec::new();
    }
    let mut suffixes = Vec::new();
    for step_index in from_step..canonical_history.len() {
        for &order in orders {
            let needed = usize::from(order);
            if step_index + 1 < needed {
                continue;
            }
            let start = step_index + 1 - needed;
            let tokens = canonical_history[start..=step_index].to_vec();
            suffixes.push(Qwen3DenseReferenceCanonicalNgram {
                order,
                step_index: step_index as u64,
                tokens,
            });
        }
    }
    suffixes
}

pub fn canonical_ngram_checksum(ngram: &[u64]) -> u64 {
    let mut checksum = ENGRAM_HASH_OFFSET_BASIS;
    for byte in (ngram.len() as u64).to_le_bytes() {
        checksum ^= u64::from(byte);
        checksum = checksum.wrapping_mul(ENGRAM_HASH_PRIME);
    }
    for token in ngram {
        for byte in token.to_le_bytes() {
            checksum ^= u64::from(byte);
            checksum = checksum.wrapping_mul(ENGRAM_HASH_PRIME);
        }
    }
    checksum
}

fn hash_word64(value: u64, seed: u64) -> u64 {
    let mut acc = ENGRAM_HASH_OFFSET_BASIS ^ seed;
    for byte in value.to_le_bytes() {
        acc ^= u64::from(byte);
        acc = acc.wrapping_mul(ENGRAM_HASH_PRIME);
    }
    acc
}

pub fn hash_engram_ngram(
    order: u8,
    head: u16,
    ngram: &[u64],
    config: &Qwen3DenseReferenceEngramHashConfig,
) -> Result<u64, String> {
    hash_engram_ngram_v1(order, head, ngram, config)
}

pub fn hash_engram_ngram_v1(
    order: u8,
    head: u16,
    ngram: &[u64],
    config: &Qwen3DenseReferenceEngramHashConfig,
) -> Result<u64, String> {
    if !config.orders.contains(&order) {
        return Err(format!("engram_hash_order_unsupported:{order}"));
    }
    let expected_len = usize::from(order);
    if ngram.len() != expected_len {
        return Err(format!(
            "engram_hash_ngram_length_mismatch:{order}:{expected_len}"
        ));
    }
    if usize::from(head) >= config.heads_per_order {
        return Err(format!("engram_hash_head_unsupported:{head}"));
    }
    let spec = engram_hash_table_spec(config, order, head)?;
    let mut head_salt = hash_word64(u64::from(head), spec.seed);
    let mut acc = head_salt ^ u64::from(order);
    for token in ngram {
        acc ^= hash_word64(*token, head_salt);
        head_salt = head_salt.rotate_left(7).wrapping_add(*token);
        acc = acc.wrapping_mul(ENGRAM_HASH_PRIME);
    }
    Ok(acc % spec.table_rows)
}

pub fn build_engram_lookup_requests(
    canonical_history: &[u64],
    config: &Qwen3DenseReferenceEngramHashConfig,
) -> Result<Vec<Qwen3DenseReferenceEngramLookupRequest>, String> {
    build_engram_lookup_requests_from_step(canonical_history, 0, config)
}

pub fn build_engram_lookup_plans(
    canonical_history: &[u64],
    config: &Qwen3DenseReferenceEngramHashConfig,
) -> Result<Vec<Qwen3DenseReferenceEngramLookupPlan>, String> {
    build_engram_lookup_plans_from_step(canonical_history, 0, config)
}

pub fn build_engram_lookup_plans_from_step(
    canonical_history: &[u64],
    from_step: usize,
    config: &Qwen3DenseReferenceEngramHashConfig,
) -> Result<Vec<Qwen3DenseReferenceEngramLookupPlan>, String> {
    let requests = build_engram_lookup_requests_from_step(canonical_history, from_step, config)?;
    let mut plans: Vec<Qwen3DenseReferenceEngramLookupPlan> = Vec::new();
    for request in requests {
        if plans.is_empty() || plans.last().unwrap().step_index != request.step_index {
            plans.push(Qwen3DenseReferenceEngramLookupPlan {
                step_index: request.step_index,
                requests: Vec::new(),
            });
        }
        if let Some(plan) = plans.last_mut() {
            plan.requests.push(request);
        }
    }
    Ok(plans)
}

pub fn build_exact_canonical_ngram_index(
    projected_history: &[u64],
    ngram_size: usize,
) -> HashMap<u64, Vec<Vec<u64>>> {
    let mut lookup = HashMap::new();
    if ngram_size == 0 || projected_history.len() < ngram_size {
        return lookup;
    }
    if ngram_size > 255 {
        return lookup;
    }
    for window in projected_history.windows(ngram_size) {
        let key = canonical_ngram_checksum(window);
        lookup
            .entry(key)
            .or_insert_with(Vec::new)
            .push(window.to_vec());
    }
    lookup
}

pub fn build_engram_lookup_requests_from_step(
    canonical_history: &[u64],
    from_step: usize,
    config: &Qwen3DenseReferenceEngramHashConfig,
) -> Result<Vec<Qwen3DenseReferenceEngramLookupRequest>, String> {
    validate_engram_hash_config(config)?;
    let mut requests = Vec::new();
    let suffixes =
        generate_canonical_suffix_ngrams_from(canonical_history, &config.orders, from_step);
    for suffix in suffixes {
        let exact_key = canonical_ngram_checksum(&suffix.tokens);
        for head in 0..(config.heads_per_order as u16) {
            let row = hash_engram_ngram(suffix.order, head, &suffix.tokens, config)?;
            requests.push(Qwen3DenseReferenceEngramLookupRequest {
                step_index: suffix.step_index,
                order: suffix.order,
                head,
                row,
                exact_key,
            });
        }
    }
    Ok(requests)
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::fs;
    use std::path::PathBuf;

    fn parse_guest_u64_define(name: &str, text: &str) -> Option<u64> {
        for line in text.lines() {
            let line = line.trim();
            let prefix = format!("#define {name}");
            if !line.starts_with(&prefix) {
                continue;
            }
            let value = line[prefix.len()..].trim();
            let value = value
                .strip_prefix("UINT64_C(")
                .and_then(|value| value.strip_suffix(')'))
                .unwrap_or(value);
            if let Some(raw) = value.strip_prefix("0x") {
                return u64::from_str_radix(raw, 16).ok();
            }
            return value.parse::<u64>().ok();
        }
        None
    }

    fn parse_guest_string_define(name: &str, text: &str) -> Option<String> {
        for line in text.lines() {
            let line = line.trim();
            let prefix = format!("#define {name}");
            if !line.starts_with(&prefix) {
                continue;
            }
            let value = line[prefix.len()..].trim().trim_matches('"');
            return Some(value.to_string());
        }
        None
    }

    #[test]
    fn hash_constants_match_guest_header() {
        let header_path = PathBuf::from(env!("CARGO_MANIFEST_DIR"))
            .join("../../guest-linux/aarch64/common/paper_engram_hash.h");
        let header = fs::read_to_string(&header_path)
            .expect("read paper_engram_hash.h from guest workspace");
        let offset = parse_guest_u64_define("PAPER_ENGRAM_FNV1A_OFFSET_BASIS", &header)
            .expect("guest offset basis constant");
        let prime = parse_guest_u64_define("PAPER_ENGRAM_FNV1A_PRIME", &header)
            .expect("guest prime constant");
        let algorithm = parse_guest_string_define("PAPER_ENGRAM_HASH_ALGORITHM_V1", &header)
            .expect("guest algorithm string");
        assert_eq!(offset, ENGRAM_HASH_OFFSET_BASIS);
        assert_eq!(prime, ENGRAM_HASH_PRIME);
        assert_eq!(algorithm, ENGRAM_HASH_ALGORITHM_VERSION);
    }

    #[test]
    fn hash_config_validation_rejects_invalid() {
        let mut config = build_default_engram_hash_config(0x12, 1, 16, 0x33);
        config.orders.clear();
        config.table_specs.clear();
        assert_eq!(
            validate_engram_hash_config(&config).expect_err("orders empty should reject"),
            "engram_hash_config_orders_empty"
        );
    }

    #[test]
    fn hash_config_validation_rejects_unsupported_algorithm() {
        let mut config = build_default_engram_hash_config(0x12, 1, 16, 0x33);
        config.algorithm = "custom".to_string();
        assert_eq!(
            validate_engram_hash_config(&config).expect_err("unsupported algorithm should reject"),
            "engram_hash_config_algorithm_unsupported:custom"
        );
        assert!(build_engram_runtime_descriptor(&config).is_err());
        assert!(build_engram_lookup_requests(&[7, 8], &config).is_err());
    }

    #[test]
    fn hash_config_validation_rejects_unsupported_hash_constants() {
        let mut config = build_default_engram_hash_config(0x12, 1, 16, 0x33);
        config.fnv1a_prime ^= 1;
        assert_eq!(
            validate_engram_hash_config(&config).expect_err("unsupported prime should reject"),
            "engram_hash_config_prime_unsupported:0x100000001b2"
        );

        let mut config = build_default_engram_hash_config(0x12, 1, 16, 0x33);
        config.fnv1a_offset_basis ^= 1;
        assert_eq!(
            validate_engram_hash_config(&config).expect_err("unsupported offset should reject"),
            "engram_hash_config_offset_basis_unsupported:0xcbf29ce484222324"
        );
    }

    #[test]
    fn hash_config_exports_runtime_table_specs() {
        let config = build_default_engram_hash_config(0x12, 2, 16, 0x33);
        let descriptor = build_engram_runtime_descriptor(&config).expect("runtime descriptor");
        assert_eq!(descriptor.version, 1);
        assert_eq!(descriptor.projection_checksum, 0x12);
        assert_eq!(descriptor.table_specs.len(), 4);
        assert_eq!(
            descriptor.table_specs[0],
            Qwen3DenseReferenceEngramHashTableSpec {
                order: 2,
                head: 0,
                table_rows: 16,
                seed: 0x33,
            }
        );
        assert_eq!(descriptor.table_specs[3].order, 3);
        assert_eq!(descriptor.table_specs[3].head, 1);
    }

    #[test]
    fn hash_config_builder_uses_explicit_orders_for_table_specs() {
        let config = build_engram_hash_config(0x12, vec![2], 2, 16, 0x33);
        validate_engram_hash_config(&config).expect("explicit-order hash config");
        assert_eq!(config.orders, vec![2]);
        assert_eq!(config.table_specs.len(), 2);
        assert!(config.table_specs.iter().all(|spec| spec.order == 2));
        let requests =
            build_engram_lookup_requests(&[7, 8, 9], &config).expect("build lookup requests");
        assert!(requests.iter().all(|request| request.order == 2));
    }

    #[test]
    fn hash_config_rejects_missing_runtime_table_spec() {
        let mut config = build_default_engram_hash_config(0x12, 2, 16, 0x33);
        config.table_specs.pop();
        assert_eq!(
            validate_engram_hash_config(&config).expect_err("missing table spec should reject"),
            "engram_hash_config_table_spec_missing:3:1"
        );
    }

    #[test]
    fn hash_row_uses_order_head_specific_table_rows() {
        let mut config = build_default_engram_hash_config(0x12, 2, 64, 0x33);
        for spec in &mut config.table_specs {
            if spec.order == 3 && spec.head == 1 {
                spec.table_rows = 7;
            }
        }
        let row = hash_engram_ngram(3, 1, &[10, 11, 12], &config).expect("hash row");
        assert!(row < 7);
    }

    #[test]
    fn generate_canonical_suffix_ngrams_is_ordered() {
        let history = vec![1, 2, 3];
        let suffixes = generate_canonical_suffix_ngrams(&history, &[2, 3]);
        assert_eq!(suffixes.len(), 3);
        assert_eq!(suffixes[0].order, 2);
        assert_eq!(suffixes[0].step_index, 1);
        assert_eq!(suffixes[0].tokens, vec![1, 2]);
        assert_eq!(suffixes[1].order, 2);
        assert_eq!(suffixes[1].step_index, 2);
        assert_eq!(suffixes[1].tokens, vec![2, 3]);
        assert_eq!(suffixes[2].order, 3);
        assert_eq!(suffixes[2].step_index, 2);
        assert_eq!(suffixes[2].tokens, vec![1, 2, 3]);
    }

    #[test]
    fn generate_canonical_suffix_ngrams_from_is_incremental() {
        let history = vec![10, 20, 30, 40];
        let suffixes = generate_canonical_suffix_ngrams_from(&history, &[2, 3], 2);
        assert_eq!(suffixes.len(), 4);
        assert_eq!(suffixes[0].order, 2);
        assert_eq!(suffixes[0].step_index, 2);
        assert_eq!(suffixes[0].tokens, vec![20, 30]);
        assert_eq!(suffixes[1].order, 3);
        assert_eq!(suffixes[1].step_index, 2);
        assert_eq!(suffixes[1].tokens, vec![10, 20, 30]);
        assert_eq!(suffixes[2].order, 2);
        assert_eq!(suffixes[2].step_index, 3);
        assert_eq!(suffixes[2].tokens, vec![30, 40]);
        assert_eq!(suffixes[3].order, 3);
        assert_eq!(suffixes[3].step_index, 3);
        assert_eq!(suffixes[3].tokens, vec![20, 30, 40]);
    }

    #[test]
    fn engram_lookup_requests_have_all_heads() {
        let config = build_default_engram_hash_config(0x99, 2, 8, 0x77);
        let requests =
            build_engram_lookup_requests(&[7, 8, 9], &config).expect("build lookup requests");
        assert_eq!(requests.len(), 6);
        assert_eq!(requests[0].order, 2);
        assert_eq!(requests[0].head, 0);
        assert_eq!(requests[1].head, 1);
        assert_eq!(requests[2].order, 2);
        assert_eq!(requests[2].head, 0);
        assert_eq!(requests[3].head, 1);
        assert_eq!(requests[4].order, 3);
        assert_eq!(requests[4].head, 0);
        assert_eq!(requests[5].head, 1);
    }

    #[test]
    fn engram_lookup_requests_from_step_only_append_new() {
        let config = build_default_engram_hash_config(0x99, 2, 8, 0x77);
        let all_requests =
            build_engram_lookup_requests(&[7, 8, 9], &config).expect("build lookup all requests");
        let requests = build_engram_lookup_requests_from_step(&[7, 8, 9], 2, &config)
            .expect("build lookup requests from step");
        assert_eq!(requests, all_requests[2..].to_vec());
    }

    #[test]
    fn engram_lookup_plans_grouped_by_step() {
        let config = build_default_engram_hash_config(0x99, 2, 8, 0x77);
        let plans = build_engram_lookup_plans_from_step(&[7, 8, 9], 0, &config)
            .expect("build lookup plans");
        assert_eq!(plans.len(), 2);
        assert_eq!(plans[0].step_index, 1);
        assert_eq!(plans[0].requests.len(), 2);
        assert_eq!(plans[0].requests[0].order, 2);
        assert_eq!(plans[0].requests[0].head, 0);
        assert_eq!(plans[1].step_index, 2);
        assert_eq!(plans[1].requests.len(), 4);
        assert_eq!(plans[1].requests[0].order, 2);
        assert_eq!(plans[1].requests[0].head, 0);
    }

    #[test]
    fn build_exact_canonical_ngram_index_matches_windows() {
        let index = build_exact_canonical_ngram_index(&[10, 11, 12], 2);
        let index_total_candidates: usize = index.values().map(|entries| entries.len()).sum();
        assert_eq!(index_total_candidates, 2);
        assert!(index.contains_key(&canonical_ngram_checksum(&[10, 11])));
        assert!(index.contains_key(&canonical_ngram_checksum(&[11, 12])));
    }

    #[test]
    fn canonical_ngram_checksum_is_length_prefixed_and_value_sensitive() {
        assert_eq!(canonical_ngram_checksum(&[1, 2]), 0x422d_ee74_521c_4b44);
        assert_eq!(canonical_ngram_checksum(&[2, 1]), 0x122a_9fb5_49f6_7d24);
        assert_eq!(canonical_ngram_checksum(&[1, 2, 3]), 0xb981_0813_92b0_3a26);
        assert_eq!(
            canonical_ngram_checksum(&[10, 11, 12]),
            0xeabd_6a01_2d50_63ab
        );
        assert_ne!(
            canonical_ngram_checksum(&[1, 2, 3]),
            canonical_ngram_checksum(&[10, 11, 12])
        );
    }

    #[test]
    fn engram_row_lookup_is_deterministic() {
        let config = build_default_engram_hash_config(0x99, 2, 64, 0x1111);
        let row_a = hash_engram_ngram(2, 1, &[10, 20], &config).expect("hash row");
        let row_b = hash_engram_ngram(2, 1, &[10, 20], &config).expect("hash row again");
        assert_eq!(row_a, row_b);
    }

    #[test]
    fn hash_contract_v1_matches_expected_vectors() {
        let config = build_default_engram_hash_config(0x99, 2, 1024, 0x1234_5678);
        let row_2_head0 =
            hash_engram_ngram_v1(2, 0, &[1, 2], &config).expect("hash row 2gram head0");
        let row_2_head1 =
            hash_engram_ngram_v1(2, 1, &[1, 2], &config).expect("hash row 2gram head1");
        let row_3_head0 =
            hash_engram_ngram_v1(3, 0, &[1, 2, 3], &config).expect("hash row 3gram head0");

        assert_eq!(row_2_head0, 852);
        assert_eq!(row_2_head1, 157);
        assert_eq!(row_3_head0, 946);
    }
}
