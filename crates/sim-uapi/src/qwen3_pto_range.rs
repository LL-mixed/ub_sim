//! Model-range contract and private weight initialization. Guest hidden/KV
//! stay in UB_GM; this module must never materialize those payloads.
use super::*;

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Qwen3PtoRangeGeometry {
    pub first: u32,
    pub end: u32,
    pub layers: u32,
    pub past: u32,
    pub tokens: u32,
    pub hidden: u32,
    pub intermediate: u32,
    pub query_heads: u32,
    pub kv_heads: u32,
    pub head_dim: u32,
    pub vocab: u32,
    pub scale_bits: u32,
}

impl Qwen3PtoRangeGeometry {
    pub fn scalars(&self) -> [u32; 12] {
        [
            self.first,
            self.end,
            self.layers,
            self.past,
            self.tokens,
            self.hidden,
            self.intermediate,
            self.query_heads,
            self.kv_heads,
            self.head_dim,
            self.vocab,
            self.scale_bits,
        ]
    }

    pub fn validate(&self) -> Result<(), String> {
        let scale = f32::from_bits(self.scale_bits);
        if self.first >= self.end
            || self.end > self.layers
            || self.layers > 128
            || self.tokens == 0
            || u64::from(self.tokens) + u64::from(self.past) > 4096
            || self.hidden == 0
            || self.hidden > 4096
            || self.intermediate == 0
            || self.intermediate > 4096
            || self.kv_heads == 0
            || self.query_heads == 0
            || self.query_heads % self.kv_heads != 0
            || self.head_dim == 0
            || self.head_dim % 2 != 0
            || u64::from(self.query_heads) * u64::from(self.head_dim) > 4096
            || self.vocab == 0
            || self.vocab > (1 << 24)
            || !scale.is_finite()
            || (scale - 1.0 / (self.head_dim as f32).sqrt()).abs() > 1e-7
        {
            return Err("qwen3_pto_range_geometry".into());
        }
        Ok(())
    }

    pub fn kv_words(&self, tokens: u32) -> Result<u32, String> {
        let words = u64::from(self.end - self.first)
            * (10 + 2 * u64::from(tokens) * u64::from(self.kv_heads) * u64::from(self.head_dim));
        u32::try_from(words).map_err(|_| "qwen3_pto_range_kv_overflow".into())
    }

    pub fn guest_shapes(&self) -> Result<[[u32; 2]; 6], String> {
        self.validate()?;
        Ok([
            if self.first == 0 {
                [1, 1]
            } else {
                [self.tokens, self.hidden]
            },
            [
                1,
                if self.past == 0 {
                    1
                } else {
                    self.kv_words(self.past)?
                },
            ],
            [1, if self.first == 0 { self.tokens } else { 1 }],
            [self.tokens, self.hidden],
            [1, self.kv_words(self.past + self.tokens)?],
            [
                1,
                if self.end == self.layers {
                    self.vocab
                } else {
                    1
                },
            ],
        ])
    }
}

pub(crate) fn validate(req: &PtoUbGmDispatchV2Req) -> Result<Qwen3PtoRangeGeometry, String> {
    if req.request_id == 0 || req.args.len() != 18 {
        return Err("pto_ub_gm_bad_control_table".into());
    }
    let mut s = [0u32; 12];
    for (index, arg) in req.args[6..].iter().enumerate() {
        let SimplerRuntimeArg::ScalarU64(value) = arg else {
            return Err("pto_ub_gm_bad_control_table".into());
        };
        s[index] = u32::try_from(*value).map_err(|_| "pto_ub_gm_bad_control_table")?;
    }
    let g = Qwen3PtoRangeGeometry {
        first: s[0],
        end: s[1],
        layers: s[2],
        past: s[3],
        tokens: s[4],
        hidden: s[5],
        intermediate: s[6],
        query_heads: s[7],
        kv_heads: s[8],
        head_dim: s[9],
        vocab: s[10],
        scale_bits: s[11],
    };
    let shapes = g.guest_shapes()?;
    for (index, arg) in req.args[..6].iter().enumerate() {
        let SimplerRuntimeArg::UbGmMemref {
            binding,
            view,
            usage,
        } = arg
        else {
            return Err("pto_ub_gm_bad_memref".into());
        };
        let expected_usage = match index {
            3 => BufferUsage::Output,
            5 if g.end == g.layers => BufferUsage::Output,
            4 => BufferUsage::Inout,
            _ => BufferUsage::Input,
        };
        let dtype = if index == 3 || (index == 0 && g.first > 0) {
            1
        } else {
            0
        };
        if binding.request_id != req.request_id
            || *usage != expected_usage
            || view.dtype != dtype
            || view.shape != shapes[index]
            || view.strides != [shapes[index][1], 1]
            || view.byte_length
                != u64::from(shapes[index][0])
                    * u64::from(shapes[index][1])
                    * if dtype == 1 { 2 } else { 4 }
        {
            return Err("qwen3_pto_range_memref_contract".into());
        }
    }
    Ok(g)
}

