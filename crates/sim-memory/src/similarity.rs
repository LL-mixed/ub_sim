use crate::{HotTensorObjectRef, TensorDType};
use thiserror::Error;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum HiddenDecodePolicy {
    F32,
    F16,
    BF16,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum MatchMetric {
    Cosine,
    NormalizedL2,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum MatchMode {
    Exact,
    Approximate,
    ExactThenApproximate,
}

impl MatchMode {
    pub fn from_str(s: &str) -> Option<Self> {
        match s {
            "exact" => Some(MatchMode::Exact),
            "approximate" => Some(MatchMode::Approximate),
            "exact-then-approximate" => Some(MatchMode::ExactThenApproximate),
            _ => None,
        }
    }

    pub fn is_exact_only(self) -> bool {
        matches!(self, MatchMode::Exact)
    }

    pub fn allows_approximate(self) -> bool {
        !matches!(self, MatchMode::Exact)
    }

    pub fn preferred_match(self) -> &'static str {
        match self {
            MatchMode::Exact => "exact",
            MatchMode::Approximate => "approximate",
            MatchMode::ExactThenApproximate => "exact-then-approximate",
        }
    }
}

impl HiddenDecodePolicy {
    pub fn from_dtype(dtype: TensorDType) -> Option<Self> {
        match dtype {
            TensorDType::F32 => Some(Self::F32),
            _ => None,
        }
    }
}

#[derive(Debug, Error)]
pub enum SimilarityError {
    #[error("dtype mismatch: query={query:?} candidate={candidate:?}")]
    DTypeMismatch {
        query: TensorDType,
        candidate: TensorDType,
    },
    #[error("shape mismatch: query={query:?} candidate={candidate:?}")]
    ShapeMismatch {
        query: Vec<u64>,
        candidate: Vec<u64>,
    },
    #[error("payload size mismatch: expected={expected} got={got}")]
    PayloadSizeMismatch { expected: u64, got: usize },
    #[error("zero norm: vector has zero magnitude")]
    ZeroNorm,
    #[error("NaN value detected")]
    NaN,
    #[error("unsupported decode policy for dtype {0:?}")]
    UnsupportedDecodePolicy(TensorDType),
    #[error("missing payload")]
    MissingPayload,
}

pub type SimilarityResult<T> = Result<T, SimilarityError>;

fn f16_bits_to_f32(bits: u16) -> f32 {
    let sign = ((bits & 0x8000) as u32) << 16;
    let exponent = (bits >> 10) & 0x1f;
    let fraction = bits & 0x03ff;
    let f32_bits: u32 = if exponent == 0 {
        if fraction == 0 {
            sign
        } else {
            // Subnormal: convert to normal by shifting and adjusting exponent
            let shift = fraction.leading_zeros() - 6; // 6 = leading_zeros of 0b1_0000_0000_00 (0x400)
            let adjusted_frac = (fraction << shift) & 0x03ff;
            let adjusted_exp = 1 - shift as u32;
            sign | (((adjusted_exp + 127 - 15) << 23) | ((adjusted_frac as u32) << 13))
        }
    } else if exponent == 0x1f {
        sign | 0x7f800000 | ((fraction as u32) << 13)
    } else {
        sign | (((exponent as u32) + 127 - 15) << 23) | ((fraction as u32) << 13)
    };
    f32::from_bits(f32_bits)
}

fn bf16_bits_to_f32(bits: u16) -> f32 {
    f32::from_bits((bits as u32) << 16)
}

pub fn decode_payload_to_f32(
    payload: &[u8],
    dtype: TensorDType,
    shape: &[u64],
    policy: HiddenDecodePolicy,
) -> SimilarityResult<Vec<f32>> {
    let expected_elems: usize = shape.iter().product::<u64>() as usize;
    let decoded = match dtype {
        TensorDType::F32 => {
            if payload.len() != expected_elems * 4 {
                return Err(SimilarityError::PayloadSizeMismatch {
                    expected: expected_elems as u64 * 4,
                    got: payload.len(),
                });
            }
            payload
                .chunks_exact(4)
                .map(|chunk| f32::from_le_bytes(chunk.try_into().expect("f32 bytes")))
                .collect()
        }
        TensorDType::Opaque => match policy {
            HiddenDecodePolicy::F16 => {
                if payload.len() != expected_elems * 2 {
                    return Err(SimilarityError::PayloadSizeMismatch {
                        expected: expected_elems as u64 * 2,
                        got: payload.len(),
                    });
                }
                payload
                    .chunks_exact(2)
                    .map(|chunk| {
                        f16_bits_to_f32(u16::from_le_bytes(chunk.try_into().expect("f16 bytes")))
                    })
                    .collect()
            }
            HiddenDecodePolicy::BF16 => {
                if payload.len() != expected_elems * 2 {
                    return Err(SimilarityError::PayloadSizeMismatch {
                        expected: expected_elems as u64 * 2,
                        got: payload.len(),
                    });
                }
                payload
                    .chunks_exact(2)
                    .map(|chunk| {
                        bf16_bits_to_f32(u16::from_le_bytes(chunk.try_into().expect("bf16 bytes")))
                    })
                    .collect()
            }
            _ => return Err(SimilarityError::UnsupportedDecodePolicy(dtype)),
        },
        _ => return Err(SimilarityError::UnsupportedDecodePolicy(dtype)),
    };
    Ok(decoded)
}

pub fn compute_cosine_similarity(query: &[f32], candidate: &[f32]) -> SimilarityResult<f32> {
    if query.len() != candidate.len() {
        return Err(SimilarityError::ShapeMismatch {
            query: vec![query.len() as u64],
            candidate: vec![candidate.len() as u64],
        });
    }
    let mut dot = 0.0f64;
    let mut query_norm_sq = 0.0f64;
    let mut candidate_norm_sq = 0.0f64;
    for (&q, &c) in query.iter().zip(candidate.iter()) {
        if q.is_nan() || c.is_nan() {
            return Err(SimilarityError::NaN);
        }
        let qd = q as f64;
        let cd = c as f64;
        dot += qd * cd;
        query_norm_sq += qd * qd;
        candidate_norm_sq += cd * cd;
    }
    let query_norm = query_norm_sq.sqrt();
    let candidate_norm = candidate_norm_sq.sqrt();
    if query_norm == 0.0 || candidate_norm == 0.0 {
        return Err(SimilarityError::ZeroNorm);
    }
    let cosine = (dot / (query_norm * candidate_norm)) as f32;
    Ok(cosine.clamp(-1.0, 1.0))
}

pub fn compute_normalized_l2(query: &[f32], candidate: &[f32]) -> SimilarityResult<f32> {
    if query.len() != candidate.len() {
        return Err(SimilarityError::ShapeMismatch {
            query: vec![query.len() as u64],
            candidate: vec![candidate.len() as u64],
        });
    }
    let mut l2_sq = 0.0f64;
    let mut query_norm_sq = 0.0f64;
    for (&q, &c) in query.iter().zip(candidate.iter()) {
        if q.is_nan() || c.is_nan() {
            return Err(SimilarityError::NaN);
        }
        let diff = (q as f64) - (c as f64);
        l2_sq += diff * diff;
        query_norm_sq += (q as f64) * (q as f64);
    }
    let query_norm = query_norm_sq.sqrt();
    if query_norm == 0.0 {
        return Err(SimilarityError::ZeroNorm);
    }
    let normalized_l2 = (l2_sq.sqrt() / query_norm) as f32;
    Ok(normalized_l2)
}

pub fn cosine_to_match_score_milli(cosine: f32) -> u32 {
    let score = ((cosine + 1.0) / 2.0 * 1000.0).round() as i64;
    score.clamp(0, 1000) as u32
}

pub fn compute_hidden_similarity(
    query_payload: &[u8],
    candidate_payload: &[u8],
    query_ref: &HotTensorObjectRef,
    candidate_ref: &HotTensorObjectRef,
    policy: HiddenDecodePolicy,
) -> SimilarityResult<(u32, MatchMetric, f32)> {
    if query_ref.dtype != candidate_ref.dtype {
        return Err(SimilarityError::DTypeMismatch {
            query: query_ref.dtype.clone(),
            candidate: candidate_ref.dtype.clone(),
        });
    }
    if query_ref.shape != candidate_ref.shape {
        return Err(SimilarityError::ShapeMismatch {
            query: query_ref.shape.clone(),
            candidate: candidate_ref.shape.clone(),
        });
    }
    let query_vec =
        decode_payload_to_f32(query_payload, query_ref.dtype, &query_ref.shape, policy)?;
    let candidate_vec = decode_payload_to_f32(
        candidate_payload,
        candidate_ref.dtype,
        &candidate_ref.shape,
        policy,
    )?;

    let cosine = compute_cosine_similarity(&query_vec, &candidate_vec)?;
    let match_score_milli = cosine_to_match_score_milli(cosine);
    let normalized_l2 = compute_normalized_l2(&query_vec, &candidate_vec)?;

    Ok((match_score_milli, MatchMetric::Cosine, normalized_l2))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn cosine_identical_vectors() {
        let v = vec![1.0f32, 2.0, 3.0];
        let cosine = compute_cosine_similarity(&v, &v).unwrap();
        assert!((cosine - 1.0).abs() < 1e-6);
    }

    #[test]
    fn cosine_opposite_vectors() {
        let a = vec![1.0f32, 2.0, 3.0];
        let b = vec![-1.0f32, -2.0, -3.0];
        let cosine = compute_cosine_similarity(&a, &b).unwrap();
        assert!((cosine + 1.0).abs() < 1e-6);
    }

    #[test]
    fn cosine_orthogonal_vectors() {
        let a = vec![1.0f32, 0.0];
        let b = vec![0.0f32, 1.0];
        let cosine = compute_cosine_similarity(&a, &b).unwrap();
        assert!(cosine.abs() < 1e-6);
    }

    #[test]
    fn cosine_shape_mismatch() {
        let a = vec![1.0f32, 2.0];
        let b = vec![1.0f32, 2.0, 3.0];
        let err = compute_cosine_similarity(&a, &b).unwrap_err();
        assert!(matches!(err, SimilarityError::ShapeMismatch { .. }));
    }

    #[test]
    fn cosine_nan_rejection() {
        let a = vec![f32::NAN, 1.0];
        let b = vec![1.0f32, 1.0];
        let err = compute_cosine_similarity(&a, &b).unwrap_err();
        assert!(matches!(err, SimilarityError::NaN));
    }

    #[test]
    fn cosine_zero_norm_rejection() {
        let a = vec![0.0f32, 0.0];
        let b = vec![1.0f32, 1.0];
        let err = compute_cosine_similarity(&a, &b).unwrap_err();
        assert!(matches!(err, SimilarityError::ZeroNorm));
    }

    #[test]
    fn match_score_milli_boundaries() {
        assert_eq!(cosine_to_match_score_milli(1.0), 1000);
        assert_eq!(cosine_to_match_score_milli(-1.0), 0);
        assert_eq!(cosine_to_match_score_milli(0.0), 500);
    }

    #[test]
    fn decode_f32_payload() {
        let payload: Vec<u8> = [1.0f32, 2.0, 3.0]
            .iter()
            .flat_map(|f| f.to_le_bytes())
            .collect();
        let decoded =
            decode_payload_to_f32(&payload, TensorDType::F32, &[3], HiddenDecodePolicy::F32)
                .unwrap();
        assert_eq!(decoded, vec![1.0, 2.0, 3.0]);
    }

    #[test]
    fn decode_f16_payload() {
        // f16: 1.0 = 0x3C00, 2.0 = 0x4000
        let payload = vec![0x00, 0x3C, 0x00, 0x40];
        let decoded =
            decode_payload_to_f32(&payload, TensorDType::Opaque, &[2], HiddenDecodePolicy::F16)
                .unwrap();
        assert!((decoded[0] - 1.0).abs() < 1e-3);
        assert!((decoded[1] - 2.0).abs() < 1e-3);
    }

    #[test]
    fn decode_bf16_payload() {
        // bf16: 1.0 = 0x3F80, 2.0 = 0x4000
        let payload = vec![0x80, 0x3F, 0x00, 0x40];
        let decoded = decode_payload_to_f32(
            &payload,
            TensorDType::Opaque,
            &[2],
            HiddenDecodePolicy::BF16,
        )
        .unwrap();
        assert!((decoded[0] - 1.0).abs() < 1e-3);
        assert!((decoded[1] - 2.0).abs() < 1e-3);
    }

    #[test]
    fn decode_payload_size_mismatch() {
        let payload = vec![0u8; 4]; // too short for 3 f32s
        let err = decode_payload_to_f32(&payload, TensorDType::F32, &[3], HiddenDecodePolicy::F32)
            .unwrap_err();
        assert!(matches!(err, SimilarityError::PayloadSizeMismatch { .. }));
    }

    fn dummy_tensor_ref(dtype: TensorDType, shape: Vec<u64>) -> HotTensorObjectRef {
        HotTensorObjectRef {
            object_key: "test".to_string(),
            version: 1,
            backend: crate::HotObjectBackend::ObmmShmem,
            storage_ref: "test".to_string(),
            segment: None,
            offset: 0,
            bytes: shape.iter().product::<u64>() * 4,
            checksum: 0,
            dtype,
            shape,
        }
    }

    #[test]
    fn hidden_similarity_dtype_mismatch() {
        let q = dummy_tensor_ref(TensorDType::F32, vec![2]);
        let c = dummy_tensor_ref(TensorDType::Opaque, vec![2]);
        let err = compute_hidden_similarity(&[0u8; 8], &[0u8; 4], &q, &c, HiddenDecodePolicy::F16)
            .unwrap_err();
        assert!(matches!(err, SimilarityError::DTypeMismatch { .. }));
    }

    #[test]
    fn hidden_similarity_shape_mismatch() {
        let q = dummy_tensor_ref(TensorDType::F32, vec![2]);
        let c = dummy_tensor_ref(TensorDType::F32, vec![3]);
        let err = compute_hidden_similarity(&[0u8; 8], &[0u8; 12], &q, &c, HiddenDecodePolicy::F32)
            .unwrap_err();
        assert!(matches!(err, SimilarityError::ShapeMismatch { .. }));
    }

    #[test]
    fn hidden_similarity_happy_path() {
        let v = [1.0f32, 2.0, 3.0];
        let payload: Vec<u8> = v.iter().flat_map(|f| f.to_le_bytes()).collect();
        let r = dummy_tensor_ref(TensorDType::F32, vec![3]);
        let (score, metric, l2) =
            compute_hidden_similarity(&payload, &payload, &r, &r, HiddenDecodePolicy::F32).unwrap();
        assert_eq!(score, 1000);
        assert_eq!(metric, MatchMetric::Cosine);
        assert!(l2.abs() < 1e-5);
    }

    #[test]
    fn hidden_similarity_allows_distinct_payload_checksums() {
        let payload: Vec<u8> = [1.0f32, 2.0, 3.0]
            .iter()
            .flat_map(|f| f.to_le_bytes())
            .collect();
        let mut query_ref = dummy_tensor_ref(TensorDType::F32, vec![3]);
        let mut candidate_ref = dummy_tensor_ref(TensorDType::F32, vec![3]);
        query_ref.checksum = 0x1111;
        candidate_ref.checksum = 0x2222;

        let (score, _, _) = compute_hidden_similarity(
            &payload,
            &payload,
            &query_ref,
            &candidate_ref,
            HiddenDecodePolicy::F32,
        )
        .unwrap();

        assert_eq!(score, 1000);
    }

    #[test]
    fn match_mode_from_str() {
        assert_eq!(MatchMode::from_str("exact"), Some(MatchMode::Exact));
        assert_eq!(
            MatchMode::from_str("approximate"),
            Some(MatchMode::Approximate)
        );
        assert_eq!(
            MatchMode::from_str("exact-then-approximate"),
            Some(MatchMode::ExactThenApproximate)
        );
        assert_eq!(MatchMode::from_str("unknown"), None);
    }

    #[test]
    fn match_mode_flags() {
        let exact = MatchMode::Exact;
        let exact_then = MatchMode::ExactThenApproximate;
        let approximate = MatchMode::Approximate;
        assert!(exact.is_exact_only());
        assert!(!exact.allows_approximate());
        assert!(!exact_then.is_exact_only());
        assert!(exact_then.allows_approximate());
        assert!(!approximate.is_exact_only());
        assert!(approximate.allows_approximate());
        assert_eq!(exact.preferred_match(), "exact");
        assert_eq!(exact_then.preferred_match(), "exact-then-approximate");
        assert_eq!(approximate.preferred_match(), "approximate");
    }

    #[test]
    fn hidden_decode_policy_only_accepts_typed_f32() {
        assert_eq!(
            HiddenDecodePolicy::from_dtype(TensorDType::F32),
            Some(HiddenDecodePolicy::F32)
        );
        assert_eq!(HiddenDecodePolicy::from_dtype(TensorDType::Opaque), None);
    }

    #[test]
    fn hidden_similarity_does_not_require_exact_checksum_match() {
        let q = dummy_tensor_ref(TensorDType::F32, vec![2]);
        let c = dummy_tensor_ref(TensorDType::F32, vec![2]);
        let q_payload: Vec<u8> = [1.0f32, 2.0].iter().flat_map(|f| f.to_le_bytes()).collect();
        let c_payload = q_payload.clone();
        let mut candidate_ref = c;
        candidate_ref.checksum = 42;
        let (score, _, _) = compute_hidden_similarity(
            &q_payload,
            &c_payload,
            &q,
            &candidate_ref,
            HiddenDecodePolicy::F32,
        )
        .unwrap();
        assert_eq!(score, 1000);
    }
}
