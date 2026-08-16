//! Host-native DeepSeek V4 Flash adapter backed by DwarfStar's public C API.
//!
//! The adapter intentionally delegates model semantics to ds4. The simulator
//! owns topology and transport; ds4 owns tokenizer, layer math, MoE, KV state,
//! output head, and token text.

use serde::Serialize;
use std::ffi::{CStr, CString, OsStr};
use std::fs;
use std::io::Write;
use std::os::raw::{c_char, c_float, c_int, c_void};
use std::path::{Path, PathBuf};
use std::process::Command;
use std::ptr;

#[cfg(target_os = "macos")]
const DS4_HOST_BACKEND: c_int = 0;
#[cfg(not(target_os = "macos"))]
const DS4_HOST_BACKEND: c_int = 1;
const DS4_THINK_NONE: c_int = 0;

#[repr(C)]
#[derive(Default)]
struct Ds4Tokens {
    values: *mut c_int,
    len: c_int,
    cap: c_int,
}

#[repr(C)]
#[derive(Clone, Copy, Default)]
struct Ds4TokenScore {
    id: c_int,
    logit: c_float,
    logprob: c_float,
}

#[repr(C)]
#[derive(Clone, Copy, Default)]
struct Ds4DistributedLayers {
    start: u32,
    end: u32,
    has_output: bool,
    set: bool,
}

#[repr(C)]
#[derive(Clone, Copy, Default)]
struct Ds4DistributedOptions {
    role: c_int,
    layers: Ds4DistributedLayers,
    listen_host: *const c_char,
    listen_port: c_int,
    coordinator_host: *const c_char,
    coordinator_port: c_int,
    prefill_chunk: u32,
    prefill_window: u32,
    activation_bits: u32,
    replay_check: bool,
    debug: bool,
}

#[repr(C)]
#[derive(Clone, Copy, Default)]
struct Ds4EngineOptions {
    model_path: *const c_char,
    mtp_path: *const c_char,
    backend: c_int,
    n_threads: c_int,
    prefill_chunk: u32,
    mtp_draft_tokens: c_int,
    mtp_margin: c_float,
    directional_steering_file: *const c_char,
    expert_profile_path: *const c_char,
    directional_steering_attn: c_float,
    directional_steering_ffn: c_float,
    power_percent: c_int,
    ssd_streaming_cache_experts: u32,
    ssd_streaming_cache_bytes: u64,
    ssd_streaming_preload_experts: u32,
    simulate_used_memory_bytes: u64,
    warm_weights: bool,
    quality: bool,
    ssd_streaming: bool,
    ssd_streaming_cold: bool,
    inspect_only: bool,
    load_slice: bool,
    load_layer_start: u32,
    load_layer_end: u32,
    load_output: bool,
    distributed: Ds4DistributedOptions,
}

type EngineOpen = unsafe extern "C" fn(*mut *mut c_void, *const Ds4EngineOptions) -> c_int;
type EngineClose = unsafe extern "C" fn(*mut c_void);
type EngineVocabSize = unsafe extern "C" fn(*mut c_void) -> c_int;
type EngineHiddenF32Values = unsafe extern "C" fn(*mut c_void) -> u64;
type EncodeChatPrompt =
    unsafe extern "C" fn(*mut c_void, *const c_char, *const c_char, c_int, *mut Ds4Tokens);
type TokensFree = unsafe extern "C" fn(*mut Ds4Tokens);
type SessionCreate = unsafe extern "C" fn(*mut *mut c_void, *mut c_void, c_int) -> c_int;
type SessionFree = unsafe extern "C" fn(*mut c_void);
type SessionSync = unsafe extern "C" fn(*mut c_void, *const Ds4Tokens, *mut c_char, usize) -> c_int;
type SessionArgmax = unsafe extern "C" fn(*mut c_void) -> c_int;
type SessionTopLogprobs = unsafe extern "C" fn(*mut c_void, *mut Ds4TokenScore, c_int) -> c_int;
type TokenText = unsafe extern "C" fn(*mut c_void, c_int, *mut usize) -> *mut c_char;
type SliceReset = unsafe extern "C" fn(*mut c_void, *mut c_char, usize) -> c_int;
type EvalLayerSlice = unsafe extern "C" fn(
    *mut c_void,
    *const c_int,
    u32,
    u32,
    u32,
    u32,
    *const c_float,
    *mut c_float,
    bool,
    *mut c_float,
    *mut c_char,
    usize,
) -> c_int;
type SessionLayerPayloadBytes = unsafe extern "C" fn(*mut c_void, u32, u32) -> u64;
type SessionSaveLayerPayload =
    unsafe extern "C" fn(*mut c_void, *mut libc::FILE, u32, u32, *mut c_char, usize) -> c_int;
