//! Real-weight range check through the bridge. Backing is explicitly mock;
//! W5 guest acceptance is a separate run using the same model callable.
use super::*;
use sim_models::{qwen3_dense, qwen3_dense_reference as reference};

#[derive(Debug, Serialize)]
pub struct Qwen3PtoRangeCheck {
    pub layer_end: u32,
    pub tokens: Vec<u32>,
    pub hidden_max_abs_error: f32,
    pub hidden_max_scaled_error: f32,
    pub kv_max_scaled_error: f32,
    pub terminal_top_token: Option<u64>,
    pub terminal_candidate_max_abs_error: Option<f32>,
    pub terminal_full_vocab_error: Option<FullVocabError>,
    pub read_calls: u64,
    pub write_calls: u64,
    pub fence_calls: u64,
}

#[derive(Debug, Serialize)]
pub struct FullVocabError {
    pub compared_elements: usize,
    pub max_abs_error: f32,
    pub worst_token_id: usize,
    pub absolute_limit: f32,
}

fn compare_full_vocab(actual: &[f32], expected: &[f32]) -> Result<FullVocabError, String> {
    if actual.is_empty() || actual.len() != expected.len() {
        return Err("qwen3_pto_full_vocab_length".into());
    }
    let mut report = FullVocabError {
        compared_elements: actual.len(),
        max_abs_error: 0.0,
        worst_token_id: 0,
        absolute_limit: 0.02,
    };
    for (token, (&actual, &expected)) in actual.iter().zip(expected).enumerate() {
        if !actual.is_finite() || !expected.is_finite() {
            return Err(format!("qwen3_pto_full_vocab_nonfinite:token={token}"));
        }
        let error = (actual - expected).abs();
        if error > report.max_abs_error {
            report.max_abs_error = error;
            report.worst_token_id = token;
        }
    }
    if report.max_abs_error > report.absolute_limit {
        return Err(format!(
            "qwen3_pto_full_vocab_oracle:token={}:error={}:limit={}",
            report.worst_token_id, report.max_abs_error, report.absolute_limit
        ));
    }
    Ok(report)
}

fn half_value(bits: u16) -> f32 {
    let sign = if bits & 0x8000 != 0 { -1.0 } else { 1.0 };
    let exponent = (bits >> 10) & 31;
    let fraction = bits & 1023;
    match exponent {
        0 => sign * f32::from(fraction) * 2f32.powi(-24),
        31 if fraction == 0 => sign * f32::INFINITY,
        31 => f32::NAN,
        _ => sign * (1.0 + f32::from(fraction) / 1024.0) * 2f32.powi(i32::from(exponent) - 15),
    }
}

