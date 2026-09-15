//! Operation-owned model inputs read by the provider after SDK acquire/map.
//!
//! This boundary checks identity and bytes; it does not grant service access.
//! The caller must retain its SDK mapping and compute lease until completion.
//! Inputs travel with the queued command, never through the legacy object cache.

use super::*;
use sim_services::managed_object_ref::ManagedObjectRef;

/// The Qwen range KV codec prepends five u64 words to each layer's K/V data.
/// The runtime's configured payload capacity counts only those K/V data bytes.
pub(super) fn kv_capacity_with_headers(data_bytes: usize, layers: u64) -> Result<usize, String> {
    usize::try_from(layers)
        .ok()
        .and_then(|layers| layers.checked_mul(5 * std::mem::size_of::<u64>()))
        .and_then(|headers| data_bytes.checked_add(headers))
        .ok_or_else(|| "qwen3_range_kv_capacity_overflow".to_string())
}

#[derive(Clone, Debug)]
pub struct ManagedModelInput {
    reference: ManagedObjectRef,
    bytes: Arc<[u8]>,
    model_checksum: u64,
}

impl ManagedModelInput {
    pub fn from_provider_bytes(
        reference: ManagedObjectRef,
        bytes: Vec<u8>,
    ) -> Result<Self, String> {
        if reference.access() & 1 == 0 {
            return Err("managed_model_input_read_denied".into());
        }
        if reference.payload_bytes() != bytes.len() as u64 {
            return Err("managed_model_input_length_mismatch".into());
        }
        if reference.payload_checksum()
            != sim_services::managed_object_ref::payload_checksum(&bytes)
        {
            return Err("managed_model_input_checksum_mismatch".into());
        }
        Ok(Self {
            reference,
            model_checksum: qwen3_dense_reference_range_object_payload_checksum(&bytes),
            bytes: bytes.into(),
        })
    }

    pub fn reference(&self) -> &ManagedObjectRef {
        &self.reference
    }

    pub fn bytes(&self) -> &[u8] {
        &self.bytes
    }

    pub(super) fn model_checksum(&self) -> u64 {
        self.model_checksum
    }

    fn into_operand(self) -> ModelObjectBackedOperandView {
        let len = self.bytes.len();
        ModelObjectBackedOperandView {
            object_ref: ModelInputReference::Managed(self.reference.clone()),
            backing: ModelObjectPayloadBacking::Managed(self),
            payload_range: 0..len,
        }
    }
}

