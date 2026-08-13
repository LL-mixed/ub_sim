use sim_config::RemoteMemoryModelConfig;

const FNV1A_OFFSET_BASIS: u64 = 0xcbf2_9ce4_8422_2325;
const FNV1A_PRIME: u64 = 0x0000_0100_0000_01b3;
const OUTCOME_LANE: u64 = 0x243f_6a88_85a3_08d3;
const JITTER_LANE: u64 = 0x1319_8a2e_0370_7344;
const TAIL_LANE: u64 = 0xa409_3822_299f_31d0;
const DUPLICATE_LANE: u64 = 0x082e_fa98_ec4e_6c89;
const REORDER_LANE: u64 = 0x4528_21e6_38d0_1377;
const PPM_SCALE: u64 = 1_000_000;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct ObmmOperationIdentity {
    pub map_id: u64,
    pub map_generation: u64,
    pub remote_offset: u64,
    pub length: u32,
    pub per_range_ordinal: u64,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ObmmRemoteOutcome {
    Success,
    Error,
    Drop,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct ObmmRemoteModelDecision {
    pub operation_key: u64,
    pub outcome: ObmmRemoteOutcome,
    pub jitter_ns: i64,
    pub tail_applied: bool,
    pub service_ns: u64,
    pub duplicate: bool,
    pub duplicate_delay_ns: u64,
    pub reorder_key: u64,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct ObmmRemoteCapacityError {
    pub queue_depth: u32,
}

#[derive(Clone, Debug)]
pub struct ObmmRemoteLatencyModel {
    config: RemoteMemoryModelConfig,
    pending: u32,
}

impl ObmmRemoteLatencyModel {
    pub fn new(config: RemoteMemoryModelConfig) -> Self {
        Self { config, pending: 0 }
    }

    pub fn config(&self) -> &RemoteMemoryModelConfig {
        &self.config
    }

    pub fn pending(&self) -> u32 {
        self.pending
    }

    pub fn try_accept(
        &mut self,
        identity: ObmmOperationIdentity,
    ) -> Result<ObmmRemoteModelDecision, ObmmRemoteCapacityError> {
        if self.pending >= self.config.queue_depth {
            return Err(ObmmRemoteCapacityError {
                queue_depth: self.config.queue_depth,
            });
        }
        self.pending += 1;
        Ok(decide(&self.config, identity))
    }

    pub fn release(&mut self) -> bool {
        if self.pending == 0 {
            return false;
        }
        self.pending -= 1;
        true
    }
}

pub fn operation_key(seed: u64, identity: ObmmOperationIdentity) -> u64 {
    let mut hash = FNV1A_OFFSET_BASIS;
    for bytes in [
        seed.to_le_bytes().as_slice(),
        identity.map_id.to_le_bytes().as_slice(),
        identity.map_generation.to_le_bytes().as_slice(),
        identity.remote_offset.to_le_bytes().as_slice(),
        identity.length.to_le_bytes().as_slice(),
        identity.per_range_ordinal.to_le_bytes().as_slice(),
    ] {
        for byte in bytes {
            hash ^= u64::from(*byte);
            hash = hash.wrapping_mul(FNV1A_PRIME);
        }
    }
    hash
}

pub fn decide(
    config: &RemoteMemoryModelConfig,
    identity: ObmmOperationIdentity,
) -> ObmmRemoteModelDecision {
    let key = operation_key(config.seed, identity);
    if !config.enabled {
        return ObmmRemoteModelDecision {
            operation_key: key,
            outcome: ObmmRemoteOutcome::Success,
            jitter_ns: 0,
            tail_applied: false,
            service_ns: 0,
            duplicate: false,
            duplicate_delay_ns: 0,
            reorder_key: lane_draw(key, REORDER_LANE),
        };
    }

    let outcome_draw = lane_draw(key, OUTCOME_LANE) % PPM_SCALE;
    let outcome = if outcome_draw < u64::from(config.drop_ppm) {
        ObmmRemoteOutcome::Drop
    } else if outcome_draw < u64::from(config.drop_ppm) + u64::from(config.error_ppm) {
        ObmmRemoteOutcome::Error
    } else {
        ObmmRemoteOutcome::Success
    };
    let jitter_ns = uniform_signed(lane_draw(key, JITTER_LANE), config.jitter.max_abs_ns);
    let tail_applied =
        lane_draw(key, TAIL_LANE) % PPM_SCALE < u64::from(config.tail.probability_ppm);
    let tail_ns = if tail_applied {
        config.tail.extra_latency_ns
    } else {
        0
    };
    let service_ns = clamp_service_ns(config.fixed_latency_ns, jitter_ns, tail_ns);
    let duplicate = outcome == ObmmRemoteOutcome::Success
        && lane_draw(key, DUPLICATE_LANE) % PPM_SCALE < u64::from(config.duplicate_ppm);

    ObmmRemoteModelDecision {
        operation_key: key,
        outcome,
        jitter_ns,
        tail_applied,
        service_ns,
        duplicate,
        duplicate_delay_ns: if duplicate {
            config.duplicate_delay_ns
        } else {
            0
        },
        reorder_key: lane_draw(key, REORDER_LANE),
    }
}

pub fn sort_eligible(decisions: &mut [ObmmRemoteModelDecision], reorder_window: u32) {
    let window = usize::try_from(reorder_window).unwrap_or(usize::MAX).max(1);
    for chunk in decisions.chunks_mut(window) {
        chunk.sort_by_key(|decision| (decision.service_ns, decision.reorder_key));
    }
}

fn lane_draw(operation_key: u64, lane: u64) -> u64 {
    splitmix64(operation_key ^ lane)
}

fn splitmix64(mut value: u64) -> u64 {
    value = value.wrapping_add(0x9e37_79b9_7f4a_7c15);
    value = (value ^ (value >> 30)).wrapping_mul(0xbf58_476d_1ce4_e5b9);
    value = (value ^ (value >> 27)).wrapping_mul(0x94d0_49bb_1331_11eb);
    value ^ (value >> 31)
}

fn uniform_signed(draw: u64, max_abs_ns: u64) -> i64 {
    if max_abs_ns == 0 {
        return 0;
    }
    let span = u128::from(max_abs_ns) * 2 + 1;
    let sample = u128::from(draw) % span;
    let signed = i128::try_from(sample).expect("jitter sample fits i128") - i128::from(max_abs_ns);
    i64::try_from(signed).expect("validated jitter fits i64")
}

fn clamp_service_ns(fixed_ns: u64, jitter_ns: i64, tail_ns: u64) -> u64 {
    let total = i128::from(fixed_ns) + i128::from(jitter_ns) + i128::from(tail_ns);
    if total <= 0 {
        0
    } else {
        u64::try_from(total).expect("validated service latency fits u64")
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use sim_config::{RemoteMemoryJitterConfig, RemoteMemoryJitterMode, RemoteMemoryTailConfig};

    fn model_config() -> RemoteMemoryModelConfig {
        RemoteMemoryModelConfig {
            enabled: true,
            fixed_latency_ns: 100_000,
            jitter: RemoteMemoryJitterConfig {
                mode: RemoteMemoryJitterMode::Uniform,
                max_abs_ns: 20_000,
            },
            tail: RemoteMemoryTailConfig {
                probability_ppm: 10_000,
                extra_latency_ns: 900_000,
            },
            queue_depth: 2,
            reorder_window: 2,
            drop_ppm: 1_000,
            error_ppm: 2_000,
            duplicate_ppm: 3_000,
            seed: 7,
            ..RemoteMemoryModelConfig::default()
        }
    }

    fn identity(ordinal: u64) -> ObmmOperationIdentity {
        ObmmOperationIdentity {
            map_id: 4,
            map_generation: 2,
            remote_offset: 0x12_3400 + ordinal * 8,
            length: 8,
            per_range_ordinal: ordinal,
        }
    }

    #[test]
    fn operation_key_and_decision_have_stable_golden_values() {
        let decision = decide(&model_config(), identity(3));

        assert_eq!(decision.operation_key, 0xa38f_126b_eeaf_bc89);
        assert_eq!(decision.outcome, ObmmRemoteOutcome::Success);
        assert_eq!(decision.jitter_ns, -15_157);
        assert!(!decision.tail_applied);
        assert_eq!(decision.service_ns, 84_843);
        assert!(!decision.duplicate);
        assert_eq!(decision.reorder_key, 0xf607_72f5_83d5_d19e);
    }

    #[test]
    fn decisions_do_not_depend_on_accept_order() {
        let config = model_config();
        let forward: Vec<_> = (0..16)
            .map(|ordinal| decide(&config, identity(ordinal)))
            .collect();
        let mut reverse: Vec<_> = (0..16)
            .rev()
            .map(|ordinal| decide(&config, identity(ordinal)))
            .collect();
        reverse.reverse();

        assert_eq!(forward, reverse);
    }

    #[test]
    fn capacity_rejection_does_not_create_a_decision() {
        let mut model = ObmmRemoteLatencyModel::new(model_config());
        model.try_accept(identity(0)).expect("slot 0");
        model.try_accept(identity(1)).expect("slot 1");

        let error = model
            .try_accept(identity(2))
            .expect_err("queue must be full");
        assert_eq!(error.queue_depth, 2);
        assert_eq!(model.pending(), 2);
        assert!(model.release());
        assert!(model.try_accept(identity(2)).is_ok());
    }

    #[test]
    fn disabled_model_preserves_zero_delay_success() {
        let decision = decide(&RemoteMemoryModelConfig::default(), identity(0));

        assert_eq!(decision.outcome, ObmmRemoteOutcome::Success);
        assert_eq!(decision.service_ns, 0);
        assert_eq!(decision.jitter_ns, 0);
        assert!(!decision.duplicate);
    }
}