fn append_weight(
    destination: &mut Vec<u8>,
    loaded: &qwen3_dense_reference::Qwen3DenseReferenceLoadedWeights,
    name: &str,
    rows: u32,
    cols: u32,
) -> Result<(), String> {
    use qwen3_dense_reference::Qwen3DenseReferenceWeightDType as D;
    let tensor = loaded
        .tensors
        .get(name)
        .ok_or_else(|| format!("qwen3_pto_missing_weight:{name}"))?;
    if tensor.shape != [u64::from(rows), u64::from(cols)]
        && !(rows == 1 && tensor.shape == [u64::from(cols)])
    {
        return Err(format!("qwen3_pto_weight_geometry:{name}"));
    }
    let payload = materialize_full_weight_tensor_payload(name, &loaded.tensors)?;
    let width = match tensor.dtype {
        D::F32 => 4,
        D::F16 | D::BF16 => 2,
        _ => return Err(format!("qwen3_pto_weight_dtype:{name}")),
    };
    if payload.len() as u64 != u64::from(rows) * u64::from(cols) * width {
        return Err(format!("qwen3_pto_weight_bytes:{name}"));
    }
    for word in payload.chunks_exact(width as usize) {
        let value = match tensor.dtype {
            D::F32 => f32::from_le_bytes(word.try_into().unwrap()),
            D::F16 => f16_bits_to_f32(u16::from_le_bytes(word.try_into().unwrap())),
            D::BF16 => {
                f32::from_bits(u32::from(u16::from_le_bytes(word.try_into().unwrap())) << 16)
            }
            _ => unreachable!(),
        };
        if !value.is_finite() {
            return Err(format!("qwen3_pto_nonfinite_weight:{name}"));
        }
        destination.extend_from_slice(&value.to_le_bytes());
    }
    Ok(())
}

pub(crate) fn private_constants(g: &Qwen3PtoRangeGeometry) -> Result<Vec<u8>, String> {
    g.validate()?;
    let path = std::env::var("SIM_QWEN3_DENSE_WEIGHTS_PATH")
        .map_err(|_| "qwen3_pto_missing_weights_path")?;
    let profile = qwen3_dense::profile_from_weights_dir(Path::new(&path), None, 2, 1, 1)?;
    if [
        profile.num_hidden_layers,
        profile.hidden_size,
        profile.intermediate_size,
        profile.num_attention_heads,
        profile.num_key_value_heads,
        profile.head_dim,
        profile.vocab_size,
        profile.rope_theta,
    ] != [
        u64::from(g.layers),
        u64::from(g.hidden),
        u64::from(g.intermediate),
        u64::from(g.query_heads),
        u64::from(g.kv_heads),
        u64::from(g.head_dim),
        u64::from(g.vocab),
        1_000_000,
    ] {
        return Err("qwen3_pto_model_profile_mismatch".into());
    }
    let loaded = qwen3_dense_reference_cached_loaded_weights(&path)?;
    let mut out = Vec::new();
    if g.first == 0 {
        append_weight(
            &mut out,
            &loaded,
            "model.embed_tokens.weight",
            g.vocab,
            g.hidden,
        )?;
    }
    if g.end == g.layers {
        append_weight(&mut out, &loaded, "model.norm.weight", 1, g.hidden)?;
        // Match the model's declared head, including checkpoints that store
        // an explicit head despite tie_word_embeddings=true in config.json.
        let head = if loaded.tensors.contains_key("lm_head.weight") {
            "lm_head.weight"
        } else {
            "model.embed_tokens.weight"
        };
        append_weight(&mut out, &loaded, head, g.vocab, g.hidden)?;
    }
    let qw = g.query_heads * g.head_dim;
    let kw = g.kv_heads * g.head_dim;
    for layer in g.first..g.end {
        for (suffix, rows, cols) in [
            ("input_layernorm.weight", 1, g.hidden),
            ("self_attn.q_proj.weight", qw, g.hidden),
            ("self_attn.k_proj.weight", kw, g.hidden),
            ("self_attn.v_proj.weight", kw, g.hidden),
            ("self_attn.q_norm.weight", 1, g.head_dim),
            ("self_attn.k_norm.weight", 1, g.head_dim),
            ("self_attn.o_proj.weight", g.hidden, qw),
            ("post_attention_layernorm.weight", 1, g.hidden),
            ("mlp.gate_proj.weight", g.intermediate, g.hidden),
            ("mlp.up_proj.weight", g.intermediate, g.hidden),
            ("mlp.down_proj.weight", g.hidden, g.intermediate),
        ] {
            append_weight(
                &mut out,
                &loaded,
                &format!("model.layers.{layer}.{suffix}"),
                rows,
                cols,
            )?;
        }
    }
    for position in g.past..g.past + g.tokens {
        for sine in [false, true] {
            for col in 0..g.head_dim / 2 {
                let value = rope_factor(position, col, g.head_dim, sine);
                out.extend_from_slice(&value.to_le_bytes());
            }
        }
    }
    if out.len() / 4 > u32::MAX as usize {
        return Err("qwen3_pto_constants_too_large".into());
    }
    Ok(out)
}

