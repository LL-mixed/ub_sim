use anyhow::Context;
use sim_chipbackend_simpler as simpler;
use sim_models::qwen3_dense::{
    profile_from_weights_dir, QWEN3_DENSE_DEFAULT_DECODE_TOKENS,
    QWEN3_DENSE_DEFAULT_PREFILL_TOKENS, QWEN3_DENSE_DEFAULT_TP_NODES,
};
use sim_models::qwen3_dense_reference::{
    embedding_reference_hidden_sequence_for_profile, load_safetensors_path_metadata,
    materialize_full_weight_tensor_payload, token_piece_bytes_from_tokenizer_path,
    token_piece_decode_bytes, tokenize_prompt_from_tokenizer_path, Qwen3DenseReferenceProfile,
    Qwen3DenseReferenceWeightDType, Qwen3DenseReferenceWeightTensorMetadata,
};
use std::collections::BTreeMap;
use std::env;
use std::ffi::OsString;
use std::fs;
use std::path::{Path, PathBuf};
use std::time::{Duration, Instant};

const HIDDEN: usize = 1024;
const Q_HIDDEN: usize = HIDDEN * 2;
const INTER: usize = 3072;
const HEAD_DIM: usize = 128;
const NUM_LAYERS: usize = 28;
const NUM_KV_HEADS: usize = 8;
const PAGE_SIZE: usize = 256;
const RUNTIME_BATCH: usize = 16;
const LOGITS_BATCH_TILE: usize = 16;
const PADDED_VOCAB: usize = 152_064;
const VOCAB_SIZE: usize = 151_936;

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct Qwen3SimplerGenerateArgs {
    pub build_outputs: Vec<PathBuf>,
    pub l3: bool,
    pub model_dir: PathBuf,
    pub prompt: String,
    pub max_seq_len: usize,
    pub max_new_tokens: usize,
    pub platform: String,
    pub device_id: u32,
    pub profile_verbose: bool,
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct Qwen3SimplerGenerateResult {
    pub text: String,
    pub token_ids: Vec<u64>,
    pub finish_reason: String,
}

pub fn args() -> anyhow::Result<Option<Qwen3SimplerGenerateArgs>> {
    args_from(env::args_os().skip(1))
}

pub fn args_from<I, S>(args: I) -> anyhow::Result<Option<Qwen3SimplerGenerateArgs>>
where
    I: IntoIterator<Item = S>,
    S: Into<std::ffi::OsString>,
{
    let mut args = args.into_iter().map(Into::into);
    let mode = match args.next() {
        Some(mode) if mode == "qwen3-simpler-generate" || mode == "qwen3-simpler-l3-generate" => {
            mode
        }
        _ => return Ok(None),
    };
    let legacy_l3 = mode == "qwen3-simpler-l3-generate";
    {
        let mut build_outputs = Vec::new();
        let mut l3 = legacy_l3;
        let mut model_dir = None;
        let mut prompt = None;
        let mut max_seq_len = 512usize;
        let mut max_new_tokens = 10usize;
        let mut platform = "a2a3".to_string();
        let mut device_id = 0u32;
        let mut profile_verbose = false;
        let mut pending = args.peekable();

        while let Some(value) = pending.next() {
            let text = value.to_string_lossy();
            if text == "--build-output" {
                build_outputs.push(PathBuf::from(next_value(&mut pending, "--build-output")?));
            } else if let Some(value) = text.strip_prefix("--build-output=") {
                build_outputs.push(PathBuf::from(value));
            } else if text == "--l3" {
                l3 = true;
            } else if text == "--model-dir" {
                model_dir = Some(PathBuf::from(next_value(&mut pending, "--model-dir")?));
            } else if let Some(value) = text.strip_prefix("--model-dir=") {
                model_dir = Some(PathBuf::from(value));
            } else if text == "--prompt" {
                prompt = Some(next_value(&mut pending, "--prompt")?);
            } else if let Some(value) = text.strip_prefix("--prompt=") {
                prompt = Some(value.to_string());
            } else if text == "--max-seq-len" {
                max_seq_len = parse_positive_usize(
                    "--max-seq-len",
                    &next_value(&mut pending, "--max-seq-len")?,
                )?;
            } else if let Some(value) = text.strip_prefix("--max-seq-len=") {
                max_seq_len = parse_positive_usize("--max-seq-len", value)?;
            } else if text == "--max-new-tokens" {
                max_new_tokens = parse_positive_usize(
                    "--max-new-tokens",
                    &next_value(&mut pending, "--max-new-tokens")?,
                )?;
            } else if let Some(value) = text.strip_prefix("--max-new-tokens=") {
                max_new_tokens = parse_positive_usize("--max-new-tokens", value)?;
            } else if text == "--platform" || text == "-p" {
                platform = next_value(&mut pending, "--platform")?;
            } else if let Some(value) = text.strip_prefix("--platform=") {
                platform = value.to_string();
            } else if text == "--device-id" || text == "-d" {
                device_id = parse_u32("--device-id", &next_value(&mut pending, "--device-id")?)?;
            } else if let Some(value) = text.strip_prefix("--device-id=") {
                device_id = parse_u32("--device-id", value)?;
            } else if text == "--profile-verbose" {
                profile_verbose = true;
            } else if text.starts_with("--") {
                anyhow::bail!("unknown qwen3-simpler-generate option: {text}");
            } else {
                build_outputs.push(PathBuf::from(text.as_ref()));
            }
        }
        if build_outputs.is_empty() {
            anyhow::bail!("at least one --build-output or positional build_output is required");
        }

        Ok(Some(Qwen3SimplerGenerateArgs {
            build_outputs,
            l3,
            model_dir: model_dir.ok_or_else(|| anyhow::anyhow!("--model-dir is required"))?,
            prompt: prompt.ok_or_else(|| anyhow::anyhow!("--prompt is required"))?,
            max_seq_len,
            max_new_tokens,
            platform,
            device_id,
            profile_verbose,
        }))
    }
}

pub fn run(
    args: Qwen3SimplerGenerateArgs,
    runtime_manifest_path: &Path,
) -> anyhow::Result<Qwen3SimplerGenerateResult> {
    validate_args(&args)?;
    if args.l3 {
        run_l3(args, runtime_manifest_path)
    } else {
        run_l2(args, runtime_manifest_path)
    }
}

pub fn runtime_name(args: &Qwen3SimplerGenerateArgs) -> anyhow::Result<String> {
    if args.l3 {
        L3BuildOutput::load(one_build_output(args)?)?.runtime_name()
    } else {
        L2BuildOutputs::load(&args.build_outputs)?.runtime_name()
    }
}

fn run_l3(
    args: Qwen3SimplerGenerateArgs,
    runtime_manifest_path: &Path,
) -> anyhow::Result<Qwen3SimplerGenerateResult> {
    let build = L3BuildOutput::load(one_build_output(&args)?)?;
    let runtime = RuntimePaths::from_manifest(runtime_manifest_path)?;
    if args.profile_verbose {
        eprintln!("qwen3-simpler-generate --l3:");
        eprintln!("  build_output: {}", build.root.display());
        eprintln!("  runtime_host: {}", runtime.host.display());
        eprintln!("  platform: {}", args.platform);
        eprintln!("  device_id: {}", args.device_id);
    }

    let tokenized = tokenize_prompt_from_tokenizer_path(&args.model_dir, &args.prompt)
        .map_err(anyhow::Error::msg)
        .context("failed to tokenize prompt")?;
    let token_ids = tokenized.token_ids;
    if token_ids.is_empty() {
        anyhow::bail!("prompt tokenization produced no tokens");
    }
    if token_ids.len() + args.max_new_tokens > args.max_seq_len {
        anyhow::bail!(
            "prompt tokens ({}) + max_new_tokens ({}) exceeds max_seq_len ({})",
            token_ids.len(),
            args.max_new_tokens,
            args.max_seq_len
        );
    }

    let weights = load_safetensors_path_metadata(&args.model_dir)
        .map_err(anyhow::Error::msg)
        .context("failed to load model safetensors metadata")?;
    let profile = profile_from_weights_dir(
        &args.model_dir,
        None,
        QWEN3_DENSE_DEFAULT_TP_NODES,
        QWEN3_DENSE_DEFAULT_PREFILL_TOKENS,
        QWEN3_DENSE_DEFAULT_DECODE_TOKENS,
    )
    .map_err(anyhow::Error::msg)
    .context("failed to load Qwen3 profile")?;
    let profile = Qwen3DenseReferenceProfile {
        vocab_size: profile.vocab_size,
        hidden_size: profile.hidden_size,
        intermediate_size: profile.intermediate_size,
        num_hidden_layers: profile.num_hidden_layers,
        num_attention_heads: profile.num_attention_heads,
        num_key_value_heads: profile.num_key_value_heads,
        head_dim: profile.head_dim,
        max_position_embeddings: profile.max_position_embeddings,
        rope_theta: profile.rope_theta,
        prefill_tokens: QWEN3_DENSE_DEFAULT_PREFILL_TOKENS,
        decode_tokens: QWEN3_DENSE_DEFAULT_DECODE_TOKENS,
        tp_nodes: QWEN3_DENSE_DEFAULT_TP_NODES,
    };

    let mut tensors = Qwen3SimplerTensors::build(&args, profile, &weights.tensors, &token_ids)?;
    let _runtime_env = EnvGuard::apply(&runtime.env);
    let api = simpler::RuntimeLibrary::load(&runtime.host)
        .map_err(|err| anyhow::anyhow!("failed to load simpler runtime host library: {err}"))?;
    let aicpu = fs::read(&runtime.aicpu).with_context(|| {
        format!(
            "failed to read runtime aicpu binary {}",
            runtime.aicpu.display()
        )
    })?;
    let aicore = fs::read(&runtime.aicore).with_context(|| {
        format!(
            "failed to read runtime aicore binary {}",
            runtime.aicore.display()
        )
    })?;
    let ctx = api
        .create_context(args.device_id as i32)
        .map_err(|err| anyhow::anyhow!("failed to create simpler device context: {err}"))?;

    let prefill = PreparedProgram::load(&build.prefill)?;
    let decode = PreparedProgram::load(&build.decode)?;
    let rms_lmhead = PreparedProgram::load(&build.rms_lmhead)?;

    if args.profile_verbose {
        eprintln!("  prefill kernels: {}", prefill.kernel_count);
        eprintln!("  decode kernels: {}", decode.kernel_count);
        eprintln!("  rms_lmhead kernels: {}", rms_lmhead.kernel_count);
    }

    let mut generated = Vec::with_capacity(args.max_new_tokens);
    let mut seq_len = token_ids.len();

    let total_started = Instant::now();
    let prefill_started = Instant::now();
    dispatch(
        &api,
        &ctx,
        &prefill,
        &runtime,
        &aicpu,
        &aicore,
        prefill_args(&mut tensors)?,
    )?;
    let prefill_elapsed = prefill_started.elapsed();
    if args.profile_verbose {
        eprintln!(
            "[L3-timer] prefill dispatch: {:.2} ms",
            duration_ms(prefill_elapsed)
        );
    }
    let mut current_decode_elapsed = {
        let decode_started = Instant::now();
        dispatch(
            &api,
            &ctx,
            &decode,
            &runtime,
            &aicpu,
            &aicore,
            decode_args(&mut tensors)?,
        )?;
        decode_started.elapsed()
    };

    if args.profile_verbose {
        eprintln!(
            "[L3-timer] initial decode dispatch: {:.2} ms",
            duration_ms(current_decode_elapsed)
        );
    }

    let finish_reason = loop {
        let step = generated.len();
        let step_started = Instant::now();
        tensors.copy_decode_out_to_rms_x();
        let rms_lmhead_started = Instant::now();
        dispatch(
            &api,
            &ctx,
            &rms_lmhead,
            &runtime,
            &aicpu,
            &aicore,
            rms_lmhead_args(&mut tensors)?,
        )?;
        let rms_lmhead_elapsed = rms_lmhead_started.elapsed();
        let sample_started = Instant::now();
        let token = tensors.greedy_token();
        generated.push(token);
        let reached_length = generated.len() >= args.max_new_tokens;
        if !reached_length {
            tensors.set_decode_hidden_from_token(&weights.tensors, token)?;
            seq_len += 1;
            tensors.write_decode_position(seq_len);
        }
        let sample_elapsed = sample_started.elapsed();
        let step_elapsed = step_started.elapsed() + current_decode_elapsed;
        if args.profile_verbose {
            eprintln!(
                "[L3-step] step={step:02} token={token} decode={:.2} ms rms_lmhead={:.2} ms sample_prepare={:.2} ms total={:.2} ms",
                duration_ms(current_decode_elapsed),
                duration_ms(rms_lmhead_elapsed),
                duration_ms(sample_elapsed),
                duration_ms(step_elapsed),
            );
        }
        if reached_length {
            break "length".to_string();
        }

        let decode_started = Instant::now();
        dispatch(
            &api,
            &ctx,
            &decode,
            &runtime,
            &aicpu,
            &aicore,
            decode_args(&mut tensors)?,
        )?;
        current_decode_elapsed = decode_started.elapsed();
    };

    let text = decode_token_text(&args.model_dir, &generated)?;
    if args.profile_verbose {
        eprintln!(
            "[L3-timer] generate total wall-clock: {:.2} ms",
            duration_ms(total_started.elapsed())
        );
    }
    Ok(Qwen3SimplerGenerateResult {
        text,
        token_ids: generated,
        finish_reason,
    })
}

fn run_l2(
    args: Qwen3SimplerGenerateArgs,
    runtime_manifest_path: &Path,
) -> anyhow::Result<Qwen3SimplerGenerateResult> {
    let build = L2BuildOutputs::load(&args.build_outputs)?;
    let runtime = RuntimePaths::from_manifest(runtime_manifest_path)?;
    if args.profile_verbose {
        eprintln!("qwen3-simpler-generate:");
        eprintln!("  prefill: {}", build.prefill.root.display());
        eprintln!("  decode: {}", build.decode.root.display());
        eprintln!("  final_rms: {}", build.final_rms.root.display());
        eprintln!("  lm_head: {}", build.lm_head.root.display());
        eprintln!("  runtime_host: {}", runtime.host.display());
        eprintln!("  platform: {}", args.platform);
        eprintln!("  device_id: {}", args.device_id);
    }

    let tokenized = tokenize_prompt_from_tokenizer_path(&args.model_dir, &args.prompt)
        .map_err(anyhow::Error::msg)
        .context("failed to tokenize prompt")?;
    let token_ids = tokenized.token_ids;
    if token_ids.is_empty() {
        anyhow::bail!("prompt tokenization produced no tokens");
    }
    if token_ids.len() + args.max_new_tokens > args.max_seq_len {
        anyhow::bail!(
            "prompt tokens ({}) + max_new_tokens ({}) exceeds max_seq_len ({})",
            token_ids.len(),
            args.max_new_tokens,
            args.max_seq_len
        );
    }

    let weights = load_safetensors_path_metadata(&args.model_dir)
        .map_err(anyhow::Error::msg)
        .context("failed to load model safetensors metadata")?;
    let profile = profile_from_weights_dir(
        &args.model_dir,
        None,
        QWEN3_DENSE_DEFAULT_TP_NODES,
        QWEN3_DENSE_DEFAULT_PREFILL_TOKENS,
        QWEN3_DENSE_DEFAULT_DECODE_TOKENS,
    )
    .map_err(anyhow::Error::msg)
    .context("failed to load Qwen3 profile")?;
    let profile = Qwen3DenseReferenceProfile {
        vocab_size: profile.vocab_size,
        hidden_size: profile.hidden_size,
        intermediate_size: profile.intermediate_size,
        num_hidden_layers: profile.num_hidden_layers,
        num_attention_heads: profile.num_attention_heads,
        num_key_value_heads: profile.num_key_value_heads,
        head_dim: profile.head_dim,
        max_position_embeddings: profile.max_position_embeddings,
        rope_theta: profile.rope_theta,
        prefill_tokens: QWEN3_DENSE_DEFAULT_PREFILL_TOKENS,
        decode_tokens: QWEN3_DENSE_DEFAULT_DECODE_TOKENS,
        tp_nodes: QWEN3_DENSE_DEFAULT_TP_NODES,
    };

    let mut tensors = Qwen3SimplerTensors::build(&args, profile, &weights.tensors, &token_ids)?;
    let _runtime_env = EnvGuard::apply(&runtime.env);
    let api = simpler::RuntimeLibrary::load(&runtime.host)
        .map_err(|err| anyhow::anyhow!("failed to load simpler runtime host library: {err}"))?;
    let aicpu = fs::read(&runtime.aicpu).with_context(|| {
        format!(
            "failed to read runtime aicpu binary {}",
            runtime.aicpu.display()
        )
    })?;
    let aicore = fs::read(&runtime.aicore).with_context(|| {
        format!(
            "failed to read runtime aicore binary {}",
            runtime.aicore.display()
        )
    })?;
    let ctx = api
        .create_context(args.device_id as i32)
        .map_err(|err| anyhow::anyhow!("failed to create simpler device context: {err}"))?;

    let prefill = PreparedProgram::load(&build.prefill)?;
    let decode = PreparedProgram::load(&build.decode)?;
    let final_rms = PreparedProgram::load(&build.final_rms)?;
    let lm_head = PreparedProgram::load(&build.lm_head)?;

    if args.profile_verbose {
        eprintln!("  prefill kernels: {}", prefill.kernel_count);
        eprintln!("  decode kernels: {}", decode.kernel_count);
        eprintln!("  final_rms kernels: {}", final_rms.kernel_count);
        eprintln!("  lm_head kernels: {}", lm_head.kernel_count);
    }

    let total_started = Instant::now();
    let prefill_started = Instant::now();
    for layer in 0..NUM_LAYERS {
        let layer_started = Instant::now();
        dispatch(
            &api,
            &ctx,
            &prefill,
            &runtime,
            &aicpu,
            &aicore,
            l2_prefill_args(&mut tensors, layer)?,
        )?;
        tensors.copy_prefill_out_to_hidden();
        if args.profile_verbose {
            eprintln!(
                "[L2-prefill] layer={layer:02} dispatch={:.2} ms",
                duration_ms(layer_started.elapsed())
            );
        }
    }
    if args.profile_verbose {
        eprintln!(
            "[L2-timer] prefill total: {:.2} ms",
            duration_ms(prefill_started.elapsed())
        );
    }

    tensors.copy_prefill_last_to_rms_x(token_ids.len());
    let mut current_decode_elapsed = Duration::ZERO;
    let mut generated = Vec::with_capacity(args.max_new_tokens);
    let mut seq_len = token_ids.len();

    let finish_reason = loop {
        let step = generated.len();
        let step_started = Instant::now();
        let final_rms_started = Instant::now();
        dispatch(
            &api,
            &ctx,
            &final_rms,
            &runtime,
            &aicpu,
            &aicore,
            final_rms_args(&mut tensors)?,
        )?;
        let final_rms_elapsed = final_rms_started.elapsed();
        let lm_head_started = Instant::now();
        dispatch(
            &api,
            &ctx,
            &lm_head,
            &runtime,
            &aicpu,
            &aicore,
            lm_head_args(&mut tensors)?,
        )?;
        let lm_head_elapsed = lm_head_started.elapsed();
        let sample_started = Instant::now();
        let token = tensors.greedy_token();
        generated.push(token);
        let reached_length = generated.len() >= args.max_new_tokens;
        if !reached_length {
            tensors.set_decode_hidden_from_token(&weights.tensors, token)?;
            seq_len += 1;
            tensors.write_decode_position(seq_len);
        }
        let sample_elapsed = sample_started.elapsed();
        let step_elapsed = step_started.elapsed() + current_decode_elapsed;
        if args.profile_verbose {
            eprintln!(
                "[L2-step] step={step:02} token={token} decode={:.2} ms final_rms={:.2} ms lm_head={:.2} ms sample_prepare={:.2} ms total={:.2} ms",
                duration_ms(current_decode_elapsed),
                duration_ms(final_rms_elapsed),
                duration_ms(lm_head_elapsed),
                duration_ms(sample_elapsed),
                duration_ms(step_elapsed),
            );
        }
        if reached_length {
            break "length".to_string();
        }

        let decode_started = Instant::now();
        dispatch(
            &api,
            &ctx,
            &decode,
            &runtime,
            &aicpu,
            &aicore,
            decode_args(&mut tensors)?,
        )?;
        current_decode_elapsed = decode_started.elapsed();
        tensors.copy_decode_out_to_rms_x();
    };

    let text = decode_token_text(&args.model_dir, &generated)?;
    if args.profile_verbose {
        eprintln!(
            "[L2-timer] generate total wall-clock: {:.2} ms",
            duration_ms(total_started.elapsed())
        );
    }
    Ok(Qwen3SimplerGenerateResult {
        text,
        token_ids: generated,
        finish_reason,
    })
}

fn validate_args(args: &Qwen3SimplerGenerateArgs) -> anyhow::Result<()> {
    if !matches!(args.platform.as_str(), "a2a3" | "a2a3sim" | "a5" | "a5sim") {
        anyhow::bail!("unsupported --platform {}", args.platform);
    }
    if args.max_seq_len == 0 || args.max_seq_len % PAGE_SIZE != 0 {
        anyhow::bail!("--max-seq-len must be a positive multiple of {PAGE_SIZE}");
    }
    if args.max_new_tokens == 0 {
        anyhow::bail!("--max-new-tokens must be > 0");
    }
    Ok(())
}

fn one_build_output(args: &Qwen3SimplerGenerateArgs) -> anyhow::Result<&Path> {
    if args.build_outputs.len() != 1 {
        anyhow::bail!(
            "--l3 mode requires exactly one Qwen3GenChunked build_output; got {}",
            args.build_outputs.len()
        );
    }
    Ok(&args.build_outputs[0])
}

fn duration_ms(duration: Duration) -> f64 {
    duration.as_secs_f64() * 1000.0
}

fn dispatch(
    api: &simpler::RuntimeLibrary,
    ctx: &simpler::DeviceContext<'_>,
    program: &PreparedProgram,
    runtime: &RuntimePaths,
    aicpu: &[u8],
    aicore: &[u8],
    prepared: PreparedArgs,
) -> anyhow::Result<()> {
    let runtime_buf = simpler::RuntimeBuffer::allocate(api)
        .map_err(|err| anyhow::anyhow!("simpler runtime allocation failed: {err}"))?;
    api.run_runtime(
        ctx,
        runtime_buf.handle(),
        &program.callable,
        &prepared.task_args,
        program.block_dim.unwrap_or(runtime.block_dim) as i32,
        program.aicpu_thread_num.unwrap_or(runtime.aicpu_thread_num) as i32,
        runtime.device_id as i32,
        aicpu.as_ptr(),
        aicpu.len(),
        aicore.as_ptr(),
        aicore.len(),
    )
    .map_err(|err| anyhow::anyhow!("simpler runtime dispatch failed: {err}"))
}

#[derive(Clone, Debug)]
struct L3BuildOutput {
    root: PathBuf,
    prefill: NextLevelProgram,
    decode: NextLevelProgram,
    rms_lmhead: NextLevelProgram,
}

impl L3BuildOutput {
    fn load(path: &Path) -> anyhow::Result<Self> {
        let root = path
            .canonicalize()
            .with_context(|| format!("failed to canonicalize {}", path.display()))?;
        require_file(&root.join("orchestration").join("host_orch.py"))?;
        let next = root.join("next_levels");
        let prefill = NextLevelProgram::load_with_kind(
            &next.join("qwen3_prefill_all"),
            ProgramKind::L3Prefill,
        )?;
        let decode = NextLevelProgram::load_with_kind(
            &next.join("qwen3_decode_all"),
            ProgramKind::L3Decode,
        )?;
        let rms_lmhead = NextLevelProgram::load_with_kind(
            &next.join("qwen3_rms_lmhead"),
            ProgramKind::L3RmsLmHead,
        )?;
        Ok(Self {
            root,
            prefill,
            decode,
            rms_lmhead,
        })
    }

    fn runtime_name(&self) -> anyhow::Result<String> {
        common_runtime_name([
            &self.prefill.runtime_name,
            &self.decode.runtime_name,
            &self.rms_lmhead.runtime_name,
        ])
    }
}

#[derive(Clone, Debug)]
struct L2BuildOutputs {
    prefill: NextLevelProgram,
    decode: NextLevelProgram,
    final_rms: NextLevelProgram,
    lm_head: NextLevelProgram,
}

impl L2BuildOutputs {
    fn load(paths: &[PathBuf]) -> anyhow::Result<Self> {
        let mut prefill = None;
        let mut decode = None;
        let mut final_rms = None;
        let mut lm_head = None;
        for path in paths {
            let program = NextLevelProgram::load_l2(path)?;
            let slot = match program.kind {
                ProgramKind::L2Prefill => &mut prefill,
                ProgramKind::L2Decode => &mut decode,
                ProgramKind::L2FinalRms => &mut final_rms,
                ProgramKind::L2LmHead => &mut lm_head,
                _ => anyhow::bail!(
                    "{} is not a supported L2 Qwen3 build_output",
                    path.display()
                ),
            };
            if slot.is_some() {
                anyhow::bail!(
                    "duplicate L2 build_output kind {:?}: {}",
                    program.kind,
                    path.display()
                );
            }
            *slot = Some(program);
        }
        Ok(Self {
            prefill: prefill
                .ok_or_else(|| anyhow::anyhow!("missing Qwen306BPrefillProgram build_output"))?,
            decode: decode.ok_or_else(|| anyhow::anyhow!("missing Qwen3Decode build_output"))?,
            final_rms: final_rms
                .ok_or_else(|| anyhow::anyhow!("missing Qwen3FinalRMS build_output"))?,
            lm_head: lm_head.ok_or_else(|| anyhow::anyhow!("missing Qwen3LMHead build_output"))?,
        })
    }

    fn runtime_name(&self) -> anyhow::Result<String> {
        common_runtime_name([
            &self.prefill.runtime_name,
            &self.decode.runtime_name,
            &self.final_rms.runtime_name,
            &self.lm_head.runtime_name,
        ])
    }
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
enum ProgramKind {
    L2Prefill,
    L2Decode,
    L2FinalRms,
    L2LmHead,
    L3Prefill,
    L3Decode,
    L3RmsLmHead,
}

#[derive(Clone, Debug)]
struct NextLevelProgram {
    root: PathBuf,
    orch_so: PathBuf,
    function_name: String,
    runtime_name: String,
    block_dim: Option<u32>,
    aicpu_thread_num: Option<u32>,
    kernels: Vec<KernelArtifact>,
    kind: ProgramKind,
}

#[derive(Clone, Debug)]
struct KernelArtifact {
    func_id: i32,
    path: PathBuf,
}

impl NextLevelProgram {
    fn load_l2(path: &Path) -> anyhow::Result<Self> {
        let root = path
            .canonicalize()
            .with_context(|| format!("failed to canonicalize build_output {}", path.display()))?;
        let config_path = root.join("kernel_config.py");
        let config = fs::read_to_string(&config_path)
            .with_context(|| format!("failed to read {}", config_path.display()))?;
        let function_name =
            parse_function_name(&config).unwrap_or_else(|| "aicpu_orchestration_entry".to_string());
        let marker = format!(
            "{} {}",
            root.file_name()
                .and_then(|name| name.to_str())
                .unwrap_or_default(),
            function_name
        )
        .to_lowercase();
        let kind = if marker.contains("prefill") {
            ProgramKind::L2Prefill
        } else if marker.contains("decode") {
            ProgramKind::L2Decode
        } else if marker.contains("finalrms") || marker.contains("final_rms") {
            ProgramKind::L2FinalRms
        } else if marker.contains("lmhead") || marker.contains("lm_head") {
            ProgramKind::L2LmHead
        } else {
            anyhow::bail!("cannot classify Qwen3 L2 build_output {}", root.display());
        };
        Self::load_with_config(root, config, kind)
    }

    fn load_with_kind(path: &Path, kind: ProgramKind) -> anyhow::Result<Self> {
        let root = path
            .canonicalize()
            .with_context(|| format!("failed to canonicalize next_level {}", path.display()))?;
        let config_path = root.join("kernel_config.py");
        require_file(&config_path)?;
        let config = fs::read_to_string(&config_path)
            .with_context(|| format!("failed to read {}", config_path.display()))?;
        Self::load_with_config(root, config, kind)
    }

    fn load_with_config(root: PathBuf, config: String, kind: ProgramKind) -> anyhow::Result<Self> {
        let function_name =
            parse_function_name(&config).unwrap_or_else(|| "aicpu_orchestration_entry".to_string());
        let runtime_name = parse_runtime_config_string(&config, "runtime")
            .unwrap_or_else(|| "host_build_graph".to_string());
        let block_dim = parse_runtime_config_u32(&config, "block_dim");
        let aicpu_thread_num = parse_runtime_config_u32(&config, "aicpu_thread_num");
        let orch_so = find_orchestration_so(&root)?;
        require_file(&orch_so)?;
        let kernels = parse_kernels(&root, &config)?;
        if kernels.is_empty() {
            anyhow::bail!("no kernels found in {}/kernel_config.py", root.display());
        }
        Ok(Self {
            root,
            orch_so,
            function_name,
            runtime_name,
            block_dim,
            aicpu_thread_num,
            kernels,
            kind,
        })
    }
}

struct PreparedProgram {
    callable: simpler::CallableBuffer,
    kernel_count: usize,
    block_dim: Option<u32>,
    aicpu_thread_num: Option<u32>,
}

impl PreparedProgram {
    fn load(program: &NextLevelProgram) -> anyhow::Result<Self> {
        let orch = fs::read(&program.orch_so)
            .with_context(|| format!("failed to read {}", program.orch_so.display()))?;
        let kernel_bytes = program
            .kernels
            .iter()
            .map(|kernel| {
                load_kernel_binary(&kernel.path)
                    .with_context(|| format!("failed to load kernel {}", kernel.path.display()))
            })
            .collect::<anyhow::Result<Vec<_>>>()?;
        let kernel_inputs = program
            .kernels
            .iter()
            .zip(kernel_bytes.iter())
            .map(|(kernel, bytes)| simpler::KernelCallableInput {
                func_id: kernel.func_id,
                binary: bytes.as_slice(),
            })
            .collect::<Vec<_>>();
        let signature = signature_for_program(program)?;
        let callable =
            simpler::make_chip_callable(&program.function_name, &orch, &kernel_inputs, &signature)
                .map_err(|err| {
                    anyhow::anyhow!(
                        "failed to make callable for {}: {err}",
                        program.root.display()
                    )
                })?;
        Ok(Self {
            callable,
            kernel_count: program.kernels.len(),
            block_dim: program.block_dim,
            aicpu_thread_num: program.aicpu_thread_num,
        })
    }
}

fn signature_for_program(program: &NextLevelProgram) -> anyhow::Result<Vec<simpler::ArgDirection>> {
    let input = simpler::ArgDirection::In;
    let output = simpler::ArgDirection::Out;
    let inout = simpler::ArgDirection::Inout;
    match program.kind {
        ProgramKind::L2Prefill | ProgramKind::L3Prefill => Ok(vec![
            input, input, input, input, input, input, input, input, input, input, input, input,
            inout, inout, input, input, input, input, input, output,
        ]),
        ProgramKind::L2Decode | ProgramKind::L3Decode => Ok(vec![
            input, input, input, input, input, input, input, input, input, input, input, input,
            inout, inout, input, input, input, input, input, output,
        ]),
        ProgramKind::L2FinalRms => Ok(vec![input, input, output]),
        ProgramKind::L2LmHead => Ok(vec![input, input, output]),
        ProgramKind::L3RmsLmHead => Ok(vec![input, input, input, output, output]),
    }
}

#[derive(Debug)]
struct RuntimePaths {
    host: PathBuf,
    aicpu: PathBuf,
    aicore: PathBuf,
    block_dim: u32,
    aicpu_thread_num: u32,
    device_id: u32,
    env: BTreeMap<String, String>,
}

impl RuntimePaths {
    fn from_manifest(path: &Path) -> anyhow::Result<Self> {
        let text = fs::read_to_string(path)
            .with_context(|| format!("failed to read runtime manifest {}", path.display()))?;
        let value: serde_json::Value = serde_json::from_str(&text)
            .with_context(|| format!("failed to parse runtime manifest {}", path.display()))?;
        let runtime = value
            .get("simpler_runtime")
            .ok_or_else(|| anyhow::anyhow!("runtime manifest missing simpler_runtime"))?;
        Ok(Self {
            host: artifact_source(runtime, "host_runtime_library")?,
            aicpu: artifact_source(runtime, "aicpu_binary")?,
            aicore: artifact_source(runtime, "aicore_binary")?,
            block_dim: runtime
                .get("launch")
                .and_then(|v| v.get("block_dim"))
                .and_then(serde_json::Value::as_u64)
                .unwrap_or(3) as u32,
            aicpu_thread_num: runtime
                .get("launch")
                .and_then(|v| v.get("aicpu_thread_num"))
                .and_then(serde_json::Value::as_u64)
                .unwrap_or(4) as u32,
            device_id: 0,
            env: runtime_env(runtime)?,
        })
    }
}

struct EnvGuard {
    saved: Vec<(OsString, Option<OsString>)>,
}

impl EnvGuard {
    fn apply(overrides: &BTreeMap<String, String>) -> Self {
        let mut saved = Vec::with_capacity(overrides.len());
        for (key, value) in overrides {
            let key_os = OsString::from(key);
            saved.push((key_os.clone(), env::var_os(key)));
            unsafe {
                env::set_var(key_os, value);
            }
        }
        Self { saved }
    }
}

impl Drop for EnvGuard {
    fn drop(&mut self) {
        for (key, previous) in self.saved.drain(..).rev() {
            match previous {
                Some(value) => unsafe {
                    env::set_var(&key, value);
                },
                None => unsafe {
                    env::remove_var(&key);
                },
            }
        }
    }
}

struct Qwen3SimplerTensors {
    prefill_hidden: TensorBuf,
    prefill_seq_lens: TensorBuf,
    prefill_slot_mapping: TensorBuf,
    decode_hidden: TensorBuf,
    decode_seq_lens: TensorBuf,
    decode_slot_mapping: TensorBuf,
    input_rms_weight: TensorBuf,
    wq: TensorBuf,
    wk: TensorBuf,
    wv: TensorBuf,
    q_norm_weight: TensorBuf,
    k_norm_weight: TensorBuf,
    rope_cos: TensorBuf,
    rope_sin: TensorBuf,
    block_table: TensorBuf,
    k_cache_all: TensorBuf,
    v_cache_all: TensorBuf,
    wo: TensorBuf,
    post_rms_weight: TensorBuf,
    w_gate: TensorBuf,
    w_up: TensorBuf,
    w_down: TensorBuf,
    prefill_out: TensorBuf,
    decode_out: TensorBuf,
    rms_x: TensorBuf,
    final_norm_weight: TensorBuf,
    rms_normed: TensorBuf,
    lm_head_weight_t: TensorBuf,
    logits_padded: TensorBuf,
}

impl Qwen3SimplerTensors {
    fn build(
        args: &Qwen3SimplerGenerateArgs,
        profile: Qwen3DenseReferenceProfile,
        tensors: &BTreeMap<String, Qwen3DenseReferenceWeightTensorMetadata>,
        prompt_tokens: &[u64],
    ) -> anyhow::Result<Self> {
        let max_seq = args.max_seq_len;
        let max_blocks = max_seq.div_ceil(PAGE_SIZE);
        let total_pages = RUNTIME_BATCH * max_blocks;
        let kv_rows = NUM_LAYERS * total_pages * NUM_KV_HEADS * PAGE_SIZE;

        let prompt_hidden =
            embedding_reference_hidden_sequence_for_profile(profile, tensors, prompt_tokens)
                .map_err(anyhow::Error::msg)
                .context("failed to materialize prompt embeddings")?;
        let mut prefill_hidden = TensorBuf::zero_bf16(&[1, max_seq, HIDDEN]);
        for (row, hidden) in prompt_hidden.iter().enumerate() {
            write_f32_as_bf16(&mut prefill_hidden.data, row * HIDDEN, hidden);
        }
        let mut decode_hidden = TensorBuf::zero_bf16(&[1, HIDDEN]);
        write_f32_as_bf16(
            &mut decode_hidden.data,
            0,
            prompt_hidden.last().expect("nonempty prompt hidden"),
        );

        let mut prefill_seq_lens = TensorBuf::zero_i32(&[1]);
        write_i32(&mut prefill_seq_lens.data, 0, prompt_tokens.len() as i32);
        let mut decode_seq_lens = TensorBuf::zero_i32(&[1]);
        write_i32(&mut decode_seq_lens.data, 0, prompt_tokens.len() as i32);

        let mut block_table = TensorBuf::filled_i32(&[max_blocks], -1);
        for page in
            0..max_blocks.min((prompt_tokens.len() + args.max_new_tokens).div_ceil(PAGE_SIZE))
        {
            write_i32(&mut block_table.data, page, page as i32);
        }
        let mut prefill_slot_mapping = TensorBuf::filled_i32(&[max_seq], -1);
        for pos in 0..prompt_tokens.len() {
            write_i32(&mut prefill_slot_mapping.data, pos, pos as i32);
        }
        let mut decode_slot_mapping = TensorBuf::zero_i32(&[1]);
        write_i32(
            &mut decode_slot_mapping.data,
            0,
            (prompt_tokens.len() - 1) as i32,
        );

        let (rope_cos, rope_sin) = rope_tables(max_seq, HEAD_DIM, profile.rope_theta as f32);

        Ok(Self {
            prefill_hidden,
            prefill_seq_lens,
            prefill_slot_mapping,
            decode_hidden,
            decode_seq_lens,
            decode_slot_mapping,
            input_rms_weight: stack_norm(tensors, "input_layernorm.weight", HIDDEN)?,
            wq: stack_transposed(tensors, "self_attn.q_proj.weight", HIDDEN, Q_HIDDEN)?,
            wk: stack_transposed(tensors, "self_attn.k_proj.weight", HIDDEN, HIDDEN)?,
            wv: stack_transposed(tensors, "self_attn.v_proj.weight", HIDDEN, HIDDEN)?,
            q_norm_weight: stack_optional_norm(tensors, "self_attn.q_norm.weight", HEAD_DIM)?,
            k_norm_weight: stack_optional_norm(tensors, "self_attn.k_norm.weight", HEAD_DIM)?,
            rope_cos,
            rope_sin,
            block_table,
            k_cache_all: TensorBuf::zero_bf16(&[kv_rows, HEAD_DIM]),
            v_cache_all: TensorBuf::zero_bf16(&[kv_rows, HEAD_DIM]),
            wo: stack_transposed(tensors, "self_attn.o_proj.weight", Q_HIDDEN, HIDDEN)?,
            post_rms_weight: stack_norm(tensors, "post_attention_layernorm.weight", HIDDEN)?,
            w_gate: stack_transposed(tensors, "mlp.gate_proj.weight", HIDDEN, INTER)?,
            w_up: stack_transposed(tensors, "mlp.up_proj.weight", HIDDEN, INTER)?,
            w_down: stack_transposed(tensors, "mlp.down_proj.weight", INTER, HIDDEN)?,
            prefill_out: TensorBuf::zero_bf16(&[1, max_seq, HIDDEN]),
            decode_out: TensorBuf::zero_bf16(&[1, HIDDEN]),
            rms_x: TensorBuf::zero_bf16(&[LOGITS_BATCH_TILE, HIDDEN]),
            final_norm_weight: full_norm(tensors, "model.norm.weight", HIDDEN)?,
            rms_normed: TensorBuf::zero_bf16(&[LOGITS_BATCH_TILE, HIDDEN]),
            lm_head_weight_t: lm_head_weight(tensors)?,
            logits_padded: TensorBuf::zero_f32(&[LOGITS_BATCH_TILE, PADDED_VOCAB]),
        })
    }

    fn copy_decode_out_to_rms_x(&mut self) {
        self.rms_x.data.fill(0);
        let row_bytes = HIDDEN * 2;
        self.rms_x.data[..row_bytes].copy_from_slice(&self.decode_out.data[..row_bytes]);
    }

    fn copy_prefill_out_to_hidden(&mut self) {
        self.prefill_hidden
            .data
            .copy_from_slice(&self.prefill_out.data);
    }

    fn copy_prefill_last_to_rms_x(&mut self, seq_len: usize) {
        self.rms_x.data.fill(0);
        let row_bytes = HIDDEN * 2;
        let start = (seq_len - 1) * row_bytes;
        self.rms_x.data[..row_bytes]
            .copy_from_slice(&self.prefill_hidden.data[start..start + row_bytes]);
    }

    fn greedy_token(&self) -> u64 {
        let mut best = 0usize;
        let mut best_value = f32::NEG_INFINITY;
        for token in 0..VOCAB_SIZE {
            let value = read_f32(&self.logits_padded.data, token);
            if value > best_value {
                best = token;
                best_value = value;
            }
        }
        best as u64
    }

    fn set_decode_hidden_from_token(
        &mut self,
        tensors: &BTreeMap<String, Qwen3DenseReferenceWeightTensorMetadata>,
        token: u64,
    ) -> anyhow::Result<()> {
        let profile = Qwen3DenseReferenceProfile {
            vocab_size: VOCAB_SIZE as u64,
            hidden_size: HIDDEN as u64,
            intermediate_size: INTER as u64,
            num_hidden_layers: NUM_LAYERS as u64,
            num_attention_heads: 16,
            num_key_value_heads: NUM_KV_HEADS as u64,
            head_dim: HEAD_DIM as u64,
            max_position_embeddings: 40_960,
            rope_theta: 1_000_000,
            prefill_tokens: 128,
            decode_tokens: 1,
            tp_nodes: 8,
        };
        let hidden = embedding_reference_hidden_sequence_for_profile(profile, tensors, &[token])
            .map_err(anyhow::Error::msg)
            .context("failed to materialize sampled token embedding")?;
        self.decode_hidden.data.fill(0);
        write_f32_as_bf16(&mut self.decode_hidden.data, 0, &hidden[0]);
        Ok(())
    }

    fn write_decode_position(&mut self, seq_len: usize) {
        write_i32(&mut self.decode_seq_lens.data, 0, seq_len as i32);
        write_i32(&mut self.decode_slot_mapping.data, 0, (seq_len - 1) as i32);
    }
}

#[derive(Clone)]
struct TensorBuf {
    data: Vec<u8>,
    shape: Vec<u32>,
    dtype: simpler::DataType,
}

impl TensorBuf {
    fn zero_bf16(shape: &[usize]) -> Self {
        Self::zero(shape, simpler::DataType::Bfloat16)
    }
    fn zero_f32(shape: &[usize]) -> Self {
        Self::zero(shape, simpler::DataType::Float32)
    }
    fn zero_i32(shape: &[usize]) -> Self {
        Self::zero(shape, simpler::DataType::Int32)
    }
    fn filled_i32(shape: &[usize], value: i32) -> Self {
        let mut out = Self::zero_i32(shape);
        for index in 0..out.elem_count() {
            write_i32(&mut out.data, index, value);
        }
        out
    }
    fn zero(shape: &[usize], dtype: simpler::DataType) -> Self {
        let elems = shape.iter().product::<usize>();
        Self {
            data: vec![0; elems * dtype.element_size()],
            shape: shape.iter().map(|v| *v as u32).collect(),
            dtype,
        }
    }
    fn elem_count(&self) -> usize {
        self.shape.iter().map(|v| *v as usize).product()
    }
    fn continuous_view(
        &mut self,
        elem_offset: usize,
        shape: Option<&[usize]>,
    ) -> anyhow::Result<simpler::ContinuousTensor> {
        let view_shape = shape
            .map(|shape| shape.iter().map(|v| *v as u32).collect::<Vec<_>>())
            .unwrap_or_else(|| self.shape.clone());
        let view_elems = view_shape.iter().map(|v| *v as usize).product::<usize>();
        let start = elem_offset
            .checked_mul(self.dtype.element_size())
            .ok_or_else(|| anyhow::anyhow!("tensor view offset overflow"))?;
        let bytes = view_elems
            .checked_mul(self.dtype.element_size())
            .ok_or_else(|| anyhow::anyhow!("tensor view byte size overflow"))?;
        if start
            .checked_add(bytes)
            .ok_or_else(|| anyhow::anyhow!("tensor view range overflow"))?
            > self.data.len()
        {
            anyhow::bail!("tensor view exceeds backing storage");
        }
        simpler::ContinuousTensor::from_shape(
            unsafe { self.data.as_mut_ptr().add(start) } as u64,
            &view_shape,
            self.dtype,
        )
        .map_err(|err| anyhow::anyhow!("failed to build simpler tensor arg: {err}"))
    }
}

struct ArgSpec<'a> {
    tensor: &'a mut TensorBuf,
    direction: simpler::ArgDirection,
    elem_offset: usize,
    shape: Option<Vec<usize>>,
}

struct PreparedArgs {
    task_args: simpler::ChipStorageTaskArgs,
}

fn make_args(specs: Vec<ArgSpec<'_>>) -> anyhow::Result<PreparedArgs> {
    let mut tensors = Vec::with_capacity(specs.len());
    let mut _directions = Vec::with_capacity(specs.len());
    for spec in specs {
        tensors.push(
            spec.tensor
                .continuous_view(spec.elem_offset, spec.shape.as_deref())?,
        );
        _directions.push(spec.direction);
    }
    let task_args = simpler::ChipStorageTaskArgs::new(&tensors, &[])
        .map_err(|err| anyhow::anyhow!("failed to build simpler task args: {err}"))?;
    Ok(PreparedArgs { task_args })
}

fn in_arg(tensor: &mut TensorBuf) -> ArgSpec<'_> {
    ArgSpec {
        tensor,
        direction: simpler::ArgDirection::In,
        elem_offset: 0,
        shape: None,
    }
}
fn out_arg(tensor: &mut TensorBuf) -> ArgSpec<'_> {
    ArgSpec {
        tensor,
        direction: simpler::ArgDirection::Out,
        elem_offset: 0,
        shape: None,
    }
}
fn inout_arg(tensor: &mut TensorBuf) -> ArgSpec<'_> {
    ArgSpec {
        tensor,
        direction: simpler::ArgDirection::Inout,
        elem_offset: 0,
        shape: None,
    }
}
fn in_arg_view<'a>(tensor: &'a mut TensorBuf, elem_offset: usize, shape: &[usize]) -> ArgSpec<'a> {
    ArgSpec {
        tensor,
        direction: simpler::ArgDirection::In,
        elem_offset,
        shape: Some(shape.to_vec()),
    }
}
fn inout_arg_view<'a>(
    tensor: &'a mut TensorBuf,
    elem_offset: usize,
    shape: &[usize],
) -> ArgSpec<'a> {
    ArgSpec {
        tensor,
        direction: simpler::ArgDirection::Inout,
        elem_offset,
        shape: Some(shape.to_vec()),
    }
}

