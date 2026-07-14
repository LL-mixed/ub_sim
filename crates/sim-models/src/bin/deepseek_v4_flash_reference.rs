use std::env;
use std::fs;
use std::path::PathBuf;

use serde::Serialize;
use sim_models::deepseek_v4_flash_checkpoint::{
    checksum64, DeepseekV4CacheLimits, DeepseekV4CacheStats, DeepseekV4Checkpoint,
    DeepseekV4TensorDType,
};
use sim_models::deepseek_v4_flash_checkpoint_reference::{
    checksum_f32, deterministic_hidden_fixture, round_to_bf16, DeepseekV4ReferenceLayerOutput,
};

const DEFAULT_TENSOR_CACHE_BYTES: u64 = 64 * 1024 * 1024;
const DEFAULT_EXPERT_CACHE_BYTES: u64 = 256 * 1024 * 1024;

#[derive(Debug, PartialEq, Eq)]
enum Command {
    Tensor {
        model: PathBuf,
        tensor: String,
        offset: u64,
        elements: u64,
        limits: DeepseekV4CacheLimits,
    },
    Operator {
        model: PathBuf,
        tensor: String,
        seed: u64,
        row_start: usize,
        row_count: usize,
        limits: DeepseekV4CacheLimits,
    },
    Layer {
        model: PathBuf,
        layer: u64,
        token: u64,
        position: u32,
        seed: u64,
        hidden_file: Option<PathBuf>,
        limits: DeepseekV4CacheLimits,
    },
}

#[derive(Serialize)]
struct OperatorReport {
    model_revision: String,
    config_checksum: String,
    index_checksum: String,
    tensor: String,
    tensor_dtype: String,
    tensor_shape: Vec<u64>,
    row_start: usize,
    row_count: usize,
    weight_slice_checksum: String,
    scale_checksum: Option<String>,
    input_checksum: String,
    output: Vec<f32>,
    output_checksum: String,
    tensor_cache: DeepseekV4CacheStats,
    expert_cache: DeepseekV4CacheStats,
}

fn usage() -> String {
    "usage:\n  deepseek_v4_flash_reference tensor --model DIR --tensor NAME [--offset N] [--elements N] [cache limits]\n  deepseek_v4_flash_reference operator --model DIR --tensor NAME [--seed N] [--row-start N] [--row-count N] [cache limits]\n  deepseek_v4_flash_reference layer --model DIR --layer N --token N [--position 0] [--seed N | --hidden-file FILE] [cache limits]\ncache limits: --tensor-cache-bytes N --expert-cache-bytes N".to_string()
}

fn parse_u64(value: Option<String>, option: &str) -> Result<u64, String> {
    value
        .ok_or_else(|| format!("missing value for {option}"))?
        .parse::<u64>()
        .map_err(|_| format!("invalid integer for {option}"))
}

fn parse_usize(value: Option<String>, option: &str) -> Result<usize, String> {
    usize::try_from(parse_u64(value, option)?)
        .map_err(|_| format!("integer too large for {option}"))
}

fn take_string(value: Option<String>, option: &str) -> Result<String, String> {
    let value = value.ok_or_else(|| format!("missing value for {option}"))?;
    if value.is_empty() {
        return Err(format!("empty value for {option}"));
    }
    Ok(value)
}

