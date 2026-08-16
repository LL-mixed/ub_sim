use sim_models::deepseek_v4_flash_checkpoint::{
    checksum64, process_peak_resident_bytes, DeepseekV4CacheLimits, DeepseekV4Checkpoint,
};
use std::collections::BTreeMap;
use std::env;
use std::path::PathBuf;

const USAGE: &str = "usage:
  deepseek_v4_flash_checkpoint inspect --model DIR [--tensor-cache-bytes N] [--expert-cache-bytes N]
  deepseek_v4_flash_checkpoint validate --model DIR [--tensor-cache-bytes N] [--expert-cache-bytes N]
  deepseek_v4_flash_checkpoint slice --model DIR --tensor NAME [--offset N] [--bytes N] [--tensor-cache-bytes N] [--expert-cache-bytes N]";

fn main() {
    if let Err(err) = run(env::args().skip(1).collect()) {
        eprintln!("{err}");
        eprintln!("{USAGE}");
        std::process::exit(2);
    }
}

fn run(args: Vec<String>) -> Result<(), String> {
    let command = args
        .first()
        .ok_or_else(|| "deepseek_v4_checkpoint_command_missing".to_string())?;
    if !matches!(command.as_str(), "inspect" | "validate" | "slice") {
        return Err(format!(
            "deepseek_v4_checkpoint_command_unsupported:{command}"
        ));
    }
    let options = parse_options(&args[1..])?;
    validate_options(command, &options)?;
    let model = PathBuf::from(required(&options, "--model")?);
    let limits = DeepseekV4CacheLimits {
        tensor_bytes: optional_u64(
            &options,
            "--tensor-cache-bytes",
            DeepseekV4CacheLimits::default().tensor_bytes,
        )?,
        expert_bytes: optional_u64(
            &options,
            "--expert-cache-bytes",
            DeepseekV4CacheLimits::default().expert_bytes,
        )?,
    };
    let checkpoint = DeepseekV4Checkpoint::open(&model, limits)?;
    match command.as_str() {
        "inspect" | "validate" => print_inspection(&checkpoint, command == "validate"),
        "slice" => print_slice(&checkpoint, &options),
        _ => unreachable!("command validated above"),
    }
}

fn parse_options(args: &[String]) -> Result<BTreeMap<String, String>, String> {
    let mut options = BTreeMap::new();
    let mut index = 0usize;
    while index < args.len() {
        let name = &args[index];
        if !name.starts_with("--") {
            return Err(format!("deepseek_v4_checkpoint_unexpected_argument:{name}"));
        }
        let value = args
            .get(index + 1)
            .ok_or_else(|| format!("deepseek_v4_checkpoint_option_value_missing:{name}"))?;
        if value.starts_with("--") {
            return Err(format!(
                "deepseek_v4_checkpoint_option_value_missing:{name}"
            ));
        }
        if options.insert(name.clone(), value.clone()).is_some() {
            return Err(format!("deepseek_v4_checkpoint_option_duplicate:{name}"));
        }
        index += 2;
    }
    Ok(options)
}

fn validate_options(command: &str, options: &BTreeMap<String, String>) -> Result<(), String> {
    for name in options.keys() {
        let common = matches!(
            name.as_str(),
            "--model" | "--tensor-cache-bytes" | "--expert-cache-bytes"
        );
        let slice =
            command == "slice" && matches!(name.as_str(), "--tensor" | "--offset" | "--bytes");
        if !common && !slice {
            return Err(format!("deepseek_v4_checkpoint_option_unsupported:{name}"));
        }
    }
    if command == "slice" && !options.contains_key("--tensor") {
        return Err("deepseek_v4_checkpoint_required_option_missing:--tensor".to_string());
    }
    Ok(())
}

fn required(options: &BTreeMap<String, String>, name: &str) -> Result<String, String> {
    options
        .get(name)
        .cloned()
        .ok_or_else(|| format!("deepseek_v4_checkpoint_required_option_missing:{name}"))
}

fn optional_u64(
    options: &BTreeMap<String, String>,
    name: &str,
    default: u64,
) -> Result<u64, String> {
    match options.get(name) {
        Some(value) => value
            .parse::<u64>()
            .map_err(|err| format!("deepseek_v4_checkpoint_option_invalid:{name}:{value}:{err}")),
        None => Ok(default),
    }
}