fn prefill_args(t: &mut Qwen3SimplerTensors) -> anyhow::Result<PreparedArgs> {
    make_args(vec![
        in_arg(&mut t.prefill_hidden),
        in_arg(&mut t.prefill_seq_lens),
        in_arg(&mut t.input_rms_weight),
        in_arg(&mut t.wq),
        in_arg(&mut t.wk),
        in_arg(&mut t.wv),
        in_arg(&mut t.q_norm_weight),
        in_arg(&mut t.k_norm_weight),
        in_arg(&mut t.rope_cos),
        in_arg(&mut t.rope_sin),
        in_arg(&mut t.block_table),
        in_arg(&mut t.prefill_slot_mapping),
        inout_arg(&mut t.k_cache_all),
        inout_arg(&mut t.v_cache_all),
        in_arg(&mut t.wo),
        in_arg(&mut t.post_rms_weight),
        in_arg(&mut t.w_gate),
        in_arg(&mut t.w_up),
        in_arg(&mut t.w_down),
        out_arg(&mut t.prefill_out),
    ])
}

fn decode_args(t: &mut Qwen3SimplerTensors) -> anyhow::Result<PreparedArgs> {
    make_args(vec![
        in_arg(&mut t.decode_hidden),
        in_arg(&mut t.input_rms_weight),
        in_arg(&mut t.wq),
        in_arg(&mut t.wk),
        in_arg(&mut t.wv),
        in_arg(&mut t.q_norm_weight),
        in_arg(&mut t.k_norm_weight),
        in_arg(&mut t.decode_seq_lens),
        in_arg(&mut t.block_table),
        in_arg(&mut t.decode_slot_mapping),
        in_arg(&mut t.rope_cos),
        in_arg(&mut t.rope_sin),
        inout_arg(&mut t.k_cache_all),
        inout_arg(&mut t.v_cache_all),
        in_arg(&mut t.wo),
        in_arg(&mut t.post_rms_weight),
        in_arg(&mut t.w_gate),
        in_arg(&mut t.w_up),
        in_arg(&mut t.w_down),
        out_arg(&mut t.decode_out),
    ])
}

