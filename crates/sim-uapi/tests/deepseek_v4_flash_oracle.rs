use std::fs;
use std::path::{Path, PathBuf};
use std::process::Command;

use sim_models::deepseek_v4_flash_gguf::GgufCatalog;

const PROMPT_TOKENS: [usize; 21] = [
    0, 128803, 52, 278, 4908, 75, 295, 89034, 399, 3919, 123003, 28, 23442, 9861, 63030, 47121,
    317, 805, 33, 128804, 128822,
];
const HIDDEN_BYTES: u64 = PROMPT_TOKENS.len() as u64 * 16_384 * 4;
const LAYER_RANGES: [(u64, u64); 8] = [
    (0, 6),
    (6, 12),
    (12, 18),
    (18, 23),
    (23, 28),
    (28, 33),
    (33, 38),
    (38, 43),
];

#[test]
#[ignore = "requires the sibling ds4 GGUF and native simpler runtime"]
fn process_isolated_eight_range_pipeline_matches_fixed_oracle() {
    let Some(model) = sibling_gguf_path() else {
        return;
    };
    let root = repository_root();
    let model_bytes = fs::metadata(&model).expect("read model metadata").len();
    let run_dir =
        std::env::temp_dir().join(format!("ub-sim-deepseek-v4-flash-oracle-{model_bytes}"));
    fs::create_dir_all(&run_dir).expect("create oracle run directory");
    let tokens = PROMPT_TOKENS
        .iter()
        .map(usize::to_string)
        .collect::<Vec<_>>()
        .join(",");
    let mut previous_hidden: Option<PathBuf> = None;
    let logits_path = run_dir.join("logits.f32");

    for (node, (layer_start, layer_end)) in LAYER_RANGES.iter().copied().enumerate() {
        let hidden_path = run_dir.join(format!("hidden-{node}.f32"));
        let terminal = node + 1 == LAYER_RANGES.len();
        if completed_payload(&hidden_path, HIDDEN_BYTES)
            && (!terminal
                || completed_payload(
                    &logits_path,
                    sim_models::deepseek_v4_flash::DEEPSEEK_V4_FLASH_PROFILE.vocab_size * 4,
                ))
        {
            eprintln!("oracle range resume: node={node} layers=[{layer_start},{layer_end})");
            previous_hidden = Some(hidden_path);
            continue;
        }
        eprintln!("oracle range start: node={node} layers=[{layer_start},{layer_end})");
        let layers = format!("{layer_start}:{layer_end}");
        let scenario = root.join("scenarios/mvp_2host_single_domain.yaml");
        let mut command = Command::new(env!("CARGO_BIN_EXE_deepseek_v4_flash_range"));
        command
            .current_dir(&root)
            .env("ASCEND_GLOBAL_LOG_LEVEL", "2")
            .args([
                "--scenario",
                scenario.to_str().expect("scenario path"),
                "--model",
                model.to_str().expect("model path"),
                "--layers",
                &layers,
                "--tokens",
                &tokens,
                "--position",
                "0",
                "--output",
                hidden_path.to_str().expect("hidden output path"),
                "--artifact-dir",
                run_dir.to_str().expect("artifact directory"),
            ]);
        if let Some(input) = previous_hidden.as_ref() {
            command.args(["--input", input.to_str().expect("hidden input path")]);
        }
        if terminal {
            command.args([
                "--logits-output",
                logits_path.to_str().expect("logits output path"),
            ]);
        }
        let output = command.output().expect("run isolated DeepSeek range");
        assert!(
            output.status.success(),
            "node {node} range [{layer_start},{layer_end}) failed:\n{}",
            String::from_utf8_lossy(&output.stderr)
        );
        let report = last_json_report(&output.stdout).unwrap_or_else(|| {
            panic!(
                "range CLI JSON report missing:\n{}",
                String::from_utf8_lossy(&output.stdout)
            )
        });
        assert_eq!(report["status"], "ok");
        assert_eq!(
            report["layers"],
            serde_json::json!([layer_start, layer_end])
        );
        assert!(
            report["loaded_routed_expert_bytes"]
                .as_u64()
                .expect("routed expert byte count")
                > 0
        );
        eprintln!(
            "oracle range complete: node={node} layers=[{layer_start},{layer_end}) routed_expert_bytes={}",
            report["loaded_routed_expert_bytes"]
        );
        previous_hidden = Some(hidden_path);
    }

    let logits = read_f32(&logits_path);
    let (token, top_logit) = logits
        .iter()
        .copied()
        .enumerate()
        .max_by(|left, right| left.1.total_cmp(&right.1))
        .expect("non-empty logits");
    assert_eq!(token, 108149);
    assert!(
        (top_logit - 45.004646).abs() < 0.05,
        "unexpected top logit: {top_logit}"
    );
    let catalog = GgufCatalog::open(&model).expect("open oracle GGUF");
    assert_eq!(catalog.tokenizer_token_text(token).as_deref(), Ok("Ada"));

    fs::remove_dir_all(run_dir).expect("remove oracle run directory");
}

fn repository_root() -> PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR"))
        .parent()
        .and_then(Path::parent)
        .expect("repository root")
        .to_path_buf()
}

fn sibling_gguf_path() -> Option<PathBuf> {
    let ds4 = repository_root().join("../ds4");
    [
        ds4.join("ds4flash.gguf"),
        ds4.join("gguf/DeepSeek-V4-Flash-IQ2XXS-w2Q2K-AProjQ8-SExpQ8-OutQ8-chat-v2-imatrix.gguf"),
    ]
    .into_iter()
    .find(|path| path.is_file())
}

fn read_f32(path: &Path) -> Vec<f32> {
    let bytes = fs::read(path).expect("read logits file");
    assert_eq!(bytes.len() % 4, 0, "unaligned logits file");
    bytes
        .chunks_exact(4)
        .map(|chunk| f32::from_le_bytes(chunk.try_into().expect("four bytes")))
        .collect()
}

fn completed_payload(path: &Path, expected_bytes: u64) -> bool {
    fs::metadata(path)
        .map(|metadata| metadata.len() == expected_bytes)
        .unwrap_or(false)
}

fn last_json_report(stdout: &[u8]) -> Option<serde_json::Value> {
    String::from_utf8_lossy(stdout)
        .lines()
        .rev()
        .find_map(|line| serde_json::from_str(line).ok())
}

#[test]
fn mixed_native_output_uses_last_json_report() {
    let report = last_json_report(
        b"native runtime output\n{\"status\":\"progress\"}\nnoise\n{\"status\":\"ok\",\"token\":108149}\n",
    )
    .expect("final JSON report");
    assert_eq!(report["status"], "ok");
    assert_eq!(report["token"], 108149);
}
