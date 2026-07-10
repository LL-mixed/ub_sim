use sim_models::deepseek_v4_flash_adapter::{
    build_ds4_dynamic_library, ds4_eval_layer_slice, ds4_first_token, ds4_tokenize_chat,
    Ds4RunConfig, Ds4SliceConfig,
};
use sim_models::deepseek_v4_flash_gguf::{
    lower_q8_0_matrix_to_bf16_kxn, project_f16_matrix, project_q8_0_matrix, GgufCatalog,
};
use std::collections::BTreeMap;
use std::fs;
use std::path::PathBuf;

fn usage() -> &'static str {
    "usage:\n  deepseek_v4_flash_adapter catalog --model FILE [--tensor NAME]\n  deepseek_v4_flash_adapter lower-q8-bf16 --model FILE --tensor NAME --output FILE\n  deepseek_v4_flash_adapter project-f16 --model FILE --tensor NAME --input FILE --output FILE [--token-index N]\n  deepseek_v4_flash_adapter project-q8 --model FILE --tensor NAME --input FILE --output FILE [--token-index N]\n  deepseek_v4_flash_adapter build-library --ds4-dir DIR --output FILE\n  deepseek_v4_flash_adapter tokenize --library FILE --ds4-dir DIR --model FILE (--prompt TEXT | --prompt-file FILE) [--system TEXT]\n  deepseek_v4_flash_adapter first-token --library FILE --ds4-dir DIR --model FILE (--prompt TEXT | --prompt-file FILE) [--system TEXT] [--ctx N] [--top-k N]\n  deepseek_v4_flash_adapter slice --library FILE --ds4-dir DIR --model FILE --layers START:END --tokens CSV --output FILE [--input FILE] [--position N] [--ctx N] [--logits]"
}

fn read_f32_payload(path: &PathBuf) -> Result<Vec<f32>, String> {
    let payload = fs::read(path)
        .map_err(|err| format!("deepseek_f32_input_read_failed:{}:{err}", path.display()))?;
    if payload.len() % 4 != 0 {
        return Err(format!(
            "deepseek_f32_input_unaligned:{}:{}",
            path.display(),
            payload.len()
        ));
    }
    Ok(payload
        .chunks_exact(4)
        .map(|bytes| f32::from_le_bytes([bytes[0], bytes[1], bytes[2], bytes[3]]))
        .collect())
}

fn f32_payload(values: &[f32]) -> Vec<u8> {
    values
        .iter()
        .flat_map(|value| value.to_le_bytes())
        .collect()
}

fn parse_options(args: &[String]) -> Result<BTreeMap<String, Option<String>>, String> {
    let mut options = BTreeMap::new();
    let mut index = 0usize;
    while index < args.len() {
        let option = args[index].as_str();
        if !option.starts_with("--") {
            return Err(format!("unexpected_argument:{option}"));
        }
        if option == "--logits" {
            options.insert(option.to_string(), None);
            index += 1;
            continue;
        }
        let value = args
            .get(index + 1)
            .ok_or_else(|| format!("missing_value:{option}"))?;
        if value.starts_with("--") {
            return Err(format!("missing_value:{option}"));
        }
        if options
            .insert(option.to_string(), Some(value.clone()))
            .is_some()
        {
            return Err(format!("duplicate_option:{option}"));
        }
        index += 2;
    }
    Ok(options)
}

fn required(options: &BTreeMap<String, Option<String>>, name: &str) -> Result<String, String> {
    options
        .get(name)
        .and_then(Clone::clone)
        .ok_or_else(|| format!("required_option_missing:{name}"))
}

fn optional(options: &BTreeMap<String, Option<String>>, name: &str) -> Option<String> {
    options.get(name).and_then(Clone::clone)
}

fn parse_i32(value: Option<String>, default: i32, label: &str) -> Result<i32, String> {
    match value {
        Some(value) => value
            .parse::<i32>()
            .map_err(|err| format!("invalid_{label}:{value}:{err}")),
        None => Ok(default),
    }
}