fn rms_lmhead_args(t: &mut Qwen3SimplerTensors) -> anyhow::Result<PreparedArgs> {
    make_args(vec![
        in_arg(&mut t.rms_x),
        in_arg(&mut t.final_norm_weight),
        in_arg(&mut t.lm_head_weight_t),
        out_arg(&mut t.rms_normed),
        out_arg(&mut t.logits_padded),
    ])
}

fn l2_prefill_args(t: &mut Qwen3SimplerTensors, layer: usize) -> anyhow::Result<PreparedArgs> {
    let max_seq = t.prefill_hidden.shape[1] as usize;
    let total_pages = RUNTIME_BATCH * max_seq.div_ceil(PAGE_SIZE);
    let layer_cache_rows = total_pages * NUM_KV_HEADS * PAGE_SIZE;
    make_args(vec![
        in_arg(&mut t.prefill_hidden),
        in_arg(&mut t.prefill_seq_lens),
        in_arg_view(&mut t.input_rms_weight, layer * HIDDEN, &[1, HIDDEN]),
        in_arg_view(&mut t.wq, layer * HIDDEN * Q_HIDDEN, &[HIDDEN, Q_HIDDEN]),
        in_arg_view(&mut t.wk, layer * HIDDEN * HIDDEN, &[HIDDEN, HIDDEN]),
        in_arg_view(&mut t.wv, layer * HIDDEN * HIDDEN, &[HIDDEN, HIDDEN]),
        in_arg_view(&mut t.q_norm_weight, layer * HEAD_DIM, &[1, HEAD_DIM]),
        in_arg_view(&mut t.k_norm_weight, layer * HEAD_DIM, &[1, HEAD_DIM]),
        in_arg(&mut t.rope_cos),
        in_arg(&mut t.rope_sin),
        in_arg(&mut t.block_table),
        in_arg(&mut t.prefill_slot_mapping),
        inout_arg_view(
            &mut t.k_cache_all,
            layer * layer_cache_rows * HEAD_DIM,
            &[layer_cache_rows, HEAD_DIM],
        ),
        inout_arg_view(
            &mut t.v_cache_all,
            layer * layer_cache_rows * HEAD_DIM,
            &[layer_cache_rows, HEAD_DIM],
        ),
        in_arg_view(&mut t.wo, layer * Q_HIDDEN * HIDDEN, &[Q_HIDDEN, HIDDEN]),
        in_arg_view(&mut t.post_rms_weight, layer * HIDDEN, &[1, HIDDEN]),
        in_arg_view(&mut t.w_gate, layer * HIDDEN * INTER, &[HIDDEN, INTER]),
        in_arg_view(&mut t.w_up, layer * HIDDEN * INTER, &[HIDDEN, INTER]),
        in_arg_view(&mut t.w_down, layer * INTER * HIDDEN, &[INTER, HIDDEN]),
        out_arg(&mut t.prefill_out),
    ])
}