type SessionLoadLayerPayload = unsafe extern "C" fn(
    *mut c_void,
    *mut libc::FILE,
    u64,
    *const c_int,
    u32,
    u32,
    u32,
    *mut c_char,
    usize,
) -> c_int;

struct Ds4Library {
    handle: *mut c_void,
    engine_open: EngineOpen,
    engine_close: EngineClose,
    engine_vocab_size: EngineVocabSize,
    engine_hidden_f32_values: EngineHiddenF32Values,
    encode_chat_prompt: EncodeChatPrompt,
    tokens_free: TokensFree,
    session_create: SessionCreate,
    session_free: SessionFree,
    session_sync: SessionSync,
    session_argmax: SessionArgmax,
    session_top_logprobs: SessionTopLogprobs,
    token_text: TokenText,
    slice_reset: SliceReset,
    eval_layer_slice: EvalLayerSlice,
    session_layer_payload_bytes: SessionLayerPayloadBytes,
    session_save_layer_payload: SessionSaveLayerPayload,
    session_load_layer_payload: SessionLoadLayerPayload,
}

impl Ds4Library {
    fn open(path: &Path) -> Result<Self, String> {
        let path = path_to_cstring(path)?;
        let handle = unsafe { libc::dlopen(path.as_ptr(), libc::RTLD_NOW | libc::RTLD_LOCAL) };
        if handle.is_null() {
            return Err(format!("ds4_library_open_failed:{}", dlerror()));
        }
        unsafe {
            Ok(Self {
                handle,
                engine_open: load_symbol(handle, b"ds4_engine_open\0")?,
                engine_close: load_symbol(handle, b"ds4_engine_close\0")?,
                engine_vocab_size: load_symbol(handle, b"ds4_engine_vocab_size\0")?,
                engine_hidden_f32_values: load_symbol(handle, b"ds4_engine_hidden_f32_values\0")?,
                encode_chat_prompt: load_symbol(handle, b"ds4_encode_chat_prompt\0")?,
                tokens_free: load_symbol(handle, b"ds4_tokens_free\0")?,
                session_create: load_symbol(handle, b"ds4_session_create\0")?,
                session_free: load_symbol(handle, b"ds4_session_free\0")?,
                session_sync: load_symbol(handle, b"ds4_session_sync\0")?,
                session_argmax: load_symbol(handle, b"ds4_session_argmax\0")?,
                session_top_logprobs: load_symbol(handle, b"ds4_session_top_logprobs\0")?,
                token_text: load_symbol(handle, b"ds4_token_text\0")?,
                slice_reset: load_symbol(handle, b"ds4_session_layer_slice_reset\0")?,
                eval_layer_slice: load_symbol(handle, b"ds4_session_eval_layer_slice\0")?,
                session_layer_payload_bytes: load_symbol(
                    handle,
                    b"ds4_session_layer_payload_bytes\0",
                )?,
                session_save_layer_payload: load_symbol(
                    handle,
                    b"ds4_session_save_layer_payload\0",
                )?,
                session_load_layer_payload: load_symbol(
                    handle,
                    b"ds4_session_load_layer_payload\0",
                )?,
            })
        }
    }
}

impl Drop for Ds4Library {
    fn drop(&mut self) {
        if !self.handle.is_null() {
            unsafe { libc::dlclose(self.handle) };
        }
    }
}

unsafe fn load_symbol<T: Copy>(handle: *mut c_void, name: &[u8]) -> Result<T, String> {
    libc::dlerror();
    let symbol = libc::dlsym(handle, name.as_ptr().cast());
    if symbol.is_null() {
        return Err(format!(
            "ds4_symbol_missing:{}:{}",
            String::from_utf8_lossy(&name[..name.len().saturating_sub(1)]),
            dlerror()
        ));
    }
    Ok(std::mem::transmute_copy(&symbol))
}