fn parse_u32(value: Option<String>, default: u32, label: &str) -> Result<u32, String> {
    match value {
        Some(value) => value
            .parse::<u32>()
            .map_err(|err| format!("invalid_{label}:{value}:{err}")),
        None => Ok(default),
    }
}

fn parse_usize(value: Option<String>, default: usize, label: &str) -> Result<usize, String> {
    match value {
        Some(value) => value
            .parse::<usize>()
            .map_err(|err| format!("invalid_{label}:{value}:{err}")),
        None => Ok(default),
    }
}

fn prompt(options: &BTreeMap<String, Option<String>>) -> Result<String, String> {
    match (
        optional(options, "--prompt"),
        optional(options, "--prompt-file"),
    ) {
        (Some(prompt), None) => Ok(prompt),
        (None, Some(path)) => fs::read_to_string(&path)
            .map_err(|err| format!("prompt_file_read_failed:{path}:{err}"))
            .map(|prompt| prompt.trim_end_matches(['\r', '\n']).to_string()),
        (Some(_), Some(_)) => Err("prompt_options_conflict".to_string()),
        (None, None) => Err("prompt_required".to_string()),
    }
}

fn run_config(options: &BTreeMap<String, Option<String>>) -> Result<Ds4RunConfig, String> {
    Ok(Ds4RunConfig {
        library_path: PathBuf::from(required(options, "--library")?),
        runtime_dir: PathBuf::from(required(options, "--ds4-dir")?),
        model_path: PathBuf::from(required(options, "--model")?),
        prompt: prompt(options)?,
        system: optional(options, "--system").unwrap_or_default(),
        context: parse_i32(optional(options, "--ctx"), 1024, "context")?,
        top_k: parse_i32(optional(options, "--top-k"), 4, "top_k")?,
    })
}

fn parse_layers(value: &str) -> Result<(u32, u32), String> {
    let (start, end) = value
        .split_once(':')
        .ok_or_else(|| format!("invalid_layers:{value}"))?;
    let start = start
        .parse::<u32>()
        .map_err(|err| format!("invalid_layer_start:{start}:{err}"))?;
    let end = end
        .parse::<u32>()
        .map_err(|err| format!("invalid_layer_end:{end}:{err}"))?;
    if start >= end || end > 43 {
        return Err(format!("invalid_layers:{start}:{end}"));
    }
    Ok((start, end))
}

fn parse_tokens(value: &str) -> Result<Vec<i32>, String> {
    let tokens: Result<Vec<_>, _> = value
        .split(',')
        .filter(|value| !value.is_empty())
        .map(|value| {
            value
                .parse::<i32>()
                .map_err(|err| format!("invalid_token:{value}:{err}"))
        })
        .collect();
    let tokens = tokens?;
    if tokens.is_empty() {
        return Err("tokens_empty".to_string());
    }
    Ok(tokens)
}

