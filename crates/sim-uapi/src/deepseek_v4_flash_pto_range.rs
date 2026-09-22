//! DeepSeek V4 Flash callable-5 tensor ABI.
//!
//! Shared hidden, KV and logits remain provider-backed UB_GM views. This
//! module validates the frozen ABI before the range graph is dispatched; it
//! does not materialize their payloads.

use super::*;
use sim_models::deepseek_v4_flash::{
    deepseek_v4_flash_layer_compress_ratio, DEEPSEEK_V4_FLASH_PROFILE,
};

pub(crate) const TENSOR_COUNT: usize = 6;
pub(crate) const SCALAR_COUNT: usize = 23;
const MAX_TOKENS: u64 = 1 << 20;

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct DeepseekV4FlashPtoRangeGeometry {
    pub first: u32,
    pub end: u32,
    pub layers: u32,
    pub past: u32,
    pub tokens: u32,
    pub hidden: u32,
    pub hc_mult: u32,
    pub vocab: u32,
    pub attention_heads: u32,
    pub kv_heads: u32,
    pub head_dim: u32,
    pub rope_head_dim: u32,
    pub q_lora_rank: u32,
    pub o_lora_rank: u32,
    pub output_groups: u32,
    pub experts: u32,
    pub experts_used: u32,
    pub shared_experts: u32,
    pub moe_intermediate: u32,
    pub sliding_window: u32,
    pub indexer_heads: u32,
    pub indexer_head_dim: u32,
    pub indexer_top_k: u32,
}

impl DeepseekV4FlashPtoRangeGeometry {
    pub fn profile(first: u32, end: u32, past: u32, tokens: u32) -> Self {
        let profile = DEEPSEEK_V4_FLASH_PROFILE;
        Self {
            first,
            end,
            layers: profile.num_hidden_layers as u32,
            past,
            tokens,
            hidden: profile.hidden_size as u32,
            hc_mult: profile.hc_mult as u32,
            vocab: profile.vocab_size as u32,
            attention_heads: profile.num_attention_heads as u32,
            kv_heads: profile.num_key_value_heads as u32,
            head_dim: profile.head_dim as u32,
            rope_head_dim: profile.qk_rope_head_dim as u32,
            q_lora_rank: profile.q_lora_rank as u32,
            o_lora_rank: profile.o_lora_rank as u32,
            output_groups: profile.output_groups as u32,
            experts: profile.num_experts as u32,
            experts_used: profile.num_experts_used as u32,
            shared_experts: profile.num_experts_shared as u32,
            moe_intermediate: profile.moe_intermediate_size as u32,
            sliding_window: profile.sliding_window as u32,
            indexer_heads: profile.indexer_heads as u32,
            indexer_head_dim: profile.indexer_head_dim as u32,
            indexer_top_k: profile.indexer_top_k as u32,
        }
    }

    pub fn scalars(&self) -> [u32; SCALAR_COUNT] {
        [
            self.first,
            self.end,
            self.layers,
            self.past,
            self.tokens,
            self.hidden,
            self.hc_mult,
            self.vocab,
            self.attention_heads,
            self.kv_heads,
            self.head_dim,
            self.rope_head_dim,
            self.q_lora_rank,
            self.o_lora_rank,
            self.output_groups,
            self.experts,
            self.experts_used,
            self.shared_experts,
            self.moe_intermediate,
            self.sliding_window,
            self.indexer_heads,
            self.indexer_head_dim,
            self.indexer_top_k,
        ]
    }

    pub fn validate(&self) -> Result<(), String> {
        let expected = Self::profile(self.first, self.end, self.past, self.tokens);
        if self.first >= self.end
            || self.end > self.layers
            || self.tokens == 0
            || u64::from(self.past) + u64::from(self.tokens) > MAX_TOKENS
            || self.layers != expected.layers
            || self.hidden != expected.hidden
            || self.hc_mult != expected.hc_mult
            || self.vocab != expected.vocab
            || self.attention_heads != expected.attention_heads
            || self.kv_heads != expected.kv_heads
            || self.head_dim != expected.head_dim
            || self.rope_head_dim != expected.rope_head_dim
            || self.q_lora_rank != expected.q_lora_rank
            || self.o_lora_rank != expected.o_lora_rank
            || self.output_groups != expected.output_groups
            || self.experts != expected.experts
            || self.experts_used != expected.experts_used
            || self.shared_experts != expected.shared_experts
            || self.moe_intermediate != expected.moe_intermediate
            || self.sliding_window != expected.sliding_window
            || self.indexer_heads != expected.indexer_heads
            || self.indexer_head_dim != expected.indexer_head_dim
            || self.indexer_top_k != expected.indexer_top_k
        {
            return Err("deepseek_v4_flash_pto_range_geometry".into());
        }
        Ok(())
    }

