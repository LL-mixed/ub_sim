//! Independent numerical audit of optional snapshots from actual PTO kernels.
//! Dispatch success and object/lease gates are checked separately by the W5 audit.
use anyhow::{ensure, Context, Result};
use serde::Serialize;
use sim_models::qwen3_dense_reference as reference;
use std::collections::BTreeMap;
use std::path::Path;

struct Trace {
    geometry: [u32; 7],
    layers: BTreeMap<(u32, u32), Vec<f32>>,
    logits: Vec<f32>,
}

struct Cursor<'a>(&'a [u8]);
impl Cursor<'_> {
    fn word(&mut self) -> Result<u32> {
        ensure!(self.0.len() >= 4, "truncated trace");
        let value = u32::from_le_bytes(self.0[..4].try_into()?);
        self.0 = &self.0[4..];
        Ok(value)
    }
    fn values(&mut self, count: usize) -> Result<Vec<f32>> {
        ensure!(count <= self.0.len() / 4, "truncated matrix");
        (0..count)
            .map(|_| {
                let value = f32::from_bits(self.word()?);
                ensure!(value.is_finite(), "non-finite snapshot");
                Ok(value)
            })
            .collect()
    }
}

impl Trace {
    fn parse(bytes: &[u8], geometry: [u32; 7], terminal: bool) -> Result<Self> {
        ensure!(bytes.starts_with(b"QPTOTR1\0"), "invalid trace magic");
        let mut cursor = Cursor(&bytes[8..]);
        for value in geometry {
            ensure!(cursor.word()? == value, "trace geometry mismatch");
        }
        let [first, end, past, tokens, hidden, kv, vocab] = geometry;
        let mut layers = BTreeMap::new();
        // Enforce exact layer/kind order, including all history KV rows.
        for layer in first..end {
            for kind in 1..=3 {
                let rows = if kind == 1 { tokens } else { past + tokens };
                let cols = if kind == 1 { hidden } else { kv };
                for value in [kind, layer, 0, 0, rows, cols] {
                    ensure!(cursor.word()? == value, "missing/reordered layer record");
                }
                layers.insert((kind, layer), cursor.values(rows as usize * cols as usize)?);
            }
        }
        let mut logits = Vec::new();
        if terminal {
            while logits.len() < vocab as usize {
                let col = logits.len() as u32;
                let cols = 64.min(vocab - col);
                for value in [4, end, 0, col, 1, cols] {
                    ensure!(cursor.word()? == value, "missing/reordered logit record");
                }
                logits.extend(cursor.values(cols as usize)?);
            }
        }
        for _ in 0..6 {
            ensure!(cursor.word()? == 0, "missing success terminator");
        }
        ensure!(cursor.0.is_empty(), "extra trace records");
        Ok(Self {
            geometry,
            layers,
            logits,
        })
    }
}

#[derive(Default, Serialize)]
struct ErrorMetric {
    elements: usize,
    max_abs: f32,
    max_scaled: f32,
    violations: usize,
    worst_index: usize,
    actual_at_worst: f32,
    expected_at_worst: f32,
}
impl ErrorMetric {
    fn check(&mut self, actual: &[f32], expected: &[f32], limit: f32, scaled: bool) -> Result<()> {
        ensure!(
            !actual.is_empty() && actual.len() == expected.len(),
            "numerical length mismatch"
        );
        for (index, (&a, &b)) in actual.iter().zip(expected).enumerate() {
            ensure!(a.is_finite() && b.is_finite(), "non-finite comparison");
            let abs = (a - b).abs();
            let relative = abs / (1.0 + b.abs());
            if (if scaled { relative } else { abs }) > limit {
                self.violations += 1;
            }
            if if scaled {
                relative > self.max_scaled
            } else {
                abs > self.max_abs
            } {
                self.worst_index = index;
                self.actual_at_worst = a;
                self.expected_at_worst = b;
            }
            self.max_abs = self.max_abs.max(abs);
            self.max_scaled = self.max_scaled.max(relative);
            self.elements += 1;
        }
        ensure!(
            self.violations == 0,
            "numerical mismatch max_abs={} max_scaled={} violations={} limit={limit}",
            self.max_abs,
            self.max_scaled,
            self.violations
        );
        Ok(())
    }
}

#[derive(Serialize)]
struct LayerReport {
    step: usize,
    node: usize,
    layer: u64,
    hidden: ErrorMetric,
    key: ErrorMetric,
    value: ErrorMetric,
}