fn final_rms_args(t: &mut Qwen3SimplerTensors) -> anyhow::Result<PreparedArgs> {
    make_args(vec![
        in_arg(&mut t.rms_x),
        in_arg(&mut t.final_norm_weight),
        out_arg(&mut t.rms_normed),
    ])
}

fn lm_head_args(t: &mut Qwen3SimplerTensors) -> anyhow::Result<PreparedArgs> {
    make_args(vec![
        in_arg(&mut t.rms_normed),
        in_arg(&mut t.lm_head_weight_t),
        out_arg(&mut t.logits_padded),
    ])
}

fn stack_norm(
    tensors: &BTreeMap<String, Qwen3DenseReferenceWeightTensorMetadata>,
    suffix: &str,
    width: usize,
) -> anyhow::Result<TensorBuf> {
    let mut out = TensorBuf::zero_f32(&[NUM_LAYERS, width]);
    for layer in 0..NUM_LAYERS {
        let name = format!("model.layers.{layer}.{suffix}");
        let values = full_tensor_as_f32(tensors, &name, width)?;
        write_f32(&mut out.data, layer * width, &values);
    }
    Ok(out)
}

fn stack_optional_norm(
    tensors: &BTreeMap<String, Qwen3DenseReferenceWeightTensorMetadata>,
    suffix: &str,
    width: usize,
) -> anyhow::Result<TensorBuf> {
    let mut out = TensorBuf::zero_f32(&[NUM_LAYERS, width]);
    for layer in 0..NUM_LAYERS {
        let name = format!("model.layers.{layer}.{suffix}");
        let values = if tensors.contains_key(&name) {
            full_tensor_as_f32(tensors, &name, width)?
        } else {
            vec![1.0; width]
        };
        write_f32(&mut out.data, layer * width, &values);
    }
    Ok(out)
}