    fn compressor_bytes(&self, head_dim: u32, ratio: u32) -> Result<u64, String> {
        let lanes = match ratio {
            4 => 2u64,
            128 => 1u64,
            _ => return Err("deepseek_v4_flash_pto_compressor_ratio".into()),
        };
        let width = lanes
            .checked_mul(u64::from(head_dim))
            .ok_or("deepseek_v4_flash_pto_state_overflow")?;
        let rows = u64::from(ratio)
            .checked_mul(lanes)
            .ok_or("deepseek_v4_flash_pto_state_overflow")?;
        let elements = rows
            .checked_mul(width)
            .ok_or("deepseek_v4_flash_pto_state_overflow")?;
        32u64
            .checked_add(
                elements
                    .checked_mul(8)
                    .ok_or("deepseek_v4_flash_pto_state_overflow")?,
            )
            .ok_or_else(|| "deepseek_v4_flash_pto_state_overflow".into())
    }

    fn vector_bytes(elements: u64) -> Result<u64, String> {
        8u64.checked_add(
            elements
                .checked_mul(4)
                .ok_or("deepseek_v4_flash_pto_state_overflow")?,
        )
        .ok_or_else(|| "deepseek_v4_flash_pto_state_overflow".into())
    }

    pub fn range_state_bytes(&self, positions: u32) -> Result<u64, String> {
        self.validate()?;
        if positions == 0 {
            return Err("deepseek_v4_flash_pto_state_positions".into());
        }
        let raw_rows = u64::from(positions.min(self.sliding_window));
        let raw_elements = raw_rows
            .checked_mul(u64::from(self.head_dim))
            .ok_or("deepseek_v4_flash_pto_state_overflow")?;
        let mut bytes = 32u64;
        for layer in self.first..self.end {
            let ratio = deepseek_v4_flash_layer_compress_ratio(u64::from(layer))
                .ok_or("deepseek_v4_flash_pto_state_layer")?;
            bytes = bytes
                .checked_add(8)
                .and_then(|value| value.checked_add(Self::vector_bytes(raw_elements).ok()?))
                .ok_or("deepseek_v4_flash_pto_state_overflow")?;
            if ratio == 0 {
                continue;
            }
            let compressed_rows =
                (u64::from(positions) / u64::from(ratio)).min(u64::from(self.indexer_top_k));
            let compressed_elements = compressed_rows
                .checked_mul(u64::from(self.head_dim))
                .ok_or("deepseek_v4_flash_pto_state_overflow")?;
            bytes = bytes
                .checked_add(Self::vector_bytes(compressed_elements)?)
                .ok_or("deepseek_v4_flash_pto_state_overflow")?;
            if ratio == 4 {
                let indexer_elements = compressed_rows
                    .checked_mul(u64::from(self.indexer_head_dim))
                    .ok_or("deepseek_v4_flash_pto_state_overflow")?;
                bytes = bytes
                    .checked_add(Self::vector_bytes(indexer_elements)?)
                    .ok_or("deepseek_v4_flash_pto_state_overflow")?;
            }
            bytes = bytes
                .checked_add(self.compressor_bytes(self.head_dim, ratio)?)
                .ok_or("deepseek_v4_flash_pto_state_overflow")?;
            if ratio == 4 {
                bytes = bytes
                    .checked_add(self.compressor_bytes(self.indexer_head_dim, ratio)?)
                    .ok_or("deepseek_v4_flash_pto_state_overflow")?;
            }
        }
        Ok(bytes)
    }

    fn shapes(&self) -> Result<[(Vec<u32>, u16, BufferUsage); TENSOR_COUNT], String> {
        self.validate()?;
        let hidden_width = self
            .hidden
            .checked_mul(self.hc_mult)
            .ok_or("deepseek_v4_flash_pto_hidden_overflow")?;
        let previous = if self.past == 0 {
            1
        } else {
            u32::try_from(self.range_state_bytes(self.past)?)
                .map_err(|_| "deepseek_v4_flash_pto_state_overflow")?
        };
        let next = u32::try_from(self.range_state_bytes(self.past + self.tokens)?)
            .map_err(|_| "deepseek_v4_flash_pto_state_overflow")?;
        Ok([
            (
                vec![
                    if self.first == 0 { 1 } else { self.tokens },
                    if self.first == 0 { 1 } else { hidden_width },
                ],
                0,
                BufferUsage::Input,
            ),
            (vec![previous], 5, BufferUsage::Input),
            (
                vec![if self.first == 0 { self.tokens } else { 1 }],
                2,
                BufferUsage::Input,
            ),
            (vec![self.tokens, hidden_width], 0, BufferUsage::Output),
            (vec![next], 5, BufferUsage::Output),
            (
                vec![if self.end == self.layers {
                    self.vocab
                } else {
                    1
                }],
                0,
                if self.end == self.layers {
                    BufferUsage::Output
                } else {
                    BufferUsage::Input
                },
            ),
        ])
    }
}

