use std::ffi::{OsStr, OsString};
use std::fs;
use std::path::{Path, PathBuf};

use sim_models::deepseek_v4_flash::{
    deepseek_v4_flash_rope_coefficients, DEEPSEEK_V4_FLASH_PROFILE, DEEPSEEK_V4_FLASH_RMS_EPS,
};
use sim_models::deepseek_v4_flash_gguf::{
    decode_f32_tensor, project_f16_matrix, project_q8_0_matrix, quantize_q8_0_activation,
    GgufCatalog,
};
use sim_models::deepseek_v4_flash_lowering::{
    deepseek_v4_flash_hc_control_input_reference, deepseek_v4_flash_hc_split_reference,
    deepseek_v4_flash_hc_weighted_sum_reference, deepseek_v4_flash_head_rms_norm_reference,
    deepseek_v4_flash_rms_norm_reference, deepseek_v4_flash_rope_tail_reference,
    deepseek_v4_flash_sink_attention_reference,
};

struct Args {
    model: PathBuf,
    token: usize,
    state: PathBuf,
    output_prefix: PathBuf,
    heads_override: Option<PathBuf>,
}

fn parse_args(args: impl IntoIterator<Item = OsString>) -> Result<Args, String> {
    let args = args.into_iter().collect::<Vec<_>>();
    if args.len() != 4 && args.len() != 6 {
        return Err(
            "usage: deepseek_v4_flash_gguf_attention_reference MODEL_GGUF TOKEN ".to_string()
                + "LAYER0_STATE OUTPUT_PREFIX [--heads HEADS_F32]",
        );
    }
    let heads_override = if args.len() == 6 {
        if args[4].as_os_str() != OsStr::new("--heads") {
            return Err("expected --heads option".to_string());
        }
        Some(PathBuf::from(&args[5]))
    } else {
        None
    };
    let token = args[1]
        .to_str()
        .ok_or("token is not valid UTF-8")?
        .parse::<usize>()
        .map_err(|_| "invalid token".to_string())?;
    Ok(Args {
        model: PathBuf::from(&args[0]),
        token,
        state: PathBuf::from(&args[2]),
        output_prefix: PathBuf::from(&args[3]),
        heads_override,
    })
}

fn tensor_f32(catalog: &GgufCatalog, suffix: &str) -> Result<Vec<f32>, String> {
    let name = format!("blk.0.{suffix}.weight");
    let tensor = catalog.tensor(&name)?;
    if tensor.tensor_type.name != "f32" {
        return Err(format!("expected f32 tensor:{name}"));
    }
    decode_f32_tensor(&catalog.read_tensor(&name)?, &tensor.dimensions)
}

fn project(
    catalog: &GgufCatalog,
    suffix: &str,
    format: &str,
    input: &[f32],
) -> Result<Vec<f32>, String> {
    let name = format!("blk.0.{suffix}.weight");
    let tensor = catalog.tensor(&name)?;
    if tensor.tensor_type.name != format {
        return Err(format!("expected {format} tensor:{name}"));
    }
    let payload = catalog.read_tensor(&name)?;
    match format {
        "f16" => project_f16_matrix(&payload, &tensor.dimensions, input),
        "q8_0" => project_q8_0_matrix(&payload, &tensor.dimensions, input),
        _ => Err(format!("unsupported projection format:{format}")),
    }
}

fn grouped_output_a(catalog: &GgufCatalog, heads: &[f32]) -> Result<Vec<f32>, String> {
    let name = "blk.0.attn_output_a.weight";
    let tensor = catalog.tensor(name)?;
    let [group_input, total_output] = tensor.dimensions.as_slice() else {
        return Err("output-a dimensions invalid".to_string());
    };
    let group_input = usize::try_from(*group_input).map_err(|_| "output-a input too large")?;
    let total_output = usize::try_from(*total_output).map_err(|_| "output-a output too large")?;
    let groups = DEEPSEEK_V4_FLASH_PROFILE.output_groups as usize;
    if tensor.tensor_type.name != "q8_0"
        || group_input == 0
        || group_input % 32 != 0
        || total_output % groups != 0
        || heads.len() != group_input * groups
    {
        return Err("output-a grouped geometry invalid".to_string());
    }
    let payload = catalog.read_tensor(name)?;
    let group_output = total_output / groups;
    let group_bytes = group_output * (group_input / 32) * 34;
    if payload.len() != groups * group_bytes {
        return Err("output-a payload size invalid".to_string());
    }
    let mut output = Vec::with_capacity(total_output);
    for group in 0..groups {
        output.extend(project_q8_0_matrix(
            &payload[group * group_bytes..(group + 1) * group_bytes],
            &[group_input as u64, group_output as u64],
            &heads[group * group_input..(group + 1) * group_input],
        )?);
    }
    Ok(output)
}