fn parse_command(arguments: impl IntoIterator<Item = String>) -> Result<Command, String> {
    let mut arguments = arguments.into_iter();
    let action = arguments.next().ok_or_else(usage)?;
    if !matches!(action.as_str(), "tensor" | "operator" | "layer") {
        return Err(format!("unknown action:{action}\n{}", usage()));
    }

    let mut model = None;
    let mut tensor = None;
    let mut offset = 0;
    let mut elements = 16;
    let mut seed = 1;
    let mut row_start = 0;
    let mut row_count = 8;
    let mut layer = None;
    let mut token = None;
    let mut position = 0;
    let mut hidden_file = None;
    let mut tensor_cache_bytes = DEFAULT_TENSOR_CACHE_BYTES;
    let mut expert_cache_bytes = DEFAULT_EXPERT_CACHE_BYTES;
    while let Some(option) = arguments.next() {
        match option.as_str() {
            "--model" => model = Some(PathBuf::from(take_string(arguments.next(), &option)?)),
            "--tensor" => tensor = Some(take_string(arguments.next(), &option)?),
            "--offset" => offset = parse_u64(arguments.next(), &option)?,
            "--elements" => elements = parse_u64(arguments.next(), &option)?,
            "--seed" => seed = parse_u64(arguments.next(), &option)?,
            "--row-start" => row_start = parse_usize(arguments.next(), &option)?,
            "--row-count" => row_count = parse_usize(arguments.next(), &option)?,
            "--layer" => layer = Some(parse_u64(arguments.next(), &option)?),
            "--token" => token = Some(parse_u64(arguments.next(), &option)?),
            "--position" => {
                position = u32::try_from(parse_u64(arguments.next(), &option)?)
                    .map_err(|_| "integer too large for --position".to_string())?
            }
            "--hidden-file" => {
                hidden_file = Some(PathBuf::from(take_string(arguments.next(), &option)?))
            }
            "--tensor-cache-bytes" => tensor_cache_bytes = parse_u64(arguments.next(), &option)?,
            "--expert-cache-bytes" => expert_cache_bytes = parse_u64(arguments.next(), &option)?,
            _ => return Err(format!("unknown option:{option}\n{}", usage())),
        }
    }
    let model = model.ok_or_else(|| "missing --model".to_string())?;
    let limits = DeepseekV4CacheLimits {
        tensor_bytes: tensor_cache_bytes,
        expert_bytes: expert_cache_bytes,
    };
    match action.as_str() {
        "tensor" => Ok(Command::Tensor {
            model,
            tensor: tensor.ok_or_else(|| "missing --tensor".to_string())?,
            offset,
            elements,
            limits,
        }),
        "operator" => Ok(Command::Operator {
            model,
            tensor: tensor.ok_or_else(|| "missing --tensor".to_string())?,
            seed,
            row_start,
            row_count,
            limits,
        }),
        "layer" => Ok(Command::Layer {
            model,
            layer: layer.ok_or_else(|| "missing --layer".to_string())?,
            token: token.ok_or_else(|| "missing --token".to_string())?,
            position,
            seed,
            hidden_file,
            limits,
        }),
        _ => unreachable!(),
    }
}

fn read_hidden_file(path: &PathBuf, elements: usize) -> Result<Vec<f32>, String> {
    let bytes = fs::read(path).map_err(|error| {
        format!(
            "deepseek_v4_reference_hidden_file_read_failed:{}:{error}",
            path.display()
        )
    })?;
    let expected = elements
        .checked_mul(4)
        .ok_or_else(|| "deepseek_v4_reference_hidden_file_size_overflow".to_string())?;
    if bytes.len() != expected {
        return Err(format!(
            "deepseek_v4_reference_hidden_file_size_invalid:{}:actual={}:expected={expected}",
            path.display(),
            bytes.len()
        ));
    }
    let values: Vec<f32> = bytes
        .chunks_exact(4)
        .map(|chunk| f32::from_le_bytes(chunk.try_into().unwrap()))
        .collect();
    if values.iter().any(|value| !value.is_finite()) {
        return Err("deepseek_v4_reference_hidden_file_non_finite".to_string());
    }
    if values.iter().any(|value| round_to_bf16(*value) != *value) {
        return Err("deepseek_v4_reference_hidden_file_not_bf16".to_string());
    }
    Ok(values)
}

fn matrix_input_size(checkpoint: &DeepseekV4Checkpoint, name: &str) -> Result<usize, String> {
    let metadata = checkpoint.tensor(name)?;
    if metadata.shape.len() != 2 {
        return Err(format!(
            "deepseek_v4_reference_operator_tensor_not_matrix:{name}"
        ));
    }
    let stored = usize::try_from(metadata.shape[1])
        .map_err(|_| "deepseek_v4_reference_operator_input_overflow".to_string())?;
    if metadata.dtype == DeepseekV4TensorDType::I8 {
        stored
            .checked_mul(2)
            .ok_or_else(|| "deepseek_v4_reference_operator_fp4_input_overflow".to_string())
    } else {
        Ok(stored)
    }
}

