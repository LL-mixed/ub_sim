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

use std::collections::HashMap;

use crate::deepseek_v4_flash::DEEPSEEK_V4_FLASH_PROFILE;

/// A single routing decision for one token at one layer: which routed
/// experts are active for this token. Stage 2 models the *selection*
/// result; the real indexer + sinkhorn computation lives in the inference
/// engine (ds4), not the simulator.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct ExpertRouteDecision {
    pub layer_id: u64,
    pub token_index: u64,
    /// Active routed expert ids (top-6 for Flash). Sorted ascending.
    pub active_experts: Vec<u32>,
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
}

#[cfg(test)]
mod tests {
    use super::*;

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
}