fn run(args: &[String]) -> Result<(), String> {
    let command = args.first().ok_or_else(|| usage().to_string())?;
    let options = parse_options(&args[1..])?;
    match command.as_str() {
        "catalog" => {
            let model_path = PathBuf::from(required(&options, "--model")?);
            let catalog = GgufCatalog::open(&model_path)?;
            catalog.validate_deepseek_v4_flash()?;
            let tensor = optional(&options, "--tensor")
                .map(|name| catalog.tensor(&name).cloned())
                .transpose()?;
            println!(
                "{}",
                serde_json::to_string_pretty(&serde_json::json!({
                    "status": "ok",
                    "model": model_path,
                    "version": catalog.version,
                    "file_bytes": catalog.file_bytes,
                    "alignment": catalog.alignment,
                    "metadata_count": catalog.metadata_count,
                    "tensor_count": catalog.tensors.len(),
                    "tensor": tensor,
                }))
                .map_err(|err| format!("json_encode_failed:{err}"))?
            );
        }
        "lower-q8-bf16" => {
            let model_path = PathBuf::from(required(&options, "--model")?);
            let tensor_name = required(&options, "--tensor")?;
            let output_path = PathBuf::from(required(&options, "--output")?);
            let catalog = GgufCatalog::open(&model_path)?;
            catalog.validate_deepseek_v4_flash()?;
            let tensor = catalog.tensor(&tensor_name)?;
            if tensor.tensor_type.name != "q8_0" {
                return Err(format!(
                    "deepseek_q8_0_tensor_type_required:{tensor_name}:{}",
                    tensor.tensor_type.name
                ));
            }
            let lowered = lower_q8_0_matrix_to_bf16_kxn(
                &catalog.read_tensor(&tensor_name)?,
                &tensor.dimensions,
            )?;
            fs::write(&output_path, &lowered).map_err(|err| {
                format!(
                    "deepseek_q8_0_lowered_write_failed:{}:{err}",
                    output_path.display()
                )
            })?;
            println!(
                "{}",
                serde_json::json!({
                    "status": "ok",
                    "model": model_path,
                    "tensor": tensor_name,
                    "source_type": "q8_0",
                    "output_type": "bf16",
                    "output_layout": "k_x_n_row_major",
                    "dimensions": tensor.dimensions,
                    "output_bytes": lowered.len(),
                    "output": output_path,
                })
            );
        }
        "project-f16" | "project-q8" => {
            let model_path = PathBuf::from(required(&options, "--model")?);
            let tensor_name = required(&options, "--tensor")?;
            let input_path = PathBuf::from(required(&options, "--input")?);
            let output_path = PathBuf::from(required(&options, "--output")?);
            let catalog = GgufCatalog::open(&model_path)?;
            catalog.validate_deepseek_v4_flash()?;
            let tensor = catalog.tensor(&tensor_name)?;
            let expected_type = if command == "project-f16" {
                "f16"
            } else {
                "q8_0"
            };
            if tensor.tensor_type.name != expected_type {
                return Err(format!(
                    "deepseek_{expected_type}_tensor_type_required:{tensor_name}:{}",
                    tensor.tensor_type.name
                ));
            }
            let hidden_size = usize::try_from(tensor.dimensions[0])
                .map_err(|_| "deepseek_projection_hidden_size_too_large".to_string())?;
            let input = read_f32_payload(&input_path)?;
            if input.len() % hidden_size != 0 {
                return Err(format!(
                    "deepseek_projection_token_major_input_misaligned:values={}:hidden={hidden_size}",
                    input.len()
                ));
            }
            let token_index = parse_usize(optional(&options, "--token-index"), 0, "token_index")?;
            let token_count = input.len() / hidden_size;
            let token_start = token_index
                .checked_mul(hidden_size)
                .ok_or_else(|| "deepseek_projection_token_input_offset_overflow".to_string())?;
            let token_end = token_start
                .checked_add(hidden_size)
                .ok_or_else(|| "deepseek_projection_token_input_end_overflow".to_string())?;
            let token_input = input
                .get(token_start..token_end)
                .ok_or_else(|| {
                    format!(
                        "deepseek_projection_token_index_out_of_range:index={token_index}:tokens={token_count}"
                    )
                })?;
            let payload = catalog.read_tensor(&tensor_name)?;
            let projected = if command == "project-f16" {
                project_f16_matrix(&payload, &tensor.dimensions, token_input)?
            } else {
                project_q8_0_matrix(&payload, &tensor.dimensions, token_input)?
            };
            fs::write(&output_path, f32_payload(&projected)).map_err(|err| {
                format!(
                    "deepseek_projection_write_failed:{}:{err}",
                    output_path.display()
                )
            })?;
            println!(
                "{}",
                serde_json::json!({
                    "status": "ok",
                    "model": model_path,
                    "tensor": tensor_name,
                    "input": input_path,
                    "input_tokens": token_count,
                    "token_index": token_index,
                    "output": output_path,
                    "output_type": "f32",
                    "output_values": projected.len(),
                })
            );
        }
        "build-library" => {
            let ds4_dir = PathBuf::from(required(&options, "--ds4-dir")?);
            let output = PathBuf::from(required(&options, "--output")?);
            build_ds4_dynamic_library(&ds4_dir, &output)?;
            println!(
                "{}",
                serde_json::json!({
                    "status": "ok",
                    "library": output,
                    "ds4_dir": ds4_dir,
                })
            );
        }
        "tokenize" => {
            println!(
                "{}",
                serde_json::to_string_pretty(&ds4_tokenize_chat(&run_config(&options)?)?)
                    .map_err(|err| format!("json_encode_failed:{err}"))?
            );
        }
        "first-token" => {
            println!(
                "{}",
                serde_json::to_string_pretty(&ds4_first_token(&run_config(&options)?)?)
                    .map_err(|err| format!("json_encode_failed:{err}"))?
            );
        }
        "slice" => {
            let (layer_start, layer_end) = parse_layers(&required(&options, "--layers")?)?;
            let config = Ds4SliceConfig {
                library_path: PathBuf::from(required(&options, "--library")?),
                runtime_dir: PathBuf::from(required(&options, "--ds4-dir")?),
                model_path: PathBuf::from(required(&options, "--model")?),
                context: parse_i32(optional(&options, "--ctx"), 1024, "context")?,
                layer_start,
                layer_end,
                position: parse_u32(optional(&options, "--position"), 0, "position")?,
                token_ids: parse_tokens(&required(&options, "--tokens")?)?,
                input_path: optional(&options, "--input").map(PathBuf::from),
                output_path: PathBuf::from(required(&options, "--output")?),
                output_logits: options.contains_key("--logits"),
            };
            println!(
                "{}",
                serde_json::to_string_pretty(&ds4_eval_layer_slice(&config)?)
                    .map_err(|err| format!("json_encode_failed:{err}"))?
            );
        }
        _ => return Err(format!("unknown_command:{command}\n{}", usage())),
    }
    Ok(())
}