pub(super) fn resolve_managed_operands(
    req: &ModelRangeDispatchReq,
    segment: &[u8],
    mut inputs: Vec<ManagedModelInput>,
) -> Result<ModelRangeDispatchOperands, String> {
    // Hidden/token and previous KV are the supported plain W5 input roles.
    // Reject extra roles explicitly until their managed lifecycles are connected.
    if req.object_ref_count > 2 || req.object_ref_count as usize != inputs.len() {
        return Err("managed_model_input_count_mismatch".into());
    }
    if !matches!(
        std::env::var("SIM_UAPI_W4_CHIPBACKEND_PROFILE").as_deref(),
        Ok("qwen3_dense") | Ok("deepseek-v4-flash-simpler") | Ok("deepseek_v4_flash_simpler")
    ) {
        return Err("managed_model_backend_unsupported".into());
    }
    if !matches!(
        qwen3_dense_reference_engram_context_mode_from_env()?,
        Qwen3DenseReferenceEngramContextMode::Disabled
    ) {
        return Err("managed_model_engram_inputs_unsupported".into());
    }
    let contract = model_range_compute_contract(&req.task)?
        .ok_or_else(|| "managed_model_range_contract_missing".to_string())?;
    let start = req.object_ref_table_offset as usize;
    let end = start
        .checked_add(inputs.len() * ManagedObjectRef::BYTE_LEN)
        .ok_or_else(|| "managed_model_input_table_overflow".to_string())?;
    let table = segment
        .get(start..end)
        .ok_or_else(|| "managed_model_input_table_out_of_bounds".to_string())?;
    let mut result = ModelRangeDispatchOperands {
        managed: true,
        ..Default::default()
    };
    for wire in table.chunks_exact(ManagedObjectRef::BYTE_LEN) {
        let reference = ManagedObjectRef::from_le_bytes(wire).map_err(str::to_string)?;
        let index = inputs
            .iter()
            .position(|input| input.reference == reference)
            .ok_or_else(|| "managed_model_input_identity_mismatch".to_string())?;
        let input = inputs.swap_remove(index);
        let destination = match reference.object_kind() as u16 {
            QWEN3_DENSE_PROFILE_OBMM_KIND_HIDDEN_RANGE_RUNTIME_OUTPUT => {
                if contract.layer_start == 0
                    || reference.payload_bytes() != contract.hidden_bytes as u64
                {
                    return Err("managed_model_hidden_shape_mismatch".into());
                }
                &mut result.hidden_input
            }
            QWEN3_DENSE_PROFILE_OBMM_KIND_QWEN3_TOKEN_RESULT => {
                if contract.layer_start != 0 || reference.payload_bytes() != 64 {
                    return Err("managed_model_token_shape_mismatch".into());
                }
                &mut result.input_token
            }
            QWEN3_DENSE_PROFILE_OBMM_KIND_QWEN3_KV_STATE => &mut result.previous_kv,
            _ => return Err("managed_model_input_kind_unsupported".into()),
        };
        if destination.is_some() {
            return Err("managed_model_duplicate_input_role".into());
        }
        *destination = Some(input.into_operand());
    }
    let step = qwen3_dense_runtime_decode_step_from_guest_input(segment);
    let kv_header_present = segment
        .get(QWEN3_DENSE_PROFILE_PREVIOUS_KV_OFFSET..QWEN3_DENSE_PROFILE_PREVIOUS_KV_OFFSET + 8)
        .is_some_and(|header| read_u64_le_at(header, 0) != 0);
    if contract.layer_start > 0 && result.hidden_input.is_none() {
        return Err("managed_model_hidden_input_missing".into());
    }
    if contract.layer_start == 0 && step > 0 && result.input_token.is_none() {
        return Err("managed_model_token_input_missing".into());
    }
    if (step > 0 || kv_header_present) && result.previous_kv.is_none() {
        return Err("managed_model_kv_input_missing".into());
    }
    Ok(result)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn kv_capacity_includes_each_serialized_layer_header() {
        let profile = QWEN3_DENSE_REFERENCE_PROFILE;
        let state = vec![0.5; (profile.num_key_value_heads * profile.head_dim) as usize];
        let layers = (1..3)
            .map(|layer_id| Qwen3DenseReferenceLayerKvCache {
                layer_id,
                token_count: 1,
                rope_k_states: vec![state.clone()],
                v_states: vec![state.clone()],
            })
            .collect::<Vec<_>>();
        let serialized = qwen3_dense_profile_range_kv_payload_from_cache(&layers, 1, 3).unwrap();
        assert_eq!(
            kv_capacity_with_headers(2 * state.len() * 2 * 4, 2).unwrap(),
            serialized.len()
        );
        assert_eq!(kv_capacity_with_headers(8192, 1).unwrap(), 8232);
        assert!(kv_capacity_with_headers(usize::MAX, 1).is_err());
        assert!(kv_capacity_with_headers(0, u64::MAX).is_err());
    }

    fn reference(kind: u16, payload: &[u8]) -> ManagedObjectRef {
        let mut wire = [0u8; 256];
        wire[..8].copy_from_slice(&0x514f_424d_4d52_4546u64.to_le_bytes());
        wire[8..10].copy_from_slice(&2u16.to_le_bytes());
        wire[10..12].copy_from_slice(&kind.to_le_bytes());
        wire[12..14].copy_from_slice(&2u16.to_le_bytes());
        for (offset, value) in [
            (24, 1),
            (40, 4224),
            (48, payload.len() as u64),
            (
                56,
                sim_services::managed_object_ref::payload_checksum(payload),
            ),
            (72, 17),
            (80, 29),
            (88, 4224 + payload.len() as u64),
        ] {
            wire[offset..offset + 8].copy_from_slice(&value.to_le_bytes());
        }
        // Compute the codec's canonical key hash rather than assuming a fixture value.
        let hash = (1_469_598_103_934_665_603u64 ^ u64::from(b'k')).wrapping_mul(1_099_511_628_211);
        wire[32..40].copy_from_slice(&hash.to_le_bytes());
        wire[64..68].copy_from_slice(&256u32.to_le_bytes());
        wire[68..72].copy_from_slice(&1u32.to_le_bytes());
        wire[96] = b'k';
        wire[192] = b'n';
        ManagedObjectRef::from_le_bytes(&wire).unwrap()
    }

    #[test]
    fn payload_validates_read_length_and_checksum() {
        let bytes = vec![0x5a; 64];
        let reference = reference(QWEN3_DENSE_PROFILE_OBMM_KIND_QWEN3_TOKEN_RESULT, &bytes);
        assert!(ManagedModelInput::from_provider_bytes(reference.clone(), bytes.clone()).is_ok());
        let accepted =
            ManagedModelInput::from_provider_bytes(reference.clone(), bytes.clone()).unwrap();
        assert_ne!(accepted.model_checksum(), reference.payload_checksum());
        assert_eq!(accepted.reference(), &reference);
        let mut legacy_hash = *reference.as_le_bytes();
        legacy_hash[56..64].copy_from_slice(&accepted.model_checksum().to_le_bytes());
        assert_eq!(
            ManagedModelInput::from_provider_bytes(
                ManagedObjectRef::from_le_bytes(&legacy_hash).unwrap(),
                bytes.clone()
            )
            .unwrap_err(),
            "managed_model_input_checksum_mismatch"
        );
        assert!(ManagedModelInput::from_provider_bytes(reference.clone(), vec![0; 64]).is_err());
        assert!(ManagedModelInput::from_provider_bytes(reference.clone(), vec![0x5a; 63]).is_err());
        let mut wire = *reference.as_le_bytes();
        wire[68..72].copy_from_slice(&2u32.to_le_bytes());
        assert_eq!(
            ManagedModelInput::from_provider_bytes(
                ManagedObjectRef::from_le_bytes(&wire).unwrap(),
                bytes
            )
            .unwrap_err(),
            "managed_model_input_read_denied"
        );
    }

    #[test]
    fn managed_dispatch_identity_consumption_and_lifetime() {
        const CHILD: &str = "MANAGED_MODEL_INPUT_TEST_CHILD";
        if std::env::var_os(CHILD).is_none() {
            let mut child = std::process::Command::new(std::env::current_exe().unwrap());
            for (key, _) in std::env::vars_os() {
                if key.to_string_lossy().starts_with("SIM_") {
                    child.env_remove(key);
                }
            }
            assert!(child.args(["--exact", "managed_model_input::tests::managed_dispatch_identity_consumption_and_lifetime", "--nocapture"])
                .env(CHILD, "1").env("SIM_UAPI_W4_CHIPBACKEND_PROFILE", "qwen3_dense")
                .status().unwrap().success());
            return;
        }
        let len = qwen3_dense_runtime_hidden_range_bytes() as usize;
        let bytes = vec![0x5a; len];
        let reference = reference(
            QWEN3_DENSE_PROFILE_OBMM_KIND_HIDDEN_RANGE_RUNTIME_OUTPUT,
            &bytes,
        );
        let input =
            ManagedModelInput::from_provider_bytes(reference.clone(), bytes.clone()).unwrap();
        let weak = Arc::downgrade(&input.bytes);
        let req = ModelRangeDispatchReq {
            op_id: 457,
            segment: SegmentHandle(1),
            task: TaskKey {
                logical_system: LogicalSystemId(1),
                scope_depth: 8,
                task_id: 457,
                coord: HierarchyCoord {
                    levels: [
                        0x5133_060b,
                        1,
                        1,
                        2,
                        2,
                        qwen3_dense_runtime_tp_nodes() as u32,
                        qwen3_dense_runtime_total_layers() as u32,
                        len as u32,
                    ],
                },
            },
            object_ref_table_offset: 128,
            object_ref_count: 1,
        };
        let mut segment = vec![0; 128 + 256];
        segment[128..].copy_from_slice(reference.as_le_bytes());
        let operands = resolve_managed_operands(&req, &segment, vec![input]).unwrap();
        assert!(weak.upgrade().is_some());
        let contract = model_range_compute_contract(&req.task).unwrap().unwrap();
        assert_eq!(
            qwen3_dense_profile_range_input_checksum_with_operands(
                contract,
                &segment,
                Some(&operands)
            )
            .unwrap(),
            qwen3_dense_reference_range_object_payload_checksum(&bytes)
        );
        // No inline payload exists: DeepSeek must consume the same managed bytes.
        assert_eq!(
            deepseek_input(
                &segment,
                Some(&operands),
                QWEN3_DENSE_PROFILE_OBMM_KIND_HIDDEN_RANGE_RUNTIME_OUTPUT,
                len
            )
            .unwrap()
            .0,
            bytes
        );
        drop(operands);
        assert!(weak.upgrade().is_none());
        assert!(resolve_managed_operands(&req, &segment, vec![]).is_err());
        for offset in [16, 20, 24, 40, 72, 80, 192] {
            let mut altered = *reference.as_le_bytes();
            altered[offset] = altered[offset].wrapping_add(1);
            if offset == 40 {
                // Keep the different view structurally in bounds.
                altered[88..96].copy_from_slice(&(reference.allocation_bytes() + 1).to_le_bytes());
            }
            let changed = ManagedObjectRef::from_le_bytes(&altered).unwrap();
            let input = ManagedModelInput::from_provider_bytes(changed, bytes.clone()).unwrap();
            assert_eq!(
                resolve_managed_operands(&req, &segment, vec![input]).unwrap_err(),
                "managed_model_input_identity_mismatch",
                "offset={offset}"
            );
        }
        let input =
            || ManagedModelInput::from_provider_bytes(reference.clone(), bytes.clone()).unwrap();
        let mut duplicate_req = req.clone();
        duplicate_req.object_ref_count = 2;
        let mut duplicate_segment = segment.clone();
        duplicate_segment.extend_from_slice(reference.as_le_bytes());
        assert_eq!(
            resolve_managed_operands(&duplicate_req, &duplicate_segment, vec![input(), input()])
                .unwrap_err(),
            "managed_model_duplicate_input_role"
        );
        let mut v1_segment = segment.clone();
        v1_segment[136] = 1;
        assert!(resolve_managed_operands(&req, &v1_segment, vec![input()]).is_err());
        assert!(
            resolve_managed_operands(&req, &segment[..segment.len() - 1], vec![input()]).is_err()
        );

        let config = sim_config::ScenarioConfig::from_yaml_file(
            Path::new(env!("CARGO_MANIFEST_DIR"))
                .join("../../scenarios/mvp_8host_single_domain.yaml"),
        )
        .unwrap();
        let mut surface = LocalGuestUapiSurface::new(SimTopology::from_config(&config).unwrap());
        let handle = surface.create_segment(segment.len() as u64).unwrap();
        surface.write_segment_payload(handle, 0, &segment).unwrap();
        let mut dispatched = req.clone();
        dispatched.segment = handle;
        let held = input();
        let weak = Arc::downgrade(&held.bytes);
        // A missing input fails before backend invocation and never consults V1 caches.
        let event = surface
            .run_model_range_dispatch(dispatched.clone(), Some(vec![]))
            .unwrap();
        assert!(matches!(
            event.status,
            CompletionStatus::FatalFailure { .. }
        ));
        assert_eq!(event.op_id, req.op_id);
        // Force a terminal failure after successful resolution, with owned bytes released.
        std::env::set_var(
            "SIM_QWEN3_DENSE_WEIGHTS_PATH",
            "/nonexistent/managed-input-test/weights",
        );
        let event = surface
            .run_model_range_dispatch(dispatched, Some(vec![held]))
            .unwrap();
        assert!(matches!(
            event.status,
            CompletionStatus::FatalFailure { .. }
        ));
        assert!(weak.upgrade().is_none());
    }

    #[test]
    fn deepseek_decode_consumes_managed_token_and_kv_without_inline_payload() {
        let mut token = vec![0; 64];
        write_u64_le_at(&mut token, 8, 7);
        let kv = vec![0x5a; 64];
        let token_ref = reference(QWEN3_DENSE_PROFILE_OBMM_KIND_QWEN3_TOKEN_RESULT, &token);
        let kv_ref = reference(QWEN3_DENSE_PROFILE_OBMM_KIND_QWEN3_KV_STATE, &kv);
        let mut operands = ModelRangeDispatchOperands {
            managed: true,
            input_token: Some(
                ManagedModelInput::from_provider_bytes(token_ref, token)
                    .unwrap()
                    .into_operand(),
            ),
            previous_kv: Some(
                ManagedModelInput::from_provider_bytes(kv_ref.clone(), kv.clone())
                    .unwrap()
                    .into_operand(),
            ),
            ..Default::default()
        };
        let mut guest = vec![0; QWEN3_DENSE_PROFILE_PREVIOUS_KV_OFFSET + 32];
        let header = &mut guest[QWEN3_DENSE_PROFILE_PREVIOUS_KV_OFFSET..];
        write_u64_le_at(header, 0, QWEN3_DENSE_PROFILE_PREVIOUS_KV_MARKER);
        write_u64_le_at(header, 8, kv.len() as u64);
        write_u64_le_at(
            header,
            16,
            qwen3_dense_reference_range_object_payload_checksum(&kv),
        );
        let result =
            deepseek_v4_flash_decode_slice_inputs(100, 0, 1, &[42, 7], &guest, Some(&operands))
                .unwrap();
        assert_eq!(result, (vec![7], vec![42], Some(kv), 1));
        // Removing an input cannot revive the all-zero inline buffer.
        operands.input_token = None;
        assert!(deepseek_v4_flash_decode_slice_inputs(
            100,
            0,
            1,
            &[42, 7],
            &guest,
            Some(&operands)
        )
        .is_err());
    }
}