// Diagnostic only: both inputs use the reference wire conversion here. This
// isolates quantization-boundary crossings from any difference in TCVT's
// tie-breaking rule. These values never feed the independent reference.
fn handoff_differences(actual: &[f32], expected: &[f32]) -> Result<Vec<serde_json::Value>> {
    ensure!(actual.len() == expected.len(), "handoff length mismatch");
    let mut differences = Vec::new();
    for (index, (&a, &b)) in actual.iter().zip(expected).enumerate() {
        ensure!(a.is_finite() && b.is_finite(), "non-finite handoff input");
        let ca = sim_uapi::qwen3_reference_f16_handoff(a);
        let cb = sim_uapi::qwen3_reference_f16_handoff(b);
        if ca.to_bits() != cb.to_bits() {
            differences.push(serde_json::json!({
                "index":index, "candidate_pre_f16":a, "reference_pre_f16":b,
                "candidate_using_reference_rounding":ca, "reference_wire":cb,
            }));
        }
    }
    Ok(differences)
}

fn verify(
    directory: &Path,
    weights: &Path,
    prompt: &[u64],
    steps: usize,
) -> Result<serde_json::Value> {
    let config = std::fs::read_to_string(weights.join("config.json"))?;
    let mut p = reference::profile_from_config_json(&config).map_err(anyhow::Error::msg)?;
    p.tp_nodes = 2;
    ensure!(
        p.num_hidden_layers == 28 && p.hidden_size == 1024 && p.vocab_size == 151936,
        "requires W5 Qwen3-0.6B two-node geometry"
    );
    ensure!(
        steps == 8 && !prompt.is_empty() && prompt.len() <= 1024,
        "requires 8 steps and bounded prompt"
    );
    ensure!(
        prompt.iter().all(|t| *t < p.vocab_size),
        "invalid prompt token"
    );
    let loaded = reference::load_safetensors_path_metadata(weights).map_err(anyhow::Error::msg)?;
    let mut caches = BTreeMap::new();
    let mut next_token = 0;
    let mut layers = Vec::new();
    let mut logits_reports = Vec::new();
    let mut used_files = Vec::new();
    let mut failures = Vec::new();
    let mut handoffs = Vec::new();
    for step in 0..steps {
        let ids = if step == 0 {
            prompt.to_vec()
        } else {
            vec![next_token]
        };
        let past = if step == 0 {
            0
        } else {
            prompt.len() + step - 1
        } as u32;
        let mut sequence =
            reference::embedding_reference_hidden_sequence_for_profile(p, &loaded.tensors, &ids)
                .map_err(anyhow::Error::msg)?;
        for node in 0..2 {
            let first = node * 14;
            let end = first + 14;
            let name = format!("range-{first}-{end}-past-{past}.bin");
            let path = directory.join(&name);
            let bytes = std::fs::read(&path).with_context(|| name.clone())?;
            let trace = Trace::parse(
                &bytes,
                [
                    first,
                    end,
                    past,
                    ids.len() as u32,
                    p.hidden_size as u32,
                    (p.num_key_value_heads * p.head_dim) as u32,
                    p.vocab_size as u32,
                ],
                node == 1,
            )
            .with_context(|| name.clone())?;
            used_files.push(name);
            for layer in u64::from(first)..u64::from(end) {
                let (forward, next) = if step == 0 {
                    reference::forward_reference_from_hidden_sequence_range_with_kv_cache_for_profile(
                        p, &loaded.tensors, layer, layer + 1, &sequence,
                    ).map_err(anyhow::Error::msg)?
                } else {
                    let cache = caches.get(&layer).context("missing reference cache")?;
                    let forward =
                        reference::forward_incremental_range_with_kv_cache_from_hidden_for_profile(
                            p,
                            &loaded.tensors,
                            std::slice::from_ref(cache),
                            layer,
                            layer + 1,
                            u64::from(past),
                            &sequence[0],
                        )
                        .map_err(anyhow::Error::msg)?;
                    let next = vec![forward.forward.final_hidden.clone()];
                    (forward, next)
                };
                let cache = forward
                    .kv_cache
                    .into_iter()
                    .next()
                    .context("empty reference cache")?;
                let mut report = LayerReport {
                    step,
                    node: node as usize + 1,
                    layer,
                    hidden: ErrorMetric::default(),
                    key: ErrorMetric::default(),
                    value: ErrorMetric::default(),
                };
                let checked = report
                    .hidden
                    .check(
                        &trace.layers[&(1, layer as u32)],
                        &next.iter().flatten().copied().collect::<Vec<_>>(),
                        0.002,
                        true,
                    )
                    .with_context(|| format!("step={step} layer={layer} hidden"));
                if let Err(error) = checked {
                    failures.push(format!("{error:#}"));
                }
                let checked = report
                    .key
                    .check(
                        &trace.layers[&(2, layer as u32)],
                        &cache
                            .rope_k_states
                            .iter()
                            .flatten()
                            .copied()
                            .collect::<Vec<_>>(),
                        0.0002,
                        true,
                    )
                    .with_context(|| format!("step={step} layer={layer} key"));
                if let Err(error) = checked {
                    failures.push(format!("{error:#}"));
                }
                let checked = report
                    .value
                    .check(
                        &trace.layers[&(3, layer as u32)],
                        &cache.v_states.iter().flatten().copied().collect::<Vec<_>>(),
                        0.0002,
                        true,
                    )
                    .with_context(|| format!("step={step} layer={layer} value"));
                if let Err(error) = checked {
                    failures.push(format!("{error:#}"));
                }
                caches.insert(layer, cache);
                sequence = next;
                layers.push(report);
            }
            if node == 0 {
                handoffs.push(serde_json::json!({
                    "step":step,
                    "scope":"nodeA pre-F16 snapshots; candidate is converted with reference rounding for diagnosis, not observed wire bytes",
                    "differences":handoff_differences(
                        &trace.layers[&(1, end - 1)],
                        &sequence.iter().flatten().copied().collect::<Vec<_>>(),
                    )?,
                }));
                // Match the independent reference W5 inter-node wire conversion.
                for row in &mut sequence {
                    for v in row {
                        *v = sim_uapi::qwen3_reference_f16_handoff(*v);
                    }
                }
            } else {
                let (summary, expected) =
                    reference::full_vocab_logits_values_from_hidden_for_profile(
                        p,
                        &loaded.tensors,
                        sequence.last().context("empty terminal sequence")?,
                    )
                    .map_err(anyhow::Error::msg)?;
                let mut error = ErrorMetric::default();
                let checked = error
                    .check(&trace.logits, &expected, 0.02, false)
                    .with_context(|| format!("step={step} full logits"));
                if let Err(error) = checked {
                    failures.push(format!("{error:#}"));
                }
                let actual_top = trace
                    .logits
                    .iter()
                    .enumerate()
                    .max_by(|a, b| a.1.total_cmp(b.1))
                    .context("empty logits")?
                    .0 as u64;
                ensure!(
                    actual_top == summary.top_token_id,
                    "step {step}: top token mismatch"
                );
                next_token = summary.top_token_id;
                logits_reports
                    .push(serde_json::json!({"step":step, "token":next_token, "error":error}));
            }
            ensure!(trace.geometry[2] == past, "past mismatch");
        }
        eprintln!("qwen3-pto-trace-check: step={step} all layers/logits checked token={next_token} failures={}", failures.len());
    }
    let mut found: Vec<_> = std::fs::read_dir(directory)?
        .map(|item| {
            let item = item?;
            Ok(item.file_name().to_string_lossy().into_owned())
        })
        .collect::<Result<_>>()?;
    used_files.sort();
    found.sort();
    ensure!(found == used_files, "unexpected or missing snapshot files");
    Ok(
        serde_json::json!({"status":if failures.is_empty() {"pass"} else {"fail"}, "failures":failures,
        "scope":"actual PTO snapshots vs independent two-node reference; requires separate successful W5 execution audit",
        "steps":steps, "nodes":2, "prompt":prompt, "trace_files":used_files,
        "limits":{"hidden_scaled":0.002,"kv_scaled":0.0002,"logits_abs":0.02},
        "layers":layers,"logits":logits_reports,"handoff_diagnostics":handoffs}),
    )
}

