use sim_models::qwen3_dense_reference::{
    load_tokenizer_asset_summary, token_piece_from_policy, token_piece_from_tokenizer_path,
    tokenizer_policy, QWEN3_DENSE_REFERENCE_PROFILE,
    QWEN3_DENSE_REFERENCE_TOKENIZER_ASSET_POLICY_KIND, QWEN3_DENSE_REFERENCE_TOKENIZER_POLICY_KIND,
};

fn main() -> Result<(), Box<dyn std::error::Error>> {
    let args = std::env::args().skip(1).collect::<Vec<_>>();
    if let Some(path) = args.first() {
        let token_id = args
            .get(1)
            .map(|value| value.parse::<u64>())
            .transpose()?
            .unwrap_or(123);
        let path = std::path::Path::new(path);
        let summary = load_tokenizer_asset_summary(path)?;
        let sample = token_piece_from_tokenizer_path(path, token_id)?;
        let output = serde_json::json!({
            "model": summary.model_id,
            "source": summary.source,
            "policy_kind": QWEN3_DENSE_REFERENCE_TOKENIZER_ASSET_POLICY_KIND,
            "aggregate_checksum": summary.aggregate_checksum,
            "vocab_size": summary.vocab_size,
            "vocab_entries": summary.vocab_entries,
            "added_tokens": summary.added_tokens,
            "merge_rules": summary.merge_rules,
            "files": summary.files.iter().map(|file| serde_json::json!({
                "name": file.name,
                "bytes": file.bytes,
                "checksum": file.checksum,
            })).collect::<Vec<_>>(),
            "sample": {
                "token_id": sample.token_id,
                "byte_len": sample.byte_len,
                "word0": sample.word0,
                "word1": sample.word1,
                "checksum": sample.checksum,
            }
        });
        println!("{}", serde_json::to_string_pretty(&output)?);
        return Ok(());
    }

    let policy = tokenizer_policy(QWEN3_DENSE_REFERENCE_PROFILE);
    let sample = token_piece_from_policy(policy, 123);
    let summary = serde_json::json!({
        "model": policy.model_id,
        "tokenizer_family": policy.tokenizer_family,
        "policy_kind": QWEN3_DENSE_REFERENCE_TOKENIZER_POLICY_KIND,
        "policy_hash": policy.policy_hash,
        "vocab_size": policy.vocab_size,
        "synthetic_piece_prefix": policy.synthetic_piece_prefix,
        "synthetic_piece_digits": policy.synthetic_piece_digits,
        "synthetic_piece_bytes": policy.synthetic_piece_bytes,
        "sample": {
            "token_id": sample.token_id,
            "byte_len": sample.byte_len,
            "word0": sample.word0,
            "word1": sample.word1,
            "checksum": sample.checksum,
        }
    });
    println!("{}", serde_json::to_string_pretty(&summary)?);
    Ok(())
}