fn dlerror() -> String {
    let error = unsafe { libc::dlerror() };
    if error.is_null() {
        return "unknown".to_string();
    }
    unsafe { CStr::from_ptr(error) }
        .to_string_lossy()
        .into_owned()
}

fn path_to_cstring(path: &Path) -> Result<CString, String> {
    CString::new(path.as_os_str().to_string_lossy().as_bytes())
        .map_err(|_| format!("path_contains_nul:{}", path.display()))
}

struct Engine<'a> {
    library: &'a Ds4Library,
    ptr: *mut c_void,
}

impl Drop for Engine<'_> {
    fn drop(&mut self) {
        if !self.ptr.is_null() {
            unsafe { (self.library.engine_close)(self.ptr) };
        }
    }
}

struct Session<'a> {
    library: &'a Ds4Library,
    ptr: *mut c_void,
}

impl Drop for Session<'_> {
    fn drop(&mut self) {
        if !self.ptr.is_null() {
            unsafe { (self.library.session_free)(self.ptr) };
        }
    }
}

struct Tokens<'a> {
    library: &'a Ds4Library,
    raw: Ds4Tokens,
}

impl Tokens<'_> {
    fn as_slice(&self) -> Result<&[c_int], String> {
        if self.raw.len < 0 || (self.raw.len > 0 && self.raw.values.is_null()) {
            return Err("ds4_token_buffer_invalid".to_string());
        }
        Ok(unsafe { std::slice::from_raw_parts(self.raw.values, self.raw.len as usize) })
    }
}

impl Drop for Tokens<'_> {
    fn drop(&mut self) {
        unsafe { (self.library.tokens_free)(&mut self.raw) };
    }
}

#[derive(Debug, Clone, Serialize, PartialEq)]
pub struct Ds4TokenCandidate {
    pub id: i32,
    pub text: String,
    pub logit: f32,
    pub logprob: f32,
}

#[derive(Debug, Clone, Serialize, PartialEq)]
pub struct Ds4FirstTokenReport {
    pub model: String,
    pub prompt_tokens: usize,
    pub token: Ds4TokenCandidate,
    pub candidates: Vec<Ds4TokenCandidate>,
}

#[derive(Debug, Clone, Serialize, PartialEq)]
pub struct Ds4TokenizationReport {
    pub model: String,
    pub token_count: usize,
    pub token_ids: Vec<i32>,
}

#[derive(Debug, Clone, Serialize, PartialEq)]
pub struct Ds4SliceReport {
    pub model: String,
    pub layer_start: u32,
    pub layer_end: u32,
    pub position: u32,
    pub token_count: usize,
    pub output_kind: String,
    pub value_count: usize,
    pub hidden_value_count: usize,
    pub logits_count: usize,
    pub kv_payload_bytes: usize,
    pub output_path: String,
    pub selected_token: Option<Ds4TokenCandidate>,
    pub candidates: Vec<Ds4TokenCandidate>,
}

#[derive(Debug, Clone)]
pub struct Ds4SliceMemoryConfig {
    pub library_path: PathBuf,
    pub runtime_dir: PathBuf,
    pub model_path: PathBuf,
    pub context: i32,
    pub layer_start: u32,
    pub layer_end: u32,
    pub position: u32,
    pub token_ids: Vec<i32>,
    pub input_hidden: Option<Vec<f32>>,
    pub previous_token_ids: Vec<i32>,
    pub previous_kv: Option<Vec<u8>>,
    pub output_logits: bool,
}

#[derive(Debug, Clone)]
pub struct Ds4SliceOutput {
    pub report: Ds4SliceReport,
    pub hidden: Vec<f32>,
    pub logits: Vec<f32>,
    pub kv_payload: Vec<u8>,
}

#[derive(Debug, Clone)]
pub struct Ds4RunConfig {
    pub library_path: PathBuf,
    pub runtime_dir: PathBuf,
    pub model_path: PathBuf,
    pub prompt: String,
    pub system: String,
    pub context: i32,
    pub top_k: i32,
}