pub fn run_qwen3_pto_range_check(
    scenario: &Path,
    manifest: &Path,
    weights: &Path,
    layer_end: u32,
    tokens: &[u32],
) -> Result<Qwen3PtoRangeCheck, String> {
    let _lock = BRIDGE_MOCK_ENV_LOCK.lock().map_err(|_| "pto_env_lock")?;
    let _scenario = EnvRestore::set("SIM_UAPI_SCENARIO_CONFIG", scenario);
    let _manifest = EnvRestore::set("SIMPLER_QWEN3_PTO_RANGE_MANIFEST", manifest);
    let _weights = EnvRestore::set("SIM_QWEN3_DENSE_WEIGHTS_PATH", weights);
    let profile = qwen3_dense::profile_from_weights_dir(weights, None, 2, tokens.len() as u64, 1)?;
    let g = sim_uapi::Qwen3PtoRangeGeometry {
        first: 0,
        end: layer_end,
        layers: profile.num_hidden_layers as u32,
        past: 0,
        tokens: tokens.len() as u32,
        hidden: profile.hidden_size as u32,
        intermediate: profile.intermediate_size as u32,
        query_heads: profile.num_attention_heads as u32,
        kv_heads: profile.num_key_value_heads as u32,
        head_dim: profile.head_dim as u32,
        vocab: profile.vocab_size as u32,
        scale_bits: (1.0 / (profile.head_dim as f32).sqrt()).to_bits(),
    };
    let shapes = g.guest_shapes()?;
    if tokens.iter().any(|t| *t >= g.vocab) {
        return Err("qwen3_pto_token_bounds".into());
    }
    let mut regions = Vec::new();
    let mut memrefs = Vec::new();
    for (i, shape) in shapes.iter().enumerate() {
        let dtype = if i == 3 { 1 } else { 0 };
        let bytes = u64::from(shape[0]) * u64::from(shape[1]) * if dtype == 1 { 2 } else { 4 };
        let (role, access, mock_access) = match i {
            3 => (
                sim_qemu::LingquPtoMemrefRole::Output,
                sim_qemu::LINGQU_PTO_UB_GM_WRITE,
                MockAccess::Write,
            ),
            5 if g.end == g.layers => (
                sim_qemu::LingquPtoMemrefRole::Output,
                sim_qemu::LINGQU_PTO_UB_GM_WRITE,
                MockAccess::Write,
            ),
            4 => (
                sim_qemu::LingquPtoMemrefRole::InOut,
                sim_qemu::LINGQU_PTO_UB_GM_READ | sim_qemu::LINGQU_PTO_UB_GM_WRITE,
                MockAccess::ReadWrite,
            ),
            _ => (
                sim_qemu::LingquPtoMemrefRole::Input,
                sim_qemu::LINGQU_PTO_UB_GM_READ,
                MockAccess::Read,
            ),
        };
        // Sparse 128 MiB mock slots keep packed KV and full logits disjoint.
        if bytes > 0x0800_0000 {
            return Err("qwen3_pto_mock_slot_overflow".into());
        }
        let slot = i as u64 * 1024;
        let mut memref = authorized_memref(
            i as u64 + 1,
            slot,
            bytes,
            u64::from(shape[0]) * u64::from(shape[1]),
            role,
            access,
            i as u32,
        )
        .map_err(|e| e.to_string())?;
        memref.memref.rank = 2;
        memref.memref.dtype = dtype;
        memref.shape = [shape[0], shape[1], 0, 0, 0];
        memref.strides = [shape[1], 1, 0, 0, 0];
        let mut data = vec![0u8; bytes as usize];
        if i == 2 {
            data = f32s_to_bytes(&tokens.iter().map(|t| *t as f32).collect::<Vec<_>>());
        } else if i == 4 {
            let stride = (10 + 2 * g.tokens * g.kv_heads * g.head_dim) as usize * 4;
            for layer in 0..g.end {
                for (n, v) in [layer, g.tokens, g.tokens, g.tokens, g.kv_heads * g.head_dim]
                    .into_iter()
                    .enumerate()
                {
                    let offset = layer as usize * stride + n * 8;
                    data[offset..offset + 8].copy_from_slice(&u64::from(v).to_le_bytes());
                }
            }
        }
        regions.push(MockRegion {
            binding_id: i as u64 + 1,
            ub_gm_base: memref.binding.ub_gm_base,
            access: mock_access,
            bytes: data,
        });
        memrefs.push(memref);
    }
    let backend = Box::new(Mutex::new(MockBackend {
        request_id: REQUEST_ID,
        regions,
        counters: MockCounters::default(),
    }));
    let scenario_c =
        CString::new(scenario.to_string_lossy().as_bytes()).map_err(|_| "scenario_path")?;
    let bridge = BridgeHandle(sim_qemu::linqu_ub_bridge_new_from_yaml(scenario_c.as_ptr()));
    if bridge.0.is_null() || sim_qemu::linqu_ub_bridge_register_endpoint(bridge.0, 1, 0) != 0 {
        return Err("qwen3_pto_bridge_create".into());
    }
    let ops = sim_qemu::PtoSimUbGmAccessOpsV1 {
        abi_version: sim_qemu::PTO_SIM_UB_GM_ACCESS_ABI_V1,
        struct_bytes: std::mem::size_of::<sim_qemu::PtoSimUbGmAccessOpsV1>() as u32,
        read: Some(mock_read),
        write: Some(mock_write),
        fence: Some(mock_fence),
    };
    let context = (&*backend as *const Mutex<MockBackend>).cast_mut().cast();
    if sim_qemu::linqu_ub_bridge_register_ub_gm_access_v1(bridge.0, &ops, context, PTO_DEVICE_CNA)
        != 0
    {
        return Err("qwen3_pto_bridge_register".into());
    }
    let mut fingerprint = 0;
    if sim_qemu::linqu_ub_bridge_query_ub_gm_callable_v1(bridge.0, 3, &mut fingerprint) != 0 {
        return Err("qwen3_pto_bridge_query".into());
    }
    let scalars: Vec<_> = g
        .scalars()
        .into_iter()
        .enumerate()
        .map(|(i, value)| sim_qemu::LingquPtoScalarV1 {
            abi_version: sim_qemu::LINGQU_PTO_SCALAR_ABI_V1,
            struct_bytes: std::mem::size_of::<sim_qemu::LingquPtoScalarV1>() as u32,
            arg_index: 6 + i as u32,
            dtype: 8,
            flags: 0,
            value: value.into(),
        })
        .collect();
    let mut control = sim_qemu::LingquPtoDispatchControlV2 {
        abi_version: sim_qemu::LINGQU_PTO_DISPATCH_ABI_V2,
        struct_bytes: std::mem::size_of::<sim_qemu::LingquPtoDispatchControlV2>() as u32,
        request_id: REQUEST_ID,
        callable_id: 3,
        memref_count: 6,
        scalar_count: 12,
        memref_table_iova: 0x2000,
        scalar_table_iova: 0x3000,
        artifact_fingerprint: fingerprint,
        metadata_crc32: 0,
        requester_cna: PTO_DEVICE_CNA,
    };
    control.metadata_crc32 = sim_qemu::authorized_metadata_crc32(&control, &memrefs, &scalars)
        .map_err(|e| format!("range_crc:{e:?}"))?;
    if sim_qemu::linqu_ub_bridge_submit_ub_gm_v2(
        bridge.0,
        1,
        1,
        &control,
        memrefs.as_ptr(),
        6,
        scalars.as_ptr(),
        12,
    ) != 0
    {
        return Err("qwen3_pto_bridge_submit".into());
    }
    let (mut submitted, mut pending) = (0, 0);
    if sim_qemu::linqu_ub_bridge_ring_doorbell(bridge.0, 1, 1, &mut submitted, &mut pending) != 0
        || submitted != 1
        || pending != 0
    {
        return Err("qwen3_pto_bridge_doorbell".into());
    }
    let mut completion = [0u8; 64];
    if sim_qemu::linqu_ub_bridge_poll_completion(bridge.0, 1, completion.as_mut_ptr(), 64) != 0 {
        return Err("qwen3_pto_completion_missing".into());
    }
    let completion = sim_qemu::decode_completion(&completion).map_err(|e| format!("{e:?}"))?;
    if completion.status != CompletionStatus::Success {
        return Err(format!("qwen3_pto_range_failed:{:?}", completion.status));
    }
    eprintln!("qwen3-pto-range-check: candidate complete; checking independent reference");

    let p = reference::Qwen3DenseReferenceProfile {
        vocab_size: profile.vocab_size,
        hidden_size: profile.hidden_size,
        intermediate_size: profile.intermediate_size,
        num_hidden_layers: profile.num_hidden_layers,
        num_attention_heads: profile.num_attention_heads,
        num_key_value_heads: profile.num_key_value_heads,
        head_dim: profile.head_dim,
        max_position_embeddings: profile.max_position_embeddings,
        rope_theta: profile.rope_theta,
        prefill_tokens: tokens.len() as u64,
        decode_tokens: 1,
        tp_nodes: 2,
    };
    let loaded = reference::load_safetensors_path_metadata(weights)?;
    let ids: Vec<u64> = tokens.iter().map(|t| u64::from(*t)).collect();
    let input =
        reference::embedding_reference_hidden_sequence_for_profile(p, &loaded.tensors, &ids)?;
    let (expected, sequence) =
        reference::forward_reference_from_hidden_sequence_range_with_kv_cache_for_profile(
            p,
            &loaded.tensors,
            0,
            layer_end.into(),
            &input,
        )?;
    let state = backend.lock().map_err(|_| "qwen3_pto_backend_lock")?;
    let mut maximum = 0f32;
    let mut scaled = 0f32;
    for (bytes, target) in state.regions[3]
        .bytes
        .chunks_exact(2)
        .zip(sequence.iter().flatten())
    {
        let actual = half_value(u16::from_le_bytes(bytes.try_into().unwrap()));
        if !actual.is_finite() {
            return Err("qwen3_pto_nonfinite_hidden".into());
        }
        maximum = maximum.max((actual - target).abs());
        scaled = scaled.max((actual - target).abs() / (1.0 + target.abs()));
    }
    let stride = (10 + 2 * g.tokens * g.kv_heads * g.head_dim) as usize * 4;
    let mut kv_error = 0f32;
    for (layer, cache) in expected.kv_cache.iter().enumerate() {
        let payload = &state.regions[4].bytes[layer * stride + 40..(layer + 1) * stride];
        let target = cache.rope_k_states.iter().chain(&cache.v_states).flatten();
        for (actual, expected) in bytes_to_f32s(payload).into_iter().zip(target) {
            if !actual.is_finite() {
                return Err("qwen3_pto_nonfinite_kv".into());
            }
            kv_error = kv_error.max((actual - expected).abs() / (1.0 + expected.abs()));
        }
    }
    let mut terminal_top_token = None;
    let mut terminal_candidate_max_abs_error = None;
    let mut terminal_full_vocab_error = None;
    if g.end == g.layers {
        let (expected_logits, expected_values) =
            reference::full_vocab_logits_values_from_hidden_for_profile(
                p,
                &loaded.tensors,
                sequence.last().ok_or("empty_range_output")?,
            )?;
        let logits = bytes_to_f32s(&state.regions[5].bytes);
        terminal_full_vocab_error = Some(compare_full_vocab(&logits, &expected_values)?);
        if logits.iter().any(|v| !v.is_finite()) {
            return Err("qwen3_pto_nonfinite_logits".into());
        }
        let top = logits
            .iter()
            .enumerate()
            .max_by(|a, b| a.1.total_cmp(b.1))
            .ok_or("qwen3_pto_empty_logits")?
            .0 as u64;
        let mut error = 0f32;
        for candidate in &expected_logits.top_candidates {
            error = error.max(
                (logits[candidate.token_id as usize] - f32::from_bits(candidate.logit_bits as u32))
                    .abs(),
            );
        }
        if top != expected_logits.top_token_id || error > 0.02 {
            return Err(format!(
                "qwen3_pto_logits_oracle:top={top}:expected={}:error={error}",
                expected_logits.top_token_id
            ));
        }
        terminal_top_token = Some(top);
        terminal_candidate_max_abs_error = Some(error);
    }
    // F16 handoff adds quantization; KV stays F32. Freeze before candidate run.
    if scaled > 0.002
        || kv_error > 0.0002
        || state.counters.read_calls == 0
        || state.counters.write_calls == 0
    {
        return Err(format!(
            "qwen3_pto_range_oracle:hidden={scaled}:kv={kv_error}"
        ));
    }
    Ok(Qwen3PtoRangeCheck {
        layer_end,
        tokens: tokens.to_vec(),
        hidden_max_abs_error: maximum,
        hidden_max_scaled_error: scaled,
        kv_max_scaled_error: kv_error,
        terminal_top_token,
        terminal_candidate_max_abs_error,
        terminal_full_vocab_error,
        read_calls: state.counters.read_calls,
        write_calls: state.counters.write_calls,
        fence_calls: state.counters.fence_calls,
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn full_vocab_checks_non_candidates_and_exact_lengths() {
        let expected = [5.0, 4.0, 3.0, 2.0, -10.0, -20.0];
        let mut actual = expected;
        actual[5] += 0.01;
        let report = compare_full_vocab(&actual, &expected).unwrap();
        assert_eq!(report.compared_elements, 6);
        assert_eq!(report.worst_token_id, 5);
        assert!(report.max_abs_error > 0.009);
        actual[5] += 0.02;
        assert!(compare_full_vocab(&actual, &expected)
            .unwrap_err()
            .contains("token=5"));
        assert!(compare_full_vocab(&expected[..5], &expected).is_err());
        assert!(compare_full_vocab(&[], &[]).is_err());
        actual[5] = f32::NAN;
        assert!(compare_full_vocab(&actual, &expected).is_err());
        assert!(compare_full_vocab(&expected, &actual).is_err());
    }
}