/// The managed path must never read the inline V1 materialization.
pub(super) fn deepseek_input<'a>(
    guest: &'a [u8],
    operands: Option<&'a ModelRangeDispatchOperands>,
    kind: u16,
    len: usize,
) -> Result<(&'a [u8], ModelInputMetadata), String> {
    if let Some(operands) = operands.filter(|value| value.managed) {
        let input = match kind {
            QWEN3_DENSE_PROFILE_OBMM_KIND_HIDDEN_RANGE_RUNTIME_OUTPUT => {
                operands.hidden_input.as_ref()
            }
            QWEN3_DENSE_PROFILE_OBMM_KIND_QWEN3_TOKEN_RESULT => operands.input_token.as_ref(),
            _ => None,
        }
        .ok_or_else(|| "managed_model_input_missing".to_string())?;
        if input.bytes().len() != len {
            return Err("managed_model_input_shape_mismatch".into());
        }
        return Ok((input.bytes(), input.metadata()));
    }
    let reference = deepseek_v4_flash_materialized_object_ref(guest, 0, kind)?;
    let end = QWEN3_DENSE_PROFILE_RANGE_INPUT_PAYLOAD_OFFSET
        .checked_add(len)
        .ok_or_else(|| "deepseek_v4_flash_input_size_overflow".to_string())?;
    let bytes = guest
        .get(QWEN3_DENSE_PROFILE_RANGE_INPUT_PAYLOAD_OFFSET..end)
        .ok_or_else(|| "deepseek_v4_flash_input_materialization_missing".to_string())?;
    Ok((
        bytes,
        ModelInputMetadata {
            object_kind: reference.object_kind,
            payload_bytes: reference.payload_bytes,
            payload_checksum: reference.payload_checksum,
        },
    ))
}