fn with_runtime_dir<T>(
    runtime_dir: &Path,
    run: impl FnOnce() -> Result<T, String>,
) -> Result<T, String> {
    let original = std::env::current_dir().map_err(|err| format!("current_dir_failed:{err}"))?;
    std::env::set_current_dir(runtime_dir)
        .map_err(|err| format!("ds4_runtime_dir_failed:{}:{err}", runtime_dir.display()))?;
    let result = run();
    let restore = std::env::set_current_dir(&original)
        .map_err(|err| format!("restore_current_dir_failed:{}:{err}", original.display()));
    match (result, restore) {
        (Ok(value), Ok(())) => Ok(value),
        (Err(err), _) => Err(err),
        (Ok(_), Err(err)) => Err(err),
    }
}

fn open_engine<'a>(
    library: &'a Ds4Library,
    model_path: &Path,
    layer_range: Option<(u32, u32)>,
    load_output: bool,
) -> Result<Engine<'a>, String> {
    let model_path = path_to_cstring(model_path)?;
    let mut options = Ds4EngineOptions {
        model_path: model_path.as_ptr(),
        backend: DS4_HOST_BACKEND,
        mtp_draft_tokens: 1,
        mtp_margin: 3.0,
        ..Ds4EngineOptions::default()
    };
    if let Some((start, end)) = layer_range {
        if start >= end || end > 43 {
            return Err(format!("ds4_layer_range_invalid:{start}:{end}"));
        }
        options.load_slice = true;
        options.load_layer_start = start;
        options.load_layer_end = end - 1;
        options.load_output = load_output;
    }
    let mut engine = ptr::null_mut();
    let rc = unsafe { (library.engine_open)(&mut engine, &options) };
    if rc != 0 || engine.is_null() {
        return Err(format!("ds4_engine_open_failed:rc={rc}"));
    }
    Ok(Engine {
        library,
        ptr: engine,
    })
}

fn create_session<'a>(engine: &'a Engine<'a>, context: i32) -> Result<Session<'a>, String> {
    if context <= 0 {
        return Err(format!("ds4_context_invalid:{context}"));
    }
    let mut session = ptr::null_mut();
    let rc = unsafe { (engine.library.session_create)(&mut session, engine.ptr, context) };
    if rc != 0 || session.is_null() {
        return Err(format!("ds4_session_create_failed:rc={rc}"));
    }
    Ok(Session {
        library: engine.library,
        ptr: session,
    })
}

fn tokenize_chat<'a>(
    library: &'a Ds4Library,
    engine: *mut c_void,
    system: &str,
    prompt: &str,
) -> Result<Tokens<'a>, String> {
    let system = CString::new(system).map_err(|_| "ds4_system_contains_nul".to_string())?;
    let prompt = CString::new(prompt).map_err(|_| "ds4_prompt_contains_nul".to_string())?;
    let mut raw = Ds4Tokens::default();
    unsafe {
        (library.encode_chat_prompt)(
            engine,
            system.as_ptr(),
            prompt.as_ptr(),
            DS4_THINK_NONE,
            &mut raw,
        )
    };
    let tokens = Tokens { library, raw };
    if tokens.as_slice()?.is_empty() {
        return Err("ds4_chat_tokenization_empty".to_string());
    }
    Ok(tokens)
}

fn token_text(library: &Ds4Library, engine: *mut c_void, token: i32) -> Result<String, String> {
    let mut len = 0usize;
    let text = unsafe { (library.token_text)(engine, token, &mut len) };
    if text.is_null() {
        return Err(format!("ds4_token_text_failed:{token}"));
    }
    let bytes = unsafe { std::slice::from_raw_parts(text.cast::<u8>(), len) };
    let result = String::from_utf8(bytes.to_vec())
        .map_err(|err| format!("ds4_token_text_utf8_failed:{token}:{err}"));
    unsafe { libc::free(text.cast()) };
    result
}

fn top_candidates(
    library: &Ds4Library,
    engine: *mut c_void,
    session: *mut c_void,
    top_k: i32,
) -> Result<Vec<Ds4TokenCandidate>, String> {
    let top_k = top_k.clamp(1, 64);
    let mut scores = vec![Ds4TokenScore::default(); top_k as usize];
    let count = unsafe { (library.session_top_logprobs)(session, scores.as_mut_ptr(), top_k) };
    if count <= 0 || count > top_k {
        return Err(format!("ds4_top_logprobs_failed:count={count}"));
    }
    scores
        .into_iter()
        .take(count as usize)
        .map(|score| {
            Ok(Ds4TokenCandidate {
                id: score.id,
                text: token_text(library, engine, score.id)?,
                logit: score.logit,
                logprob: score.logprob,
            })
        })
        .collect()
}