fn stack_transposed(
    tensors: &BTreeMap<String, Qwen3DenseReferenceWeightTensorMetadata>,
    suffix: &str,
    in_dim: usize,
    out_dim: usize,
) -> anyhow::Result<TensorBuf> {
    let mut out = TensorBuf::zero_bf16(&[NUM_LAYERS * in_dim, out_dim]);
    for layer in 0..NUM_LAYERS {
        let name = format!("model.layers.{layer}.{suffix}");
        let tensor = tensors
            .get(&name)
            .ok_or_else(|| anyhow::anyhow!("missing weight tensor {name}"))?;
        let source_rows = out_dim;
        let source_cols = in_dim;
        if tensor.shape != vec![source_rows as u64, source_cols as u64] {
            anyhow::bail!(
                "weight shape mismatch for {name}: got {:?}, expected [{source_rows}, {source_cols}]",
                tensor.shape
            );
        }
        let values = full_tensor_as_f32(tensors, &name, source_rows * source_cols)?;
        for i in 0..in_dim {
            for o in 0..out_dim {
                let value = values[o * in_dim + i];
                let dst_elem = (layer * in_dim + i) * out_dim + o;
                write_bf16(&mut out.data, dst_elem, f32_to_bf16(value));
            }
        }
    }
    Ok(out)
}