fn main() {
    let args: Vec<String> = std::env::args().skip(1).collect();
    if let Err(err) = run(&args) {
        eprintln!("{err}");
        std::process::exit(2);
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parses_topology_independent_layer_ranges() {
        assert_eq!(parse_layers("0:22").unwrap(), (0, 22));
        assert_eq!(parse_layers("22:43").unwrap(), (22, 43));
        assert_eq!(parse_layers("0:15").unwrap(), (0, 15));
        assert!(parse_layers("0:0").is_err());
        assert!(parse_layers("0:44").is_err());
    }

    #[test]
    fn parses_real_token_ids_without_model_specific_count() {
        assert_eq!(parse_tokens("1,2,108149").unwrap(), vec![1, 2, 108149]);
        assert!(parse_tokens("").is_err());
        assert!(parse_tokens("1,bad").is_err());
    }

    #[test]
    fn prompt_requires_exactly_one_source() {
        let empty = BTreeMap::new();
        assert_eq!(prompt(&empty).unwrap_err(), "prompt_required");
        let both = BTreeMap::from([
            ("--prompt".to_string(), Some("x".to_string())),
            ("--prompt-file".to_string(), Some("y".to_string())),
        ]);
        assert_eq!(prompt(&both).unwrap_err(), "prompt_options_conflict");
    }

    #[test]
    fn token_index_defaults_to_zero_and_rejects_invalid_values() {
        assert_eq!(parse_usize(None, 0, "token_index"), Ok(0));
        assert_eq!(
            parse_usize(Some("17".to_string()), 0, "token_index"),
            Ok(17)
        );
        assert!(parse_usize(Some("-1".to_string()), 0, "token_index").is_err());
    }

    #[test]
    fn usage_exposes_f16_and_q8_projection_commands() {
        assert!(usage().contains("project-f16"));
        assert!(usage().contains("project-q8"));
    }
}