pub fn ds4_tokenize_chat(config: &Ds4RunConfig) -> Result<Ds4TokenizationReport, String> {
    with_runtime_dir(&config.runtime_dir, || {
        let library = Ds4Library::open(&config.library_path)?;
        let engine = open_engine(&library, &config.model_path, None, false)?;
        let tokens = tokenize_chat(&library, engine.ptr, &config.system, &config.prompt)?;
        let token_ids = tokens.as_slice()?.to_vec();
        Ok(Ds4TokenizationReport {
            model: config.model_path.display().to_string(),
            token_count: token_ids.len(),
            token_ids,
        })
    })
}

pub fn ds4_first_token(config: &Ds4RunConfig) -> Result<Ds4FirstTokenReport, String> {
    with_runtime_dir(&config.runtime_dir, || {
        let library = Ds4Library::open(&config.library_path)?;
        let engine = open_engine(&library, &config.model_path, None, false)?;
        let tokens = tokenize_chat(&library, engine.ptr, &config.system, &config.prompt)?;
        let session = create_session(&engine, config.context)?;
        let mut error = [0 as c_char; 512];
        let rc = unsafe {
            (library.session_sync)(session.ptr, &tokens.raw, error.as_mut_ptr(), error.len())
        };
        if rc != 0 {
            return Err(format!(
                "ds4_session_sync_failed:rc={rc}:{}",
                c_error(&error)
            ));
        }
        let selected_id = unsafe { (library.session_argmax)(session.ptr) };
        if selected_id < 0 {
            return Err(format!("ds4_session_argmax_failed:{selected_id}"));
        }
        let candidates = top_candidates(&library, engine.ptr, session.ptr, config.top_k)?;
        let token = candidates
            .iter()
            .find(|candidate| candidate.id == selected_id)
            .cloned()
            .unwrap_or(Ds4TokenCandidate {
                id: selected_id,
                text: token_text(&library, engine.ptr, selected_id)?,
                logit: f32::NAN,
                logprob: f32::NAN,
            });
        Ok(Ds4FirstTokenReport {
            model: config.model_path.display().to_string(),
            prompt_tokens: tokens.as_slice()?.len(),
            token,
            candidates,
        })
    })
}

#[derive(Debug, Clone)]
pub struct Ds4SliceConfig {
    pub library_path: PathBuf,
    pub runtime_dir: PathBuf,
    pub model_path: PathBuf,
    pub context: i32,
    pub layer_start: u32,
    pub layer_end: u32,
    pub position: u32,
    pub token_ids: Vec<i32>,
    pub input_path: Option<PathBuf>,
    pub output_path: PathBuf,
    pub output_logits: bool,
}

pub fn ds4_eval_layer_slice(config: &Ds4SliceConfig) -> Result<Ds4SliceReport, String> {
    let hidden_values = 16_384usize
        .checked_mul(config.token_ids.len())
        .ok_or_else(|| "ds4_slice_hidden_count_overflow".to_string())?;
    let input_hidden = match &config.input_path {
        Some(path) => Some(read_f32_file(path, hidden_values)?),
        None => None,
    };
    let output = ds4_eval_layer_slice_in_memory(&Ds4SliceMemoryConfig {
        library_path: config.library_path.clone(),
        runtime_dir: config.runtime_dir.clone(),
        model_path: config.model_path.clone(),
        context: config.context,
        layer_start: config.layer_start,
        layer_end: config.layer_end,
        position: config.position,
        token_ids: config.token_ids.clone(),
        input_hidden,
        previous_token_ids: Vec::new(),
        previous_kv: None,
        output_logits: config.output_logits,
    })?;
    let values = if config.output_logits {
        &output.logits
    } else {
        &output.hidden
    };
    write_f32_file(&config.output_path, values)?;
    let mut report = output.report;
    report.output_path = config.output_path.display().to_string();
    report.value_count = values.len();
    Ok(report)
}