fn full_norm(
    tensors: &BTreeMap<String, Qwen3DenseReferenceWeightTensorMetadata>,
    name: &str,
    width: usize,
) -> anyhow::Result<TensorBuf> {
    let mut out = TensorBuf::zero_f32(&[1, width]);
    let values = full_tensor_as_f32(tensors, name, width)?;
    write_f32(&mut out.data, 0, &values);
    Ok(out)
}

fn lm_head_weight(
    tensors: &BTreeMap<String, Qwen3DenseReferenceWeightTensorMetadata>,
) -> anyhow::Result<TensorBuf> {
    let name = if tensors.contains_key("lm_head.weight") {
        "lm_head.weight"
    } else {
        "model.embed_tokens.weight"
    };
    let tensor = tensors
        .get(name)
        .ok_or_else(|| anyhow::anyhow!("missing lm head tensor {name}"))?;
    if tensor.shape != vec![VOCAB_SIZE as u64, HIDDEN as u64] {
        anyhow::bail!(
            "lm head shape mismatch for {name}: got {:?}, expected [{VOCAB_SIZE}, {HIDDEN}]",
            tensor.shape
        );
    }
    let mut out = TensorBuf::zero_bf16(&[PADDED_VOCAB, HIDDEN]);
    let values = full_tensor_as_f32(tensors, name, VOCAB_SIZE * HIDDEN)?;
    for (idx, value) in values.iter().copied().enumerate() {
        write_bf16(&mut out.data, idx, f32_to_bf16(value));
    }
    Ok(out)
}

