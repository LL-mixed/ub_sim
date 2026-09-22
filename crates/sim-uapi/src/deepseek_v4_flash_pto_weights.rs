//! Read-only GGUF tensor index for the DeepSeek direct PTO kernel.
//!
//! The index carries catalog metadata and the canonical model path. Model
//! payload bytes stay in the GGUF file and are mapped read-only by the host
//! simulation weight provider. Managed hidden, KV and logits never enter this
//! private descriptor.

use sim_models::deepseek_v4_flash_gguf::{GgufCatalog, GgufTensorInfo};
use std::path::Path;

pub(crate) const MAGIC: u64 = u64::from_le_bytes(*b"DS4PTOI1");
pub(crate) const VERSION: u32 = 1;
pub(crate) const HEADER_BYTES: usize = 64;
pub(crate) const RECORD_BYTES: usize = 112;
const MAX_TENSORS: usize = 1_000_000;
const MAX_DESCRIPTOR_BYTES: usize = 64 * 1024 * 1024;

fn append_u32(bytes: &mut Vec<u8>, value: u32) {
    bytes.extend_from_slice(&value.to_le_bytes());
}

fn append_u64(bytes: &mut Vec<u8>, value: u64) {
    bytes.extend_from_slice(&value.to_le_bytes());
}

fn fnv1a(bytes: &[u8]) -> u64 {
    bytes.iter().fold(0xcbf2_9ce4_8422_2325, |hash, byte| {
        (hash ^ u64::from(*byte)).wrapping_mul(0x0000_0100_0000_01b3)
    })
}

fn fingerprint_tensor(hash: &mut u64, tensor: &GgufTensorInfo) {
    let mut field = |bytes: &[u8]| {
        for byte in (bytes.len() as u64).to_le_bytes().iter().chain(bytes) {
            *hash ^= u64::from(*byte);
            *hash = hash.wrapping_mul(0x0000_0100_0000_01b3);
        }
    };
    field(tensor.name.as_bytes());
    field(&tensor.tensor_type.code.to_le_bytes());
    field(&tensor.absolute_offset.to_le_bytes());
    field(&tensor.bytes.to_le_bytes());
    for dimension in &tensor.dimensions {
        field(&dimension.to_le_bytes());
    }
}

fn catalog_fingerprint(catalog: &GgufCatalog) -> u64 {
    let mut hash = 0xcbf2_9ce4_8422_2325;
    for tensor in catalog.tensors.values() {
        fingerprint_tensor(&mut hash, tensor);
    }
    hash
}

pub(crate) fn encode(catalog: &GgufCatalog) -> Result<Vec<u8>, String> {
    if catalog.tensors.is_empty() || catalog.tensors.len() > MAX_TENSORS {
        return Err("deepseek_pto_weight_index_tensor_count".into());
    }
    let path = catalog
        .path
        .to_str()
        .ok_or("deepseek_pto_weight_index_path_encoding")?
        .as_bytes();
    if path.is_empty() || path.contains(&0) {
        return Err("deepseek_pto_weight_index_path".into());
    }

    let records_bytes = catalog
        .tensors
        .len()
        .checked_mul(RECORD_BYTES)
        .ok_or("deepseek_pto_weight_index_size")?;
    let strings_bytes = catalog.tensors.values().try_fold(0usize, |total, tensor| {
        total
            .checked_add(tensor.name.len())
            .ok_or("deepseek_pto_weight_index_size")
    })?;
    let records_offset = HEADER_BYTES;
    let strings_offset = records_offset
        .checked_add(records_bytes)
        .ok_or("deepseek_pto_weight_index_size")?;
    let total_bytes = strings_offset
        .checked_add(strings_bytes)
        .and_then(|value| value.checked_add(path.len()))
        .ok_or("deepseek_pto_weight_index_size")?;
    if total_bytes > MAX_DESCRIPTOR_BYTES
        || catalog.tensors.len() > u32::MAX as usize
        || strings_bytes > u32::MAX as usize
        || path.len() > u32::MAX as usize
    {
        return Err("deepseek_pto_weight_index_size".into());
    }

    let mut bytes = Vec::with_capacity(total_bytes);
    append_u64(&mut bytes, MAGIC);
    append_u32(&mut bytes, VERSION);
    append_u32(&mut bytes, HEADER_BYTES as u32);
    append_u64(&mut bytes, catalog.file_bytes);
    append_u64(&mut bytes, catalog_fingerprint(catalog));
    append_u32(&mut bytes, catalog.tensors.len() as u32);
    append_u32(&mut bytes, RECORD_BYTES as u32);
    append_u64(&mut bytes, records_offset as u64);
    append_u64(&mut bytes, strings_offset as u64);
    append_u32(&mut bytes, strings_bytes as u32);
    append_u32(&mut bytes, path.len() as u32);
    debug_assert_eq!(bytes.len(), HEADER_BYTES);

    let mut name_offset = 0usize;
    for tensor in catalog.tensors.values() {
        if tensor.dimensions.is_empty() || tensor.dimensions.len() > 8 {
            return Err(format!(
                "deepseek_pto_weight_index_rank:{}:{}",
                tensor.name,
                tensor.dimensions.len()
            ));
        }
        let tensor_end = tensor
            .absolute_offset
            .checked_add(tensor.bytes)
            .ok_or_else(|| format!("deepseek_pto_weight_index_bounds:{}", tensor.name))?;
        if tensor.bytes == 0 || tensor_end > catalog.file_bytes {
            return Err(format!("deepseek_pto_weight_index_bounds:{}", tensor.name));
        }
        append_u64(&mut bytes, fnv1a(tensor.name.as_bytes()));
        append_u64(&mut bytes, tensor.absolute_offset);
        append_u64(&mut bytes, tensor.bytes);
        append_u64(&mut bytes, tensor.elements);
        for index in 0..8 {
            append_u64(
                &mut bytes,
                tensor.dimensions.get(index).copied().unwrap_or(0),
            );
        }
        append_u32(&mut bytes, name_offset as u32);
        append_u32(&mut bytes, tensor.name.len() as u32);
        append_u32(&mut bytes, tensor.tensor_type.code);
        append_u32(&mut bytes, tensor.dimensions.len() as u32);
        name_offset += tensor.name.len();
    }
    debug_assert_eq!(bytes.len(), strings_offset);
    for tensor in catalog.tensors.values() {
        bytes.extend_from_slice(tensor.name.as_bytes());
    }
    bytes.extend_from_slice(path);
    debug_assert_eq!(bytes.len(), total_bytes);
    Ok(bytes)
}