pub fn ds4_eval_layer_slice_in_memory(
    config: &Ds4SliceMemoryConfig,
) -> Result<Ds4SliceOutput, String> {
    with_runtime_dir(&config.runtime_dir, || {
        if config.token_ids.is_empty() {
            return Err("ds4_slice_tokens_empty".to_string());
        }
        let library = Ds4Library::open(&config.library_path)?;
        let engine = open_engine(
            &library,
            &config.model_path,
            Some((config.layer_start, config.layer_end)),
            config.output_logits,
        )?;
        let session = create_session(&engine, config.context)?;
        let hidden_values = unsafe { (library.engine_hidden_f32_values)(engine.ptr) };
        let hidden_count = hidden_values
            .checked_mul(config.token_ids.len() as u64)
            .and_then(|count| usize::try_from(count).ok())
            .ok_or_else(|| "ds4_slice_hidden_count_overflow".to_string())?;
        let input = config.input_hidden.as_ref();
        if let Some(input) = input {
            if input.len() != hidden_count {
                return Err(format!(
                    "ds4_slice_input_hidden_count_mismatch:expected={hidden_count}:actual={}",
                    input.len()
                ));
            }
        } else if config.layer_start != 0 {
            return Err("ds4_slice_input_hidden_required".to_string());
        }
        if config.previous_kv.is_some() != !config.previous_token_ids.is_empty() {
            return Err("ds4_slice_previous_kv_token_state_mismatch".to_string());
        }
        let vocab_size = unsafe { (library.engine_vocab_size)(engine.ptr) };
        if vocab_size <= 0 {
            return Err(format!("ds4_vocab_size_invalid:{vocab_size}"));
        }
        let mut hidden_output = vec![0.0f32; hidden_count];
        let mut logits = if config.output_logits {
            vec![0.0f32; vocab_size as usize]
        } else {
            Vec::new()
        };
        let mut error = [0 as c_char; 512];
        let reset_rc =
            unsafe { (library.slice_reset)(session.ptr, error.as_mut_ptr(), error.len()) };
        if reset_rc != 0 {
            return Err(format!(
                "ds4_slice_reset_failed:rc={reset_rc}:{}",
                c_error(&error)
            ));
        }
        if let Some(previous_kv) = config.previous_kv.as_ref() {
            load_layer_payload(
                &library,
                session.ptr,
                previous_kv,
                &config.previous_token_ids,
                config.layer_start,
                config.layer_end - 1,
            )?;
        }
        error.fill(0);
        let rc = unsafe {
            (library.eval_layer_slice)(
                session.ptr,
                config.token_ids.as_ptr(),
                config.token_ids.len() as u32,
                config.position,
                config.layer_start,
                config.layer_end - 1,
                input.as_ref().map_or(ptr::null(), |values| values.as_ptr()),
                hidden_output.as_mut_ptr(),
                config.output_logits,
                if logits.is_empty() {
                    ptr::null_mut()
                } else {
                    logits.as_mut_ptr()
                },
                error.as_mut_ptr(),
                error.len(),
            )
        };
        if rc != 0 {
            return Err(format!("ds4_slice_eval_failed:rc={rc}:{}", c_error(&error)));
        }
        let candidates = if config.output_logits {
            top_raw_logit_candidates(&library, engine.ptr, &logits, 4)?
        } else {
            Vec::new()
        };
        let selected_token = candidates.first().cloned();
        let kv_payload = save_layer_payload(
            &library,
            session.ptr,
            config.layer_start,
            config.layer_end - 1,
        )?;
        Ok(Ds4SliceOutput {
            report: Ds4SliceReport {
                model: config.model_path.display().to_string(),
                layer_start: config.layer_start,
                layer_end: config.layer_end,
                position: config.position,
                token_count: config.token_ids.len(),
                output_kind: if config.output_logits {
                    "logits".to_string()
                } else {
                    "hidden_f32".to_string()
                },
                value_count: if config.output_logits {
                    logits.len()
                } else {
                    hidden_output.len()
                },
                hidden_value_count: hidden_output.len(),
                logits_count: logits.len(),
                kv_payload_bytes: kv_payload.len(),
                output_path: String::new(),
                selected_token,
                candidates,
            },
            hidden: hidden_output,
            logits,
            kv_payload,
        })
    })
}