fn run(command: Command) -> Result<(), String> {
    match command {
        Command::Tensor {
            model,
            tensor,
            offset,
            elements,
            limits,
        } => {
            let checkpoint = DeepseekV4Checkpoint::open(model, limits)?;
            let report = checkpoint.reference_decode_tensor(&tensor, offset, elements)?;
            println!(
                "{}",
                serde_json::to_string_pretty(&report)
                    .map_err(|error| format!("reference report JSON failed:{error}"))?
            );
        }
        Command::Operator {
            model,
            tensor,
            seed,
            row_start,
            row_count,
            limits,
        } => {
            let checkpoint = DeepseekV4Checkpoint::open(model, limits)?;
            let input =
                deterministic_hidden_fixture(seed, matrix_input_size(&checkpoint, &tensor)?);
            let output = checkpoint.reference_matvec_rows(&tensor, &input, row_start, row_count)?;
            let metadata = checkpoint.tensor(&tensor)?;
            let row_bytes = usize::try_from(metadata.shape[1])
                .map_err(|_| "deepseek_v4_reference_operator_row_overflow".to_string())?
                .checked_mul(usize::try_from(metadata.dtype.storage_bytes()).unwrap())
                .ok_or_else(|| "deepseek_v4_reference_operator_row_bytes_overflow".to_string())?;
            let weight_slice = checkpoint.read_tensor_slice(
                &tensor,
                (row_start * row_bytes) as u64,
                (row_count * row_bytes) as u64,
            )?;
            let weight_slice_checksum = format!("fnv1a64:{:016x}", checksum64(&weight_slice));
            let scale_checksum = metadata
                .scale_tensor
                .as_deref()
                .map(|name| {
                    let scale = checkpoint.tensor(name)?;
                    let payload = checkpoint.read_tensor_slice(name, 0, scale.payload_bytes())?;
                    Ok::<String, String>(format!("fnv1a64:{:016x}", checksum64(&payload)))
                })
                .transpose()?;
            let (tensor_cache, expert_cache) = checkpoint.cache_stats()?;
            let report = OperatorReport {
                model_revision: checkpoint.identity.revision.clone(),
                config_checksum: format!("fnv1a64:{:016x}", checkpoint.identity.config_checksum),
                index_checksum: format!("fnv1a64:{:016x}", checkpoint.identity.index_checksum),
                tensor: tensor.clone(),
                tensor_dtype: metadata.dtype.safetensors_name().to_string(),
                tensor_shape: metadata.shape.clone(),
                row_start,
                row_count,
                weight_slice_checksum,
                scale_checksum,
                input_checksum: checksum_f32(&input),
                output_checksum: checksum_f32(&output),
                output,
                tensor_cache,
                expert_cache,
            };
            println!(
                "{}",
                serde_json::to_string_pretty(&report)
                    .map_err(|error| format!("reference report JSON failed:{error}"))?
            );
        }
        Command::Layer {
            model,
            layer,
            token,
            position,
            seed,
            hidden_file,
            limits,
        } => {
            let checkpoint = DeepseekV4Checkpoint::open(model, limits)?;
            let elements =
                usize::try_from(checkpoint.config.hidden_size * checkpoint.config.hc_mult)
                    .map_err(|_| "deepseek_v4_reference_hidden_elements_overflow".to_string())?;
            let hidden = match hidden_file {
                Some(path) => read_hidden_file(&path, elements)?,
                None => deterministic_hidden_fixture(seed, elements),
            };
            let report: DeepseekV4ReferenceLayerOutput =
                checkpoint.reference_layer_forward(layer, token, position, &hidden)?;
            println!(
                "{}",
                serde_json::to_string_pretty(&report)
                    .map_err(|error| format!("reference report JSON failed:{error}"))?
            );
        }
    }
    Ok(())
}

fn main() {
    let result = parse_command(env::args().skip(1)).and_then(run);
    if let Err(error) = result {
        eprintln!("{error}");
        std::process::exit(2);
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn operator_arguments_are_strict_and_bounded() {
        let command = parse_command(
            [
                "operator",
                "--model",
                "/model",
                "--tensor",
                "layers.0.attn.wkv.weight",
                "--seed",
                "7",
                "--row-start",
                "4",
                "--row-count",
                "16",
            ]
            .map(str::to_string),
        )
        .unwrap();
        assert!(matches!(
            command,
            Command::Operator {
                seed: 7,
                row_start: 4,
                row_count: 16,
                ..
            }
        ));
        assert!(
            parse_command(["operator", "--model", "/model", "--bad"].map(str::to_string)).is_err()
        );
    }

    #[test]
    fn layer_arguments_accept_seed_or_hidden_file() {
        let command = parse_command(
            [
                "layer",
                "--model",
                "/model",
                "--layer",
                "3",
                "--token",
                "42",
                "--hidden-file",
                "/tmp/hidden.f32le",
            ]
            .map(str::to_string),
        )
        .unwrap();
        assert!(matches!(
            command,
            Command::Layer {
                layer: 3,
                token: 42,
                hidden_file: Some(_),
                ..
            }
        ));
        assert!(
            parse_command(["layer", "--model", "/model", "--layer", "3"].map(str::to_string))
                .is_err()
        );
    }
}
