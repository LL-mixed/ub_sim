use serde::{Deserialize, Serialize};
use std::collections::HashMap;

const ENGRAM_HASH_OFFSET_BASIS: u64 = 0xcbf2_9ce4_8422_2325;
const ENGRAM_HASH_PRIME: u64 = 0x0000_0100_0000_01b3;

#[derive(Clone, Debug, Eq, PartialEq, Serialize, Deserialize)]
pub struct Qwen3DenseReferenceEngramHashConfig {
    pub version: u64,
    pub projection_checksum: u64,
    pub orders: Vec<u8>,
    pub heads_per_order: usize,
    pub table_rows: u64,
    pub seed: u64,
    pub algorithm: String,
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
    Qwen3DenseReferenceEngramHashConfig {
        version: 1,
        projection_checksum,
        orders: vec![2, 3],
        heads_per_order,
        table_rows,
        seed,
        algorithm: "fnv1a-x64+length-prefix".to_string(),
    }
}

pub fn validate_engram_hash_config(
    config: &Qwen3DenseReferenceEngramHashConfig,
) -> Result<(), String> {
    if config.orders.is_empty() {
        return Err("engram_hash_config_orders_empty".to_string());
    }
    if config.heads_per_order == 0 {
        return Err("engram_hash_config_heads_must_be_positive".to_string());
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
    Ok(())
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
    for token in ngram {
        let len = token.to_le_bytes();
        checksum ^= *token;
        for byte in len {
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
    let mut head_salt = hash_word64(u64::from(head), config.seed);
    let mut acc = head_salt ^ u64::from(order);
    for token in ngram {
        acc ^= hash_word64(*token, head_salt);
        head_salt = head_salt.rotate_left(7).wrapping_add(*token);
        acc = acc.wrapping_mul(ENGRAM_HASH_PRIME);
    }
    Ok(acc % config.table_rows)
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

    #[test]
    fn hash_config_validation_rejects_invalid() {
        let mut config = build_default_engram_hash_config(0x12, 1, 16, 0x33);
        config.orders.clear();
        assert_eq!(
            validate_engram_hash_config(&config).expect_err("orders empty should reject"),
            "engram_hash_config_orders_empty"
        );
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
    fn engram_row_lookup_is_deterministic() {
        let config = build_default_engram_hash_config(0x99, 2, 64, 0x1111);
        let row_a = hash_engram_ngram(2, 1, &[10, 20], &config).expect("hash row");
        let row_b = hash_engram_ngram(2, 1, &[10, 20], &config).expect("hash row again");
        assert_eq!(row_a, row_b);
    }
}