pub fn run_if_requested() -> Result<bool> {
    let mut args = std::env::args_os().skip(1);
    if args.next().as_deref() != Some(std::ffi::OsStr::new("qwen3-pto-trace-check")) {
        return Ok(false);
    }
    let mut values = BTreeMap::new();
    while let Some(key) = args.next() {
        let key = key.to_str().context("invalid option")?.to_string();
        ensure!(
            ["--trace-dir", "--weights", "--tokens", "--steps"].contains(&key.as_str()),
            "unknown option {key}"
        );
        let value = args.next().context("missing option value")?;
        ensure!(values.insert(key, value).is_none(), "duplicate option");
    }
    let get = |key: &str| values.get(key).with_context(|| format!("provide {key}"));
    let tokens: Vec<u64> = get("--tokens")?
        .to_str()
        .context("invalid tokens")?
        .split(',')
        .map(str::parse)
        .collect::<std::result::Result<_, _>>()?;
    let steps = get("--steps")?.to_str().context("invalid steps")?.parse()?;
    let report = verify(
        Path::new(get("--trace-dir")?),
        Path::new(get("--weights")?),
        &tokens,
        steps,
    )?;
    println!("{}", serde_json::to_string_pretty(&report)?);
    ensure!(
        report["status"] == "pass",
        "numerical trace audit failed; see structured failures"
    );
    Ok(true)
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn handoff_diagnostic_exposes_quantization_crossings_without_changing_inputs() {
        let midpoint = 1.0 + 1.0 / 2048.0;
        let actual = [midpoint + f32::EPSILON];
        let expected = [midpoint - f32::EPSILON];
        let differences = handoff_differences(&actual, &expected).unwrap();
        assert_eq!(differences.len(), 1);
        assert_eq!(differences[0]["reference_wire"], 1.0);
        assert_eq!(
            differences[0]["candidate_using_reference_rounding"],
            1.0 + 1.0 / 1024.0
        );
        assert_eq!(actual[0], midpoint + f32::EPSILON);
        assert_eq!(expected[0], midpoint - f32::EPSILON);
        assert!(handoff_differences(&[1.0], &[1.0001]).unwrap().is_empty());
        assert!(handoff_differences(&actual, &[]).is_err());
        assert!(handoff_differences(&[f32::NAN], &expected).is_err());
    }
    #[test]
    fn trace_parser_requires_every_record_and_final_marker() {
        let g = [0, 1, 0, 1, 2, 2, 3];
        let mut bytes = b"QPTOTR1\0".to_vec();
        let append = |bytes: &mut Vec<u8>, values: &[u32]| {
            for v in values {
                bytes.extend_from_slice(&v.to_le_bytes());
            }
        };
        append(&mut bytes, &g);
        for kind in 1..=3 {
            append(
                &mut bytes,
                &[kind, 0, 0, 0, 1, 2, 1f32.to_bits(), 2f32.to_bits()],
            );
        }
        append(
            &mut bytes,
            &[
                4,
                1,
                0,
                0,
                1,
                3,
                1f32.to_bits(),
                2f32.to_bits(),
                3f32.to_bits(),
            ],
        );
        append(&mut bytes, &[0; 6]);
        assert_eq!(
            Trace::parse(&bytes, g, true).unwrap().logits,
            vec![1.0, 2.0, 3.0]
        );
        for size in 0..bytes.len() {
            assert!(Trace::parse(&bytes[..size], g, true).is_err());
        }
        let mut wrong = bytes.clone();
        wrong[36] = 3;
        assert!(Trace::parse(&wrong, g, true).is_err());
        let mut nan = bytes.clone();
        nan[60..64].copy_from_slice(&f32::NAN.to_bits().to_le_bytes());
        assert!(Trace::parse(&nan, g, true).is_err());
        assert!(Trace::parse(&bytes, g, false).is_err());
        bytes.push(0);
        assert!(Trace::parse(&bytes, g, true).is_err());
    }
    #[test]
    fn numeric_audit_rejects_outliers_and_bad_reference() {
        let mut error = ErrorMetric::default();
        error.check(&[1.001], &[1.0], 0.002, true).unwrap();
        assert_eq!(error.elements, 1);
        assert!(error.check(&[1.1], &[1.0], 0.002, true).is_err());
        assert!(error.check(&[1.0], &[f32::INFINITY], 0.02, false).is_err());
        assert!(error.check(&[1.0], &[], 0.02, false).is_err());
        assert_eq!(sim_uapi::qwen3_reference_f16_handoff(1.0001), 1.0);
        assert_eq!(
            sim_uapi::qwen3_reference_f16_handoff(1.0 + 1.0 / 2048.0),
            1.0 + 1.0 / 1024.0
        );
    }
}