pub(crate) fn validate(
    req: &PtoUbGmDispatchV2Req,
) -> Result<DeepseekV4FlashPtoRangeGeometry, String> {
    if req.callable_id != PTO_UB_GM_DEEPSEEK_V4_FLASH_RANGE_CALLABLE_ID
        || req.request_id == 0
        || req.args.len() != TENSOR_COUNT + SCALAR_COUNT
    {
        return Err("pto_ub_gm_bad_control_table".into());
    }
    let mut scalars = [0u32; SCALAR_COUNT];
    for (index, arg) in req.args[TENSOR_COUNT..].iter().enumerate() {
        let SimplerRuntimeArg::ScalarU64(value) = arg else {
            return Err("pto_ub_gm_bad_control_table".into());
        };
        scalars[index] = u32::try_from(*value).map_err(|_| "pto_ub_gm_bad_control_table")?;
    }
    let geometry = DeepseekV4FlashPtoRangeGeometry {
        first: scalars[0],
        end: scalars[1],
        layers: scalars[2],
        past: scalars[3],
        tokens: scalars[4],
        hidden: scalars[5],
        hc_mult: scalars[6],
        vocab: scalars[7],
        attention_heads: scalars[8],
        kv_heads: scalars[9],
        head_dim: scalars[10],
        rope_head_dim: scalars[11],
        q_lora_rank: scalars[12],
        o_lora_rank: scalars[13],
        output_groups: scalars[14],
        experts: scalars[15],
        experts_used: scalars[16],
        shared_experts: scalars[17],
        moe_intermediate: scalars[18],
        sliding_window: scalars[19],
        indexer_heads: scalars[20],
        indexer_head_dim: scalars[21],
        indexer_top_k: scalars[22],
    };
    let shapes = geometry.shapes()?;
    for (index, (arg, (shape, dtype, usage))) in
        req.args[..TENSOR_COUNT].iter().zip(shapes).enumerate()
    {
        let SimplerRuntimeArg::UbGmMemref {
            binding,
            view,
            usage: actual_usage,
        } = arg
        else {
            return Err("pto_ub_gm_bad_memref".into());
        };
        let width = match dtype {
            0 | 2 => 4,
            5 => 1,
            _ => unreachable!(),
        };
        let elements = shape.iter().try_fold(1u64, |product, value| {
            product.checked_mul(u64::from(*value))
        });
        let strides = match shape.as_slice() {
            [length] => vec![u32::from(*length != 0)],
            [rows, columns] if *rows != 0 && *columns != 0 => vec![*columns, 1],
            _ => return Err("deepseek_v4_flash_pto_range_memref_contract".into()),
        };
        let expected_access = match usage {
            BufferUsage::Input => sim_core::SimplerUbGmAccess::Read,
            BufferUsage::Output => sim_core::SimplerUbGmAccess::Write,
            BufferUsage::Inout => sim_core::SimplerUbGmAccess::ReadWrite,
            _ => return Err("deepseek_v4_flash_pto_range_memref_contract".into()),
        };
        if binding.request_id != req.request_id
            || binding.binding_id != index as u64 + 1
            || binding.backend_cookie == 0
            || binding.flags != 0
            || binding.access != expected_access
            || view.aperture_offset > binding.aperture_length
            || view.byte_length > binding.aperture_length - view.aperture_offset
            || *actual_usage != usage
            || view.dtype != dtype
            || view.shape != shape
            || view.strides != strides
            || elements.and_then(|value| value.checked_mul(width)) != Some(view.byte_length)
        {
            return Err("deepseek_v4_flash_pto_range_memref_contract".into());
        }
    }
    Ok(geometry)
}

#[cfg(test)]
mod tests {
    use super::*;
    use sim_core::{SimplerUbGmAccess, SimplerUbGmBinding, SimplerUbGmView};