fn top_raw_logit_candidates(
    library: &Ds4Library,
    engine: *mut c_void,
    logits: &[f32],
    count: usize,
) -> Result<Vec<Ds4TokenCandidate>, String> {
    let mut ranked: Vec<(usize, f32)> = logits.iter().copied().enumerate().collect();
    ranked.sort_unstable_by(|left, right| right.1.total_cmp(&left.1));
    ranked
        .into_iter()
        .take(count)
        .map(|(id, logit)| {
            Ok(Ds4TokenCandidate {
                id: id as i32,
                text: token_text(library, engine, id as i32)?,
                logit,
                logprob: f32::NAN,
            })
        })
        .collect()
}

struct TemporaryFile(*mut libc::FILE);

impl TemporaryFile {
    fn open() -> Result<Self, String> {
        let file = unsafe { libc::tmpfile() };
        if file.is_null() {
            return Err(format!(
                "ds4_slice_tmpfile_failed:{}",
                std::io::Error::last_os_error()
            ));
        }
        Ok(Self(file))
    }
}

impl Drop for TemporaryFile {
    fn drop(&mut self) {
        if !self.0.is_null() {
            unsafe { libc::fclose(self.0) };
        }
    }
}

fn save_layer_payload(
    library: &Ds4Library,
    session: *mut c_void,
    layer_start: u32,
    layer_end: u32,
) -> Result<Vec<u8>, String> {
    let payload_bytes =
        unsafe { (library.session_layer_payload_bytes)(session, layer_start, layer_end) };
    let payload_len =
        usize::try_from(payload_bytes).map_err(|_| "ds4_slice_kv_payload_too_large".to_string())?;
    if payload_len == 0 {
        return Err("ds4_slice_kv_payload_empty".to_string());
    }
    let file = TemporaryFile::open()?;
    let mut error = [0 as c_char; 512];
    let rc = unsafe {
        (library.session_save_layer_payload)(
            session,
            file.0,
            layer_start,
            layer_end,
            error.as_mut_ptr(),
            error.len(),
        )
    };
    if rc != 0 {
        return Err(format!(
            "ds4_slice_kv_save_failed:rc={rc}:{}",
            c_error(&error)
        ));
    }
    if unsafe { libc::fflush(file.0) } != 0
        || unsafe { libc::fseek(file.0, 0, libc::SEEK_SET) } != 0
    {
        return Err(format!(
            "ds4_slice_kv_rewind_failed:{}",
            std::io::Error::last_os_error()
        ));
    }
    let mut payload = vec![0u8; payload_len];
    let read = unsafe { libc::fread(payload.as_mut_ptr().cast(), 1, payload_len, file.0) };
    if read != payload_len {
        return Err(format!(
            "ds4_slice_kv_read_failed:expected={payload_len}:actual={read}"
        ));
    }
    Ok(payload)
}

fn load_layer_payload(
    library: &Ds4Library,
    session: *mut c_void,
    payload: &[u8],
    tokens: &[i32],
    layer_start: u32,
    layer_end: u32,
) -> Result<(), String> {
    let file = TemporaryFile::open()?;
    let written = unsafe { libc::fwrite(payload.as_ptr().cast(), 1, payload.len(), file.0) };
    if written != payload.len() || unsafe { libc::fseek(file.0, 0, libc::SEEK_SET) } != 0 {
        return Err(format!(
            "ds4_slice_kv_stage_failed:expected={}:actual={written}:{}",
            payload.len(),
            std::io::Error::last_os_error()
        ));
    }
    let mut error = [0 as c_char; 512];
    let token_count = u32::try_from(tokens.len())
        .map_err(|_| "ds4_slice_previous_token_count_too_large".to_string())?;
    let rc = unsafe {
        (library.session_load_layer_payload)(
            session,
            file.0,
            payload.len() as u64,
            tokens.as_ptr(),
            token_count,
            layer_start,
            layer_end,
            error.as_mut_ptr(),
            error.len(),
        )
    };
    if rc != 0 {
        return Err(format!(
            "ds4_slice_kv_load_failed:rc={rc}:{}",
            c_error(&error)
        ));
    }
    Ok(())
}

fn c_error(buffer: &[c_char]) -> String {
    let end = buffer
        .iter()
        .position(|byte| *byte == 0)
        .unwrap_or(buffer.len());
    let bytes: Vec<u8> = buffer[..end].iter().map(|byte| *byte as u8).collect();
    String::from_utf8_lossy(&bytes).into_owned()
}