fn print_inspection(checkpoint: &DeepseekV4Checkpoint, schema_only: bool) -> Result<(), String> {
    let mut dtype_counts = BTreeMap::new();
    let mut scale_associations = 0usize;
    for tensor in checkpoint.tensors.values() {
        *dtype_counts
            .entry(tensor.dtype.safetensors_name())
            .or_insert(0usize) += 1;
        scale_associations += usize::from(tensor.scale_tensor.is_some());
    }
    let (tensor_cache, expert_cache) = checkpoint.cache_stats()?;
    let output = serde_json::json!({
        "schema_valid": true,
        "schema_only": schema_only,
        "model_dir": checkpoint.root(),
        "identity": checkpoint.identity,
        "config": checkpoint.config,
        "shard_count": checkpoint.shards.len(),
        "shards": checkpoint.shards,
        "tensor_count": checkpoint.tensors.len(),
        "dtype_counts": dtype_counts,
        "scale_associations": scale_associations,
        "total_payload_bytes": checkpoint.total_payload_bytes,
        "metadata_resident_bytes": checkpoint.metadata_resident_bytes,
        "tensor_cache": tensor_cache,
        "expert_cache": expert_cache,
        "process_peak_resident_bytes": process_peak_resident_bytes(),
        "max_position_embeddings_retained_not_validated": checkpoint.config.max_position_embeddings,
    });
    println!(
        "{}",
        serde_json::to_string_pretty(&output)
            .map_err(|err| format!("deepseek_v4_checkpoint_output_serialize_failed:{err}"))?
    );
    Ok(())
}

fn print_slice(
    checkpoint: &DeepseekV4Checkpoint,
    options: &BTreeMap<String, String>,
) -> Result<(), String> {
    let tensor_name = required(options, "--tensor")?;
    let tensor = checkpoint.tensor(&tensor_name)?;
    let offset = optional_u64(options, "--offset", 0)?;
    let remaining = tensor.payload_bytes().checked_sub(offset).ok_or_else(|| {
        format!(
            "deepseek_v4_tensor_slice_offset_oob:{tensor_name}:offset={offset}:tensor_bytes={}",
            tensor.payload_bytes()
        )
    })?;
    let bytes = optional_u64(options, "--bytes", remaining.min(4096))?;
    let payload = if tensor.is_routed_expert() {
        checkpoint.read_expert_slice(&tensor_name, offset, bytes)?
    } else {
        checkpoint.read_tensor_slice(&tensor_name, offset, bytes)?
    };
    let scale = tensor
        .scale_tensor
        .as_deref()
        .map(|name| checkpoint.tensor(name))
        .transpose()?;
    let (tensor_cache, expert_cache) = checkpoint.cache_stats()?;
    let payload_checksum = checksum64(&payload);
    let output = serde_json::json!({
        "model_dir": checkpoint.root(),
        "identity": checkpoint.identity,
        "tensor": tensor,
        "scale": scale,
        "slice_offset": offset,
        "slice_bytes": bytes,
        "payload_checksum": format!("fnv1a64:{payload_checksum:016x}"),
        "tensor_cache": tensor_cache,
        "expert_cache": expert_cache,
        "metadata_resident_bytes": checkpoint.metadata_resident_bytes,
        "process_peak_resident_bytes": process_peak_resident_bytes(),
    });
    println!(
        "{}",
        serde_json::to_string_pretty(&output)
            .map_err(|err| format!("deepseek_v4_checkpoint_output_serialize_failed:{err}"))?
    );
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn cli_parser_rejects_duplicate_and_unknown_options() {
        let duplicate = parse_options(&[
            "--model".to_string(),
            "a".to_string(),
            "--model".to_string(),
            "b".to_string(),
        ])
        .expect_err("duplicate model must fail");
        assert!(duplicate.contains("option_duplicate"));

        let options = parse_options(&["--unknown".to_string(), "x".to_string()])
            .expect("parse syntactically valid option");
        let unknown = validate_options("inspect", &options).expect_err("unknown option must fail");
        assert!(unknown.contains("option_unsupported"));
    }

    #[test]
    fn slice_requires_tensor_name() {
        let options = parse_options(&["--model".to_string(), "model".to_string()])
            .expect("parse model option");
        let error = validate_options("slice", &options).expect_err("tensor must be required");
        assert!(error.contains("--tensor"));
    }
}