fn full_tensor_as_f32(
    tensors: &BTreeMap<String, Qwen3DenseReferenceWeightTensorMetadata>,
    name: &str,
    expected_elems: usize,
) -> anyhow::Result<Vec<f32>> {
    let tensor = tensors
        .get(name)
        .ok_or_else(|| anyhow::anyhow!("missing weight tensor {name}"))?;
    let payload = materialize_full_weight_tensor_payload(name, tensors)
        .map_err(anyhow::Error::msg)
        .with_context(|| format!("failed to materialize {name}"))?;
    decode_payload_as_f32(tensor.dtype, &payload, expected_elems)
        .with_context(|| format!("failed to decode {name}"))
}

fn decode_payload_as_f32(
    dtype: Qwen3DenseReferenceWeightDType,
    payload: &[u8],
    expected_elems: usize,
) -> anyhow::Result<Vec<f32>> {
    let elem_bytes = match dtype {
        Qwen3DenseReferenceWeightDType::F32 => 4,
        Qwen3DenseReferenceWeightDType::F16 | Qwen3DenseReferenceWeightDType::BF16 => 2,
        Qwen3DenseReferenceWeightDType::I8 | Qwen3DenseReferenceWeightDType::U8 => 1,
    };
    if payload.len() != expected_elems * elem_bytes {
        anyhow::bail!(
            "payload length mismatch: got {}, expected {}",
            payload.len(),
            expected_elems * elem_bytes
        );
    }
    let mut out = Vec::with_capacity(expected_elems);
    for idx in 0..expected_elems {
        let base = idx * elem_bytes;
        let value = match dtype {
            Qwen3DenseReferenceWeightDType::F32 => {
                f32::from_le_bytes(payload[base..base + 4].try_into().expect("f32 payload"))
            }
            Qwen3DenseReferenceWeightDType::BF16 => f32::from_bits(
                (u16::from_le_bytes(payload[base..base + 2].try_into().unwrap()) as u32) << 16,
            ),
            Qwen3DenseReferenceWeightDType::F16 => f16_to_f32(u16::from_le_bytes(
                payload[base..base + 2].try_into().unwrap(),
            )),
            Qwen3DenseReferenceWeightDType::I8 => payload[base] as i8 as f32,
            Qwen3DenseReferenceWeightDType::U8 => payload[base] as f32,
        };
        out.push(value);
    }
    Ok(out)
}

fn rope_tables(max_seq: usize, head_dim: usize, theta: f32) -> (TensorBuf, TensorBuf) {
    let half = head_dim / 2;
    let mut cos = TensorBuf::zero_f32(&[max_seq, head_dim]);
    let mut sin = TensorBuf::zero_f32(&[max_seq, head_dim]);
    for pos in 0..max_seq {
        for i in 0..half {
            let inv_freq = 1.0f32 / theta.powf(i as f32 / half as f32);
            let freq = pos as f32 * inv_freq;
            let c = freq.cos();
            let s = freq.sin();
            write_f32(&mut cos.data, pos * head_dim + i, &[c]);
            write_f32(&mut cos.data, pos * head_dim + half + i, &[c]);
            write_f32(&mut sin.data, pos * head_dim + i, &[s]);
            write_f32(&mut sin.data, pos * head_dim + half + i, &[s]);
        }
    }
    (cos, sin)
}

fn decode_token_text(model_dir: &Path, token_ids: &[u64]) -> anyhow::Result<String> {
    let mut bytes = Vec::new();
    for token in token_ids {
        let piece = token_piece_bytes_from_tokenizer_path(model_dir, *token)
            .map_err(anyhow::Error::msg)
            .with_context(|| format!("failed to decode token {token}"))?;
        bytes.extend_from_slice(&token_piece_decode_bytes(&piece));
    }
    Ok(String::from_utf8_lossy(&bytes).into_owned())
}

fn parse_function_name(config: &str) -> Option<String> {
    config
        .lines()
        .find_map(|line| line.split_once("\"function_name\""))
        .and_then(|(_, rest)| rest.split_once('"'))
        .and_then(|(_, rest)| rest.split_once('"'))
        .map(|(value, _)| value.to_string())
}

fn parse_runtime_config_u32(config: &str, key: &str) -> Option<u32> {
    config
        .lines()
        .find(|line| line.contains(&format!("\"{key}\"")) || line.contains(&format!("'{key}'")))
        .and_then(|line| line.split(':').nth(1))
        .and_then(|rest| rest.split(',').next())
        .and_then(|value| value.trim().parse::<u32>().ok())
}

fn parse_runtime_config_string(config: &str, key: &str) -> Option<String> {
    config
        .lines()
        .find(|line| line.contains(&format!("\"{key}\"")) || line.contains(&format!("'{key}'")))
        .and_then(|line| line.split(':').nth(1))
        .and_then(|rest| rest.split(',').next())
        .map(|value| {
            value
                .trim()
                .trim_matches('"')
                .trim_matches('\'')
                .to_string()
        })
        .filter(|value| !value.is_empty())
}

fn common_runtime_name<'a, I>(runtime_names: I) -> anyhow::Result<String>
where
    I: IntoIterator<Item = &'a String>,
{
    let mut iter = runtime_names.into_iter();
    let first = iter
        .next()
        .ok_or_else(|| anyhow::anyhow!("no runtime names to validate"))?;
    for runtime_name in iter {
        if runtime_name != first {
            anyhow::bail!("Qwen3 simpler programs use mixed runtimes: {first} and {runtime_name}");
        }
    }
    Ok(first.clone())
}

fn find_orchestration_so(root: &Path) -> anyhow::Result<PathBuf> {
    let orch_dir = root.join("orchestration");
    let mut matches = fs::read_dir(&orch_dir)
        .with_context(|| format!("failed to read {}", orch_dir.display()))?
        .filter_map(Result::ok)
        .map(|entry| entry.path())
        .filter(|path| path.extension().and_then(|ext| ext.to_str()) == Some("so"))
        .collect::<Vec<_>>();
    matches.sort();
    match matches.as_slice() {
        [path] => Ok(path.clone()),
        [] => anyhow::bail!("no orchestration .so found in {}", orch_dir.display()),
        _ => anyhow::bail!(
            "multiple orchestration .so files found in {}",
            orch_dir.display()
        ),
    }
}

fn parse_kernels(root: &Path, config: &str) -> anyhow::Result<Vec<KernelArtifact>> {
    let mut kernels = Vec::new();
    for line in config.lines() {
        if !line.contains("\"func_id\"") || !line.contains("\"name\"") {
            continue;
        }
        let func_id = line
            .split("\"func_id\"")
            .nth(1)
            .and_then(|rest| rest.split(':').nth(1))
            .and_then(|rest| rest.split(',').next())
            .and_then(|value| value.trim().parse::<i32>().ok())
            .ok_or_else(|| anyhow::anyhow!("failed to parse kernel func_id from {line}"))?;
        let name = line
            .split("\"name\"")
            .nth(1)
            .and_then(|rest| rest.split_once('"'))
            .and_then(|(_, rest)| rest.split_once('"'))
            .map(|(value, _)| value.to_string())
            .ok_or_else(|| anyhow::anyhow!("failed to parse kernel name from {line}"))?;
        let core_type = line
            .split("\"core_type\"")
            .nth(1)
            .and_then(|rest| rest.split_once('"'))
            .and_then(|(_, rest)| rest.split_once('"'))
            .map(|(value, _)| value.to_string());
        let mut candidates = Vec::new();
        if let Some(core_type) = core_type.as_deref() {
            candidates.push(
                root.join("kernels")
                    .join(core_type)
                    .join(format!("{name}.o")),
            );
        }
        candidates.push(root.join("kernels").join("aic").join(format!("{name}.o")));
        candidates.push(root.join("kernels").join("aiv").join(format!("{name}.o")));
        let path = candidates
            .into_iter()
            .find(|path| path.is_file())
            .ok_or_else(|| {
                anyhow::anyhow!(
                    "compiled kernel object for {name} not found under {}/kernels",
                    root.display()
                )
            })?;
        kernels.push(KernelArtifact { func_id, path });
    }
    kernels.sort_by_key(|kernel| kernel.func_id);
    Ok(kernels)
}

fn load_kernel_binary(path: &Path) -> anyhow::Result<Vec<u8>> {
    let bytes = fs::read(path).with_context(|| format!("failed to read {}", path.display()))?;
    if path.extension().and_then(|ext| ext.to_str()) == Some("o") {
        extract_elf64_text_section(&bytes)
            .with_context(|| format!("failed to extract .text from {}", path.display()))
    } else {
        Ok(bytes)
    }
}

fn extract_elf64_text_section(bytes: &[u8]) -> anyhow::Result<Vec<u8>> {
    if bytes.len() < 64 || &bytes[..4] != b"\x7fELF" {
        anyhow::bail!("not an ELF64 object");
    }
    if bytes[4] != 2 || bytes[5] != 1 {
        anyhow::bail!("only little-endian ELF64 objects are supported");
    }
    let e_shoff = read_le_u64_at(bytes, 40)? as usize;
    let e_shentsize = read_le_u16_at(bytes, 58)? as usize;
    let e_shnum = read_le_u16_at(bytes, 60)? as usize;
    let e_shstrndx = read_le_u16_at(bytes, 62)? as usize;
    if e_shentsize < 64 || e_shstrndx >= e_shnum {
        anyhow::bail!("invalid ELF64 section header metadata");
    }
    let str_header = checked_range(e_shoff, e_shstrndx * e_shentsize, e_shentsize)?;
    let str_offset = read_le_u64_at(bytes, str_header.start + 24)? as usize;
    let str_size = read_le_u64_at(bytes, str_header.start + 32)? as usize;
    let str_range = checked_range(str_offset, 0, str_size)?;
    let strtab = bytes
        .get(str_range)
        .ok_or_else(|| anyhow::anyhow!("ELF64 section string table is out of bounds"))?;

    for section in 0..e_shnum {
        let header = checked_range(e_shoff, section * e_shentsize, e_shentsize)?;
        let sh_name = read_le_u32_at(bytes, header.start)? as usize;
        let sh_offset = read_le_u64_at(bytes, header.start + 24)? as usize;
        let sh_size = read_le_u64_at(bytes, header.start + 32)? as usize;
        if cstr_at(strtab, sh_name)? == ".text" {
            let text_range = checked_range(sh_offset, 0, sh_size)?;
            return bytes
                .get(text_range)
                .map(<[u8]>::to_vec)
                .ok_or_else(|| anyhow::anyhow!("ELF64 .text section is out of bounds"));
        }
    }
    anyhow::bail!("ELF64 .text section not found")
}