fn read_f32_file(path: &Path, expected_count: usize) -> Result<Vec<f32>, String> {
    let bytes = fs::read(path)
        .map_err(|err| format!("ds4_slice_input_read_failed:{}:{err}", path.display()))?;
    let expected_bytes = expected_count
        .checked_mul(std::mem::size_of::<f32>())
        .ok_or_else(|| "ds4_slice_input_size_overflow".to_string())?;
    if bytes.len() != expected_bytes {
        return Err(format!(
            "ds4_slice_input_size_mismatch:{}:expected={expected_bytes}:actual={}",
            path.display(),
            bytes.len()
        ));
    }
    Ok(bytes
        .chunks_exact(4)
        .map(|chunk| f32::from_le_bytes([chunk[0], chunk[1], chunk[2], chunk[3]]))
        .collect())
}

fn write_f32_file(path: &Path, values: &[f32]) -> Result<(), String> {
    let mut file = fs::File::create(path)
        .map_err(|err| format!("ds4_slice_output_create_failed:{}:{err}", path.display()))?;
    for value in values {
        file.write_all(&value.to_le_bytes())
            .map_err(|err| format!("ds4_slice_output_write_failed:{}:{err}", path.display()))?;
    }
    file.flush()
        .map_err(|err| format!("ds4_slice_output_flush_failed:{}:{err}", path.display()))
}

pub fn build_ds4_dynamic_library(ds4_dir: &Path, output: &Path) -> Result<(), String> {
    if !ds4_dir.join("ds4.c").is_file() || !ds4_dir.join("ds4.h").is_file() {
        return Err(format!("ds4_source_dir_invalid:{}", ds4_dir.display()));
    }
    if !cfg!(target_os = "macos") {
        return Err("ds4_library_build_unsupported:linux_cuda_link_pending".to_string());
    }
    let make_status = Command::new("make")
        .current_dir(ds4_dir)
        .args(["ds4.o", "ds4_distributed.o", "ds4_ssd.o", "ds4_metal.o"])
        .status()
        .map_err(|err| format!("ds4_make_spawn_failed:{err}"))?;
    if !make_status.success() {
        return Err(format!("ds4_make_failed:{make_status}"));
    }
    if let Some(parent) = output.parent() {
        fs::create_dir_all(parent)
            .map_err(|err| format!("ds4_library_output_dir_failed:{}:{err}", parent.display()))?;
    }
    let mut command = Command::new("cc");
    command.current_dir(ds4_dir);
    if cfg!(target_os = "macos") {
        command.arg("-dynamiclib");
    } else {
        command.args(["-shared", "-fPIC"]);
    }
    command.arg("-o").arg(output);
    command.args([
        OsStr::new("ds4.o"),
        OsStr::new("ds4_distributed.o"),
        OsStr::new("ds4_ssd.o"),
        OsStr::new("ds4_metal.o"),
        OsStr::new("-lm"),
        OsStr::new("-pthread"),
    ]);
    if cfg!(target_os = "macos") {
        command.args([
            OsStr::new("-framework"),
            OsStr::new("Foundation"),
            OsStr::new("-framework"),
            OsStr::new("Metal"),
        ]);
    }
    let status = command
        .status()
        .map_err(|err| format!("ds4_library_link_spawn_failed:{err}"))?;
    if !status.success() {
        return Err(format!("ds4_library_link_failed:{status}"));
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn f32_payload_round_trips_and_rejects_wrong_size() {
        let path = std::env::temp_dir().join(format!(
            "deepseek_v4_flash_adapter_f32_{}",
            std::process::id()
        ));
        let values = [1.25f32, -2.5, 0.0, f32::INFINITY];
        write_f32_file(&path, &values).expect("write payload");
        assert_eq!(read_f32_file(&path, values.len()).unwrap(), values);
        assert!(read_f32_file(&path, values.len() + 1)
            .unwrap_err()
            .contains("size_mismatch"));
        let _ = fs::remove_file(path);
    }

    #[test]
    fn invalid_source_dir_fails_closed() {
        let missing = std::env::temp_dir().join("missing-ds4-source-dir");
        let output = std::env::temp_dir().join("missing-ds4-library");
        assert!(build_ds4_dynamic_library(&missing, &output)
            .unwrap_err()
            .contains("source_dir_invalid"));
    }
}