fn layer0_kv(path: &Path) -> Result<Vec<f32>, String> {
    let bytes = fs::read(path).map_err(|error| format!("state_read_failed:{error}"))?;
    let width = DEEPSEEK_V4_FLASH_PROFILE.head_dim as usize;
    if bytes.len() != 48 + width * 4 {
        return Err(format!("layer0_state_size_invalid:{}", bytes.len()));
    }
    Ok(bytes[48..]
        .chunks_exact(4)
        .map(|word| f32::from_le_bytes(word.try_into().unwrap()))
        .collect())
}

fn read_f32_exact(path: &Path, elements: usize) -> Result<Vec<f32>, String> {
    let bytes = fs::read(path).map_err(|error| format!("tensor_read_failed:{error}"))?;
    if bytes.len() != elements * 4 {
        return Err(format!("tensor_size_invalid:{}", bytes.len()));
    }
    let values = bytes
        .chunks_exact(4)
        .map(|word| f32::from_le_bytes(word.try_into().unwrap()))
        .collect::<Vec<_>>();
    if values.iter().any(|value| !value.is_finite()) {
        return Err("tensor_contains_nonfinite_value".to_string());
    }
    Ok(values)
}

fn output_path(prefix: &Path, suffix: &str) -> PathBuf {
    let mut path = prefix.as_os_str().to_os_string();
    path.push(OsStr::new(suffix));
    PathBuf::from(path)
}

fn write_f32(prefix: &Path, suffix: &str, values: &[f32]) -> Result<(), String> {
    let path = output_path(prefix, suffix);
    let payload = values
        .iter()
        .flat_map(|value| value.to_le_bytes())
        .collect::<Vec<_>>();
    fs::write(&path, payload)
        .map_err(|error| format!("output_write_failed:{}:{error}", path.display()))
}

fn q8_code_changes(reference: &[f32], candidate: &[f32]) -> Result<(usize, usize), String> {
    if reference.len() != candidate.len() {
        return Err("Q8 activation lengths differ".to_string());
    }
    let reference = quantize_q8_0_activation(reference)?;
    let candidate = quantize_q8_0_activation(candidate)?;
    Ok((
        reference
            .values
            .iter()
            .zip(&candidate.values)
            .filter(|(left, right)| left != right)
            .count(),
        reference
            .scales
            .iter()
            .zip(&candidate.scales)
            .filter(|(left, right)| left.to_bits() != right.to_bits())
            .count(),
    ))
}