    fn request(geometry: &DeepseekV4FlashPtoRangeGeometry) -> PtoUbGmDispatchV2Req {
        let tensors = geometry.shapes().unwrap();
        let mut args = tensors
            .into_iter()
            .enumerate()
            .map(|(index, (shape, dtype, usage))| {
                let width = if dtype == 5 { 1 } else { 4 };
                let bytes = shape.iter().map(|value| u64::from(*value)).product::<u64>() * width;
                SimplerRuntimeArg::UbGmMemref {
                    binding: SimplerUbGmBinding {
                        request_id: 91,
                        binding_id: index as u64 + 1,
                        aperture_base: 0x7000_0000_0000 + index as u64 * 0x1000_0000,
                        aperture_length: bytes,
                        ub_gm_base: 0x8000_0000_0000 + index as u64 * 0x1000_0000,
                        mapped_length: bytes,
                        access: match usage {
                            BufferUsage::Input => SimplerUbGmAccess::Read,
                            BufferUsage::Output => SimplerUbGmAccess::Write,
                            BufferUsage::Inout => SimplerUbGmAccess::ReadWrite,
                            _ => unreachable!("direct model tensors use I/O roles"),
                        },
                        flags: 0,
                        backend_cookie: 1,
                    },
                    view: SimplerUbGmView {
                        aperture_offset: 0,
                        byte_length: bytes,
                        shape: shape.clone(),
                        strides: if shape.len() == 1 {
                            vec![1]
                        } else {
                            vec![shape[1], 1]
                        },
                        dtype,
                    },
                    usage,
                }
            })
            .collect::<Vec<_>>();
        args.extend(
            geometry
                .scalars()
                .into_iter()
                .map(|value| SimplerRuntimeArg::ScalarU64(u64::from(value))),
        );
        PtoUbGmDispatchV2Req {
            op_id: 90,
            request_id: 91,
            callable_id: PTO_UB_GM_DEEPSEEK_V4_FLASH_RANGE_CALLABLE_ID,
            artifact_fingerprint: 92,
            requester_cna: 7,
            args,
        }
    }

    #[test]
    fn validates_first_middle_and_terminal_ranges() {
        for geometry in [
            DeepseekV4FlashPtoRangeGeometry::profile(0, 15, 0, 3),
            DeepseekV4FlashPtoRangeGeometry::profile(15, 29, 3, 1),
            DeepseekV4FlashPtoRangeGeometry::profile(29, 43, 4, 1),
        ] {
            assert_eq!(validate(&request(&geometry)).unwrap(), geometry);
        }
    }

    #[test]
    fn rejects_shape_dtype_usage_and_geometry_drift() {
        let geometry = DeepseekV4FlashPtoRangeGeometry::profile(0, 15, 0, 3);
        for fault in 0..5 {
            let mut request = request(&geometry);
            match (&mut request.args[4], fault) {
                (SimplerRuntimeArg::UbGmMemref { view, .. }, 0) => view.byte_length += 1,
                (SimplerRuntimeArg::UbGmMemref { view, .. }, 1) => view.dtype = 0,
                (SimplerRuntimeArg::UbGmMemref { usage, .. }, 2) => *usage = BufferUsage::Input,
                (_, 3) => request.args[TENSOR_COUNT + 5] = SimplerRuntimeArg::ScalarU64(4095),
                (SimplerRuntimeArg::UbGmMemref { binding, .. }, 4) => {
                    binding.access = SimplerUbGmAccess::Read
                }
                _ => unreachable!(),
            }
            assert!(validate(&request).is_err(), "fault {fault}");
        }
    }

    #[test]
    fn state_size_is_monotonic_and_topology_specific() {
        let dense = DeepseekV4FlashPtoRangeGeometry::profile(0, 2, 0, 3);
        let ratio4 = DeepseekV4FlashPtoRangeGeometry::profile(2, 3, 0, 1);
        let ratio128 = DeepseekV4FlashPtoRangeGeometry::profile(3, 4, 0, 1);
        let mixed = DeepseekV4FlashPtoRangeGeometry::profile(2, 4, 3, 1);
        let terminal = DeepseekV4FlashPtoRangeGeometry::profile(41, 43, 4, 1);
        assert_eq!(dense.range_state_bytes(3).unwrap(), 12_352);
        assert_eq!(mixed.range_state_bytes(3).unwrap(), 618_680);
        assert_eq!(mixed.range_state_bytes(4).unwrap(), 625_336);
        assert_eq!(terminal.range_state_bytes(5).unwrap(), 629_432);
        assert!(ratio4.range_state_bytes(4).unwrap() > ratio4.range_state_bytes(3).unwrap());
        assert!(
            ratio128.range_state_bytes(128).unwrap() > ratio128.range_state_bytes(127).unwrap()
        );
        assert_ne!(
            ratio4.range_state_bytes(128).unwrap(),
            ratio128.range_state_bytes(128).unwrap()
        );
    }
}