// Preserve the reference's FP32 angle and sin_cos rounding sequence.
fn rope_factor(position: u32, col: u32, head_dim: u32, sine: bool) -> f32 {
    let angle = position as f32 / 1_000_000f32.powf((col * 2) as f32 / head_dim as f32);
    let (sin, cos) = angle.sin_cos();
    if sine {
        sin
    } else {
        cos
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn rope_constants_keep_reference_fp32_rounding() {
        let mut differs_from_double = 0;
        for position in 0..10 {
            for col in 0..64 {
                let angle = position as f32 / 1_000_000f32.powf((col * 2) as f32 / 128.0);
                let (sin, cos) = angle.sin_cos();
                for (sine, expected) in [(false, cos), (true, sin)] {
                    let actual = rope_factor(position, col, 128, sine);
                    assert_eq!(actual.to_bits(), expected.to_bits());
                    let double_angle =
                        position as f64 / 1_000_000f64.powf((col * 2) as f64 / 128.0);
                    let old = if sine {
                        double_angle.sin()
                    } else {
                        double_angle.cos()
                    } as f32;
                    differs_from_double += usize::from(actual.to_bits() != old.to_bits());
                }
            }
        }
        assert!(
            differs_from_double > 0,
            "fixture must distinguish the old path"
        );
    }
    use sim_core::{SimplerUbGmAccess, SimplerUbGmBinding, SimplerUbGmView};

    fn request(first: u32, end: u32, past: u32) -> PtoUbGmDispatchV2Req {
        let g = Qwen3PtoRangeGeometry {
            first,
            end,
            layers: 2,
            past,
            tokens: 2,
            hidden: 16,
            intermediate: 24,
            query_heads: 4,
            kv_heads: 2,
            head_dim: 8,
            vocab: 67,
            scale_bits: (1f32 / 8f32.sqrt()).to_bits(),
        };
        let mut args = Vec::new();
        for (i, shape) in g.guest_shapes().unwrap().iter().enumerate() {
            let dtype = if i == 3 || (i == 0 && first > 0) {
                1
            } else {
                0
            };
            let bytes = u64::from(shape[0]) * u64::from(shape[1]) * if dtype == 1 { 2 } else { 4 };
            let (usage, access) = match i {
                3 => (BufferUsage::Output, SimplerUbGmAccess::Write),
                5 if g.end == g.layers => (BufferUsage::Output, SimplerUbGmAccess::Write),
                4 => (BufferUsage::Inout, SimplerUbGmAccess::ReadWrite),
                _ => (BufferUsage::Input, SimplerUbGmAccess::Read),
            };
            args.push(SimplerRuntimeArg::UbGmMemref {
                binding: SimplerUbGmBinding {
                    request_id: 17,
                    binding_id: i as u64 + 1,
                    aperture_base: 0x7000_0000_0000 + i as u64 * 0x100000,
                    aperture_length: bytes,
                    ub_gm_base: 0x100000 + i as u64 * 0x100000,
                    mapped_length: bytes,
                    access,
                    flags: 0,
                    backend_cookie: 0,
                },
                view: SimplerUbGmView {
                    aperture_offset: 0,
                    byte_length: bytes,
                    dtype,
                    shape: shape.to_vec(),
                    strides: vec![shape[1], 1],
                },
                usage,
            });
        }
        args.extend(g.scalars().map(|v| SimplerRuntimeArg::ScalarU64(v.into())));
        PtoUbGmDispatchV2Req {
            op_id: 1,
            request_id: 17,
            callable_id: 3,
            artifact_fingerprint: 1,
            requester_cna: 0x10001,
            args,
        }
    }

    #[test]
    fn range_contract_covers_first_terminal_prefill_and_decode() {
        for (first, end, past) in [(0, 1, 0), (1, 2, 0), (0, 2, 3), (1, 2, 7)] {
            let req = request(first, end, past);
            assert!(validate(&req).is_ok());
            let mut bad_logits = req.clone();
            if let SimplerRuntimeArg::UbGmMemref { usage, .. } = &mut bad_logits.args[5] {
                *usage = if end == 2 {
                    BufferUsage::Input
                } else {
                    BufferUsage::Output
                };
            }
            assert!(
                validate(&bad_logits).is_err(),
                "only terminal ranges produce logits"
            );
            let mut bad = req.clone();
            if let SimplerRuntimeArg::UbGmMemref { usage, .. } = &mut bad.args[4] {
                *usage = BufferUsage::Output;
            }
            assert!(validate(&bad).is_err(), "next KV needs read/write");
            let mut bad = req.clone();
            if let SimplerRuntimeArg::UbGmMemref { view, .. } = &mut bad.args[3] {
                view.dtype = 0;
            }
            assert!(validate(&bad).is_err(), "hidden handoff must preserve F16");
            let mut bad = req.clone();
            bad.args[17] = SimplerRuntimeArg::ScalarU64(f32::NAN.to_bits().into());
            assert!(validate(&bad).is_err());
            let mut bad = req.clone();
            bad.args[9] = SimplerRuntimeArg::ScalarU64(u64::MAX);
            assert!(validate(&bad).is_err());
        }
    }
}