fn run(args: Args) -> Result<(), String> {
    let catalog = GgufCatalog::open(&args.model)?;
    catalog.validate_deepseek_v4_flash()?;
    let profile = DEEPSEEK_V4_FLASH_PROFILE;
    if args.token >= profile.vocab_size as usize {
        return Err("token out of range".to_string());
    }
    let embedding = catalog.read_f16_matrix_row("token_embd.weight", args.token)?;
    let hidden = profile.hidden_size as usize;
    let hc = profile.hc_mult as usize;
    if embedding.len() != hidden {
        return Err("embedding width mismatch".to_string());
    }
    let residual = embedding.repeat(hc);
    let eps = DEEPSEEK_V4_FLASH_RMS_EPS;
    let control = deepseek_v4_flash_hc_control_input_reference(&residual, hidden, hc, eps)?;
    let mix = project(&catalog, "hc_attn_fn", "f16", &control)?;
    let split = deepseek_v4_flash_hc_split_reference(
        &mix,
        &tensor_f32(&catalog, "hc_attn_scale")?,
        &tensor_f32(&catalog, "hc_attn_base")?,
        hc,
        profile.hc_sinkhorn_iters as usize,
        eps,
    )?;
    let mixed = deepseek_v4_flash_hc_weighted_sum_reference(&residual, &split.pre, hidden)?;
    let normalized = deepseek_v4_flash_rms_norm_reference(
        &mixed,
        Some(&tensor_f32(&catalog, "attn_norm")?),
        eps,
    )?;
    let q_lora = project(&catalog, "attn_q_a", "q8_0", &normalized)?;
    let q_lora = deepseek_v4_flash_rms_norm_reference(
        &q_lora,
        Some(&tensor_f32(&catalog, "attn_q_a_norm")?),
        eps,
    )?;
    let q = project(&catalog, "attn_q_b", "q8_0", &q_lora)?;
    let heads = profile.num_attention_heads as usize;
    let head_dim = profile.head_dim as usize;
    let rope_dim = profile.qk_rope_head_dim as usize;
    let q = deepseek_v4_flash_head_rms_norm_reference(&q, heads, head_dim, None, eps)?;
    let rope = deepseek_v4_flash_rope_coefficients(0, 0)?;
    let q = deepseek_v4_flash_rope_tail_reference(
        &q, heads, head_dim, rope_dim, &rope.cos, &rope.sin, false,
    )?;
    let kv = layer0_kv(&args.state)?;
    let attention = deepseek_v4_flash_sink_attention_reference(
        &q,
        &kv,
        &tensor_f32(&catalog, "attn_sinks")?,
        heads,
        head_dim,
    )?;
    let attention = deepseek_v4_flash_rope_tail_reference(
        &attention, heads, head_dim, rope_dim, &rope.cos, &rope.sin, true,
    )?;
    let override_heads = args
        .heads_override
        .as_ref()
        .map(|path| read_f32_exact(path, attention.len()))
        .transpose()?;
    if let Some(provided) = &override_heads {
        let (codes, scales) = q8_code_changes(&attention, provided)?;
        println!(
            "head_q8_activation_delta=pass changed_codes={codes} changed_scales={scales} total_codes={}",
            attention.len()
        );
    }
    let output_a = grouped_output_a(&catalog, override_heads.as_deref().unwrap_or(&attention))?;
    let raw_output = project(&catalog, "attn_output_b", "q8_0", &output_a)?;
    write_f32(&args.output_prefix, ".q.f32", &q)?;
    write_f32(&args.output_prefix, ".heads.f32", &attention)?;
    write_f32(&args.output_prefix, ".output-a.f32", &output_a)?;
    write_f32(&args.output_prefix, ".raw.f32", &raw_output)?;
    println!(
        "gguf_attention_reference=pass layer=0 token={} q={} heads={} output={} head_source={}",
        args.token,
        q.len(),
        attention.len(),
        raw_output.len(),
        if override_heads.is_some() {
            "provided"
        } else {
            "scalar"
        }
    );
    Ok(())
}

fn main() {
    let result = parse_args(std::env::args_os().skip(1)).and_then(run);
    if let Err(error) = result {
        eprintln!("{error}");
        std::process::exit(1);
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn arguments_and_output_suffixes_are_explicit() {
        assert!(parse_args([]).is_err());
        let args = parse_args(
            ["model.gguf", "42", "state.bin", "attention-reference"].map(OsString::from),
        )
        .unwrap();
        assert_eq!(args.token, 42);
        assert_eq!(args.state, PathBuf::from("state.bin"));
        assert!(args.heads_override.is_none());
        assert_eq!(
            output_path(&args.output_prefix, ".q.f32"),
            PathBuf::from("attention-reference.q.f32")
        );
        let with_heads = parse_args(
            [
                "model.gguf",
                "42",
                "state.bin",
                "output",
                "--heads",
                "heads.f32",
            ]
            .map(OsString::from),
        )
        .unwrap();
        assert_eq!(with_heads.heads_override, Some(PathBuf::from("heads.f32")));
        assert!(parse_args(
            [
                "model.gguf",
                "42",
                "state.bin",
                "output",
                "--unknown",
                "heads.f32"
            ]
            .map(OsString::from)
        )
        .is_err());
    }

    #[test]
    fn q8_delta_counts_changed_codes_with_shared_scale() {
        let mut reference = [0.0f32; 32];
        reference[0] = 1.0;
        let mut candidate = reference;
        candidate[1] = 0.1;
        assert_eq!(q8_code_changes(&reference, &candidate).unwrap(), (1, 0));
    }
}