pub(crate) fn private_weight_index(path: &Path) -> Result<Vec<u8>, String> {
    let canonical = path
        .canonicalize()
        .map_err(|error| format!("deepseek_pto_weight_path:{error}"))?;
    let catalog = GgufCatalog::open(&canonical)?;
    catalog.validate_deepseek_v4_flash()?;
    encode(&catalog)
}

#[cfg(test)]
mod tests {
    use super::*;
    use sim_models::deepseek_v4_flash_gguf::{GgufTensorInfo, GgufTensorType};
    use std::collections::BTreeMap;
    use std::path::PathBuf;

    fn u32_at(bytes: &[u8], offset: usize) -> u32 {
        u32::from_le_bytes(bytes[offset..offset + 4].try_into().unwrap())
    }

    fn u64_at(bytes: &[u8], offset: usize) -> u64 {
        u64::from_le_bytes(bytes[offset..offset + 8].try_into().unwrap())
    }

    fn tensor(
        name: &str,
        code: u32,
        type_name: &'static str,
        dimensions: Vec<u64>,
        offset: u64,
        bytes: u64,
    ) -> GgufTensorInfo {
        GgufTensorInfo {
            name: name.into(),
            elements: dimensions.iter().product(),
            dimensions,
            tensor_type: GgufTensorType {
                code,
                name: type_name,
                block_elements: 1,
                block_bytes: 1,
            },
            relative_offset: offset - 4096,
            absolute_offset: offset,
            bytes,
        }
    }

    fn catalog() -> GgufCatalog {
        let tensors = [
            tensor("blk.0.attn_q_a.weight", 8, "q8_0", vec![4, 2], 4096, 32),
            tensor(
                "blk.0.ffn_gate_exps.weight",
                16,
                "iq2_xxs",
                vec![256, 64, 4],
                8192,
                32768,
            ),
        ]
        .into_iter()
        .map(|tensor| (tensor.name.clone(), tensor))
        .collect::<BTreeMap<_, _>>();
        GgufCatalog {
            path: PathBuf::from("/models/deepseek-v4-flash.gguf"),
            version: 3,
            file_bytes: 65536,
            alignment: 32,
            tensor_data_offset: 4096,
            metadata_count: 0,
            metadata: BTreeMap::new(),
            tensors,
        }
    }

    #[test]
    fn encodes_bounded_mmap_catalog_with_exact_names() {
        let catalog = catalog();
        let bytes = encode(&catalog).unwrap();
        assert_eq!(u64_at(&bytes, 0), MAGIC);
        assert_eq!(u32_at(&bytes, 8), VERSION);
        assert_eq!(u32_at(&bytes, 12), HEADER_BYTES as u32);
        assert_eq!(u64_at(&bytes, 16), catalog.file_bytes);
        assert_ne!(u64_at(&bytes, 24), 0);
        assert_eq!(u32_at(&bytes, 32), 2);
        assert_eq!(u32_at(&bytes, 36), RECORD_BYTES as u32);
        assert_eq!(u64_at(&bytes, 40), HEADER_BYTES as u64);
        let strings_offset = u64_at(&bytes, 48) as usize;
        let strings_bytes = u32_at(&bytes, 56) as usize;
        let path_bytes = u32_at(&bytes, 60) as usize;
        assert_eq!(strings_offset, HEADER_BYTES + 2 * RECORD_BYTES);
        assert_eq!(
            &bytes[strings_offset + strings_bytes..strings_offset + strings_bytes + path_bytes],
            catalog.path.to_str().unwrap().as_bytes()
        );
        for (record, tensor) in catalog.tensors.values().enumerate() {
            let offset = HEADER_BYTES + record * RECORD_BYTES;
            assert_eq!(u64_at(&bytes, offset), fnv1a(tensor.name.as_bytes()));
            assert_eq!(u64_at(&bytes, offset + 8), tensor.absolute_offset);
            assert_eq!(u64_at(&bytes, offset + 16), tensor.bytes);
            assert_eq!(u64_at(&bytes, offset + 24), tensor.elements);
            assert_eq!(u32_at(&bytes, offset + 104), tensor.tensor_type.code);
            assert_eq!(u32_at(&bytes, offset + 108), tensor.dimensions.len() as u32);
        }
    }

    #[test]
    fn rejects_tensor_outside_model_file() {
        let mut catalog = catalog();
        catalog
            .tensors
            .get_mut("blk.0.attn_q_a.weight")
            .unwrap()
            .bytes = u64::MAX;
        assert!(encode(&catalog)
            .unwrap_err()
            .contains("deepseek_pto_weight_index_bounds"));
    }
}