fn checked_range(base: usize, offset: usize, len: usize) -> anyhow::Result<std::ops::Range<usize>> {
    let start = base
        .checked_add(offset)
        .ok_or_else(|| anyhow::anyhow!("range start overflow"))?;
    let end = start
        .checked_add(len)
        .ok_or_else(|| anyhow::anyhow!("range end overflow"))?;
    Ok(start..end)
}

fn read_le_u16_at(bytes: &[u8], offset: usize) -> anyhow::Result<u16> {
    let data = bytes
        .get(offset..offset + 2)
        .ok_or_else(|| anyhow::anyhow!("u16 read out of bounds at {offset}"))?;
    Ok(u16::from_le_bytes([data[0], data[1]]))
}

fn read_le_u32_at(bytes: &[u8], offset: usize) -> anyhow::Result<u32> {
    let data = bytes
        .get(offset..offset + 4)
        .ok_or_else(|| anyhow::anyhow!("u32 read out of bounds at {offset}"))?;
    Ok(u32::from_le_bytes([data[0], data[1], data[2], data[3]]))
}

fn read_le_u64_at(bytes: &[u8], offset: usize) -> anyhow::Result<u64> {
    let data = bytes
        .get(offset..offset + 8)
        .ok_or_else(|| anyhow::anyhow!("u64 read out of bounds at {offset}"))?;
    Ok(u64::from_le_bytes([
        data[0], data[1], data[2], data[3], data[4], data[5], data[6], data[7],
    ]))
}

fn cstr_at(bytes: &[u8], offset: usize) -> anyhow::Result<&str> {
    let tail = bytes
        .get(offset..)
        .ok_or_else(|| anyhow::anyhow!("string offset {offset} out of bounds"))?;
    let end = tail
        .iter()
        .position(|byte| *byte == 0)
        .unwrap_or(tail.len());
    std::str::from_utf8(&tail[..end]).context("ELF64 section name is not UTF-8")
}

fn artifact_source(runtime: &serde_json::Value, key: &str) -> anyhow::Result<PathBuf> {
    runtime
        .get(key)
        .and_then(|v| v.get("source"))
        .and_then(serde_json::Value::as_str)
        .map(PathBuf::from)
        .ok_or_else(|| anyhow::anyhow!("runtime manifest missing {key}.source"))
}

fn runtime_env(runtime: &serde_json::Value) -> anyhow::Result<BTreeMap<String, String>> {
    let mut out = BTreeMap::new();
    if let Some(value) = runtime.get("runtime_env") {
        let object = value
            .as_object()
            .ok_or_else(|| anyhow::anyhow!("runtime_env must be an object"))?;
        for (key, value) in object {
            let value = value
                .as_str()
                .ok_or_else(|| anyhow::anyhow!("runtime_env.{key} must be a string"))?;
            out.insert(key.clone(), value.to_string());
        }
    }
    Ok(out)
}

fn require_file(path: &Path) -> anyhow::Result<()> {
    if !path.is_file() {
        anyhow::bail!("required file is missing: {}", path.display());
    }
    Ok(())
}

fn next_value<I>(pending: &mut std::iter::Peekable<I>, name: &str) -> anyhow::Result<String>
where
    I: Iterator<Item = std::ffi::OsString>,
{
    pending
        .next()
        .map(|value| value.to_string_lossy().to_string())
        .ok_or_else(|| anyhow::anyhow!("{name} requires a value"))
}

fn parse_positive_usize(name: &str, value: &str) -> anyhow::Result<usize> {
    let parsed = value
        .parse::<usize>()
        .with_context(|| format!("{name} must be a positive integer"))?;
    if parsed == 0 {
        anyhow::bail!("{name} must be > 0");
    }
    Ok(parsed)
}

fn parse_u32(name: &str, value: &str) -> anyhow::Result<u32> {
    value
        .parse::<u32>()
        .with_context(|| format!("{name} must be a non-negative integer"))
}

fn write_i32(data: &mut [u8], index: usize, value: i32) {
    let base = index * 4;
    data[base..base + 4].copy_from_slice(&value.to_le_bytes());
}

fn write_f32(data: &mut [u8], start_index: usize, values: &[f32]) {
    for (offset, value) in values.iter().copied().enumerate() {
        let base = (start_index + offset) * 4;
        data[base..base + 4].copy_from_slice(&value.to_le_bytes());
    }
}

fn read_f32(data: &[u8], index: usize) -> f32 {
    let base = index * 4;
    f32::from_le_bytes(data[base..base + 4].try_into().expect("f32 bytes"))
}

fn write_f32_as_bf16(data: &mut [u8], start_index: usize, values: &[f32]) {
    for (offset, value) in values.iter().copied().enumerate() {
        write_bf16(data, start_index + offset, f32_to_bf16(value));
    }
}

fn write_bf16(data: &mut [u8], index: usize, value: u16) {
    let base = index * 2;
    data[base..base + 2].copy_from_slice(&value.to_le_bytes());
}

fn f32_to_bf16(value: f32) -> u16 {
    (value.to_bits() >> 16) as u16
}

fn f16_to_f32(bits: u16) -> f32 {
    let sign = ((bits & 0x8000) as u32) << 16;
    let exp = (bits & 0x7c00) >> 10;
    let frac = (bits & 0x03ff) as u32;
    let out = if exp == 0 {
        if frac == 0 {
            sign
        } else {
            let mut frac = frac;
            let mut exp = -14i32;
            while (frac & 0x0400) == 0 {
                frac <<= 1;
                exp -= 1;
            }
            frac &= 0x03ff;
            sign | (((exp + 127) as u32) << 23) | (frac << 13)
        }
    } else if exp == 0x1f {
        sign | 0x7f80_0000 | (frac << 13)
    } else {
        sign | (((exp as i32 - 15 + 127) as u32) << 23) | (frac << 13)
    };
    f32::from_bits(out)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn qwen3_simpler_args_accept_l2_build_outputs() {
        let args = args_from([
            "qwen3-simpler-generate",
            "/tmp/Qwen306BPrefillProgram_1",
            "--build-output",
            "/tmp/Qwen3Decode_1",
            "--build-output=/tmp/Qwen3FinalRMS_1",
            "/tmp/Qwen3LMHead_1",
            "--model-dir=/models/qwen",
            "--prompt",
            "Huawei is",
            "--max-seq-len",
            "512",
            "--max-new-tokens=10",
            "--platform",
            "a2a3",
            "--device-id=0",
            "--profile-verbose",
        ])
        .expect("parse args")
        .expect("some args");

        assert_eq!(args.build_outputs.len(), 4);
        assert!(!args.l3);
        assert_eq!(args.model_dir, PathBuf::from("/models/qwen"));
        assert_eq!(args.prompt, "Huawei is");
        assert_eq!(args.max_seq_len, 512);
        assert_eq!(args.max_new_tokens, 10);
        assert_eq!(args.platform, "a2a3");
        assert_eq!(args.device_id, 0);
        assert!(args.profile_verbose);
    }

    #[test]
    fn qwen3_simpler_l3_alias_accepts_named_options() {
        let args = args_from([
            "qwen3-simpler-l3-generate",
            "--build-output",
            "/tmp/Qwen3GenChunked_1",
            "--model-dir=/models/qwen",
            "--prompt",
            "Huawei is",
            "--max-seq-len",
            "512",
            "--max-new-tokens=10",
            "--platform",
            "a2a3",
            "--device-id=0",
            "--profile-verbose",
        ])
        .expect("parse args")
        .expect("some args");

        assert_eq!(
            args.build_outputs,
            vec![PathBuf::from("/tmp/Qwen3GenChunked_1")]
        );
        assert!(args.l3);
        assert_eq!(args.model_dir, PathBuf::from("/models/qwen"));
        assert_eq!(args.prompt, "Huawei is");
        assert_eq!(args.max_seq_len, 512);
        assert_eq!(args.max_new_tokens, 10);
        assert_eq!(args.platform, "a2a3");
        assert_eq!(args.device_id, 0);
        assert!(args.profile_verbose);
    }

    #[test]
    fn qwen3_simpler_rejects_missing_required_args() {
        let err = args_from(["qwen3-simpler-generate", "--prompt", "x"])
            .expect_err("missing build-output/model-dir");
        assert!(err.to_string().contains("build_output"));
    }

    #[test]
    fn qwen3_simpler_bf16_roundtrip_shape_arg() {
        let mut tensor = TensorBuf::zero_bf16(&[2, 3, 4]);
        let continuous = tensor.continuous_view(0, None).expect("continuous tensor");
        let _ = continuous;
    }
}
