use serde::{Deserialize, Serialize};

pub const ENGRAM_CONTEXT_HIDDEN_SIZE: usize = 1024;
pub const ENGRAM_CONTEXT_INDICES_PER_BATCH: usize = 8;
pub const ENGRAM_CONTEXT_SUPPORTED_BATCHES: [usize; 4] = [1, 4, 16, 64];

#[derive(Clone, Debug)]
pub struct EngramContextOp<'a> {
    pub table: &'a [f32],
    pub table_rows: usize,
    pub indices: &'a [i32],
    pub hidden: &'a [f32],
    pub gate_weight: &'a [f32],
    pub batch: usize,
    pub hidden_size: usize,
}

#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
pub struct EngramContextReport {
    pub op: &'static str,
    pub report_kind: &'static str,
    pub mode: &'static str,
    pub batch: usize,
    pub hidden_size: usize,
    pub table_rows: usize,
    pub indices_per_batch: usize,
    pub output_checksum: u64,
    pub gate_checksum: u64,
    pub index_checksum: u64,
    pub output_l1_milli: u64,
}

#[derive(Clone, Debug, PartialEq)]
pub struct EngramContextOutput {
    pub output: Vec<f32>,
    pub gate_values: Vec<f32>,
    pub report: EngramContextReport,
}

pub fn run_engram_context_reference(
    op: &EngramContextOp<'_>,
) -> Result<EngramContextOutput, String> {
    validate_engram_context_op(op)?;

    let hidden_size = op.hidden_size;
    let mut output = vec![0.0f32; op.batch * hidden_size];
    let mut gate_values = vec![0.0f32; op.batch];

    for batch_index in 0..op.batch {
        let vector_base = batch_index * hidden_size;
        let index_base = batch_index * ENGRAM_CONTEXT_INDICES_PER_BATCH;
        let hidden = &op.hidden[vector_base..vector_base + hidden_size];
        let gate_weight = &op.gate_weight[vector_base..vector_base + hidden_size];
        let gate = sigmoid(dot(hidden, gate_weight));
        gate_values[batch_index] = gate;

        for dim in 0..hidden_size {
            let mut table_sum = 0.0f32;
            for slot in 0..ENGRAM_CONTEXT_INDICES_PER_BATCH {
                let row = op.indices[index_base + slot] as usize;
                table_sum += op.table[row * hidden_size + dim];
            }
            let mean = table_sum / ENGRAM_CONTEXT_INDICES_PER_BATCH as f32;
            output[vector_base + dim] = hidden[dim] + gate * mean;
        }
    }

    let report = EngramContextReport {
        op: "EngramContextOp",
        report_kind: "context_augmentation",
        mode: "cpu-reference",
        batch: op.batch,
        hidden_size,
        table_rows: op.table_rows,
        indices_per_batch: ENGRAM_CONTEXT_INDICES_PER_BATCH,
        output_checksum: checksum_f32_words(&output),
        gate_checksum: checksum_f32_words(&gate_values),
        index_checksum: checksum_i32_words(op.indices),
        output_l1_milli: l1_milli(&output),
    };

    Ok(EngramContextOutput {
        output,
        gate_values,
        report,
    })
}

pub fn deterministic_engram_context_fixture(
    batch: usize,
    table_rows: usize,
) -> Result<EngramContextOutput, String> {
    if table_rows < ENGRAM_CONTEXT_INDICES_PER_BATCH {
        return Err(format!(
            "engram_context_table_rows_too_small:rows={table_rows}:min={}",
            ENGRAM_CONTEXT_INDICES_PER_BATCH
        ));
    }
    if !ENGRAM_CONTEXT_SUPPORTED_BATCHES.contains(&batch) {
        return Err(format!("unsupported_engram_context_batch:{batch}"));
    }

    let hidden_size = ENGRAM_CONTEXT_HIDDEN_SIZE;
    let mut table = vec![0.0f32; table_rows * hidden_size];
    for row in 0..table_rows {
        for dim in 0..hidden_size {
            let raw = ((row * 37 + dim * 17 + 11) % 257) as f32;
            table[row * hidden_size + dim] = (raw - 128.0) / 512.0;
        }
    }

    let mut hidden = vec![0.0f32; batch * hidden_size];
    let mut gate_weight = vec![0.0f32; batch * hidden_size];
    for batch_index in 0..batch {
        for dim in 0..hidden_size {
            let base = batch_index * hidden_size + dim;
            let hidden_raw = ((batch_index * 19 + dim * 13 + 5) % 193) as f32;
            let gate_raw = ((batch_index * 23 + dim * 7 + 3) % 127) as f32;
            hidden[base] = (hidden_raw - 96.0) / 256.0;
            gate_weight[base] = (gate_raw - 63.0) / 8192.0;
        }
    }

    let mut indices = vec![0i32; batch * ENGRAM_CONTEXT_INDICES_PER_BATCH];
    for batch_index in 0..batch {
        for slot in 0..ENGRAM_CONTEXT_INDICES_PER_BATCH {
            indices[batch_index * ENGRAM_CONTEXT_INDICES_PER_BATCH + slot] =
                ((batch_index * 5 + slot * 3) % table_rows) as i32;
        }
    }

    let op = EngramContextOp {
        table: &table,
        table_rows,
        indices: &indices,
        hidden: &hidden,
        gate_weight: &gate_weight,
        batch,
        hidden_size,
    };
    run_engram_context_reference(&op)
}

pub fn validate_engram_context_op(op: &EngramContextOp<'_>) -> Result<(), String> {
    if !ENGRAM_CONTEXT_SUPPORTED_BATCHES.contains(&op.batch) {
        return Err(format!("unsupported_engram_context_batch:{}", op.batch));
    }
    if op.table_rows == 0 {
        return Err("engram_context_table_rows_must_be_positive".to_string());
    }

    let hidden_size = op.hidden_size;
    if hidden_size == 0 {
        return Err("engram_context_hidden_size_must_be_positive".to_string());
    }
    let expected_table = op.table_rows * hidden_size;
    let expected_vectors = op.batch * hidden_size;
    let expected_indices = op.batch * ENGRAM_CONTEXT_INDICES_PER_BATCH;

    if op.table.len() != expected_table {
        return Err(format!(
            "engram_context_table_len_mismatch:expected={expected_table}:actual={}",
            op.table.len()
        ));
    }
    if op.hidden.len() != expected_vectors {
        return Err(format!(
            "engram_context_hidden_len_mismatch:expected={expected_vectors}:actual={}",
            op.hidden.len()
        ));
    }
    if op.gate_weight.len() != expected_vectors {
        return Err(format!(
            "engram_context_gate_weight_len_mismatch:expected={expected_vectors}:actual={}",
            op.gate_weight.len()
        ));
    }
    if op.indices.len() != expected_indices {
        return Err(format!(
            "engram_context_indices_len_mismatch:expected={expected_indices}:actual={}",
            op.indices.len()
        ));
    }

    for &index in op.indices {
        if index < 0 || index as usize >= op.table_rows {
            return Err(format!(
                "engram_context_index_out_of_bounds:index={index}:rows={}",
                op.table_rows
            ));
        }
    }

    Ok(())
}

fn dot(left: &[f32], right: &[f32]) -> f32 {
    left.iter()
        .zip(right.iter())
        .fold(0.0f32, |acc, (left, right)| acc + left * right)
}

fn sigmoid(value: f32) -> f32 {
    1.0 / (1.0 + (-value).exp())
}

fn checksum_f32_words(values: &[f32]) -> u64 {
    values.iter().fold(0xcbf2_9ce4_8422_2325u64, |acc, value| {
        mix_checksum(acc, value.to_bits() as u64)
    })
}

fn checksum_i32_words(values: &[i32]) -> u64 {
    values.iter().fold(0xcbf2_9ce4_8422_2325u64, |acc, value| {
        mix_checksum(acc, *value as u32 as u64)
    })
}

fn mix_checksum(acc: u64, value: u64) -> u64 {
    let mixed = acc ^ value;
    mixed.wrapping_mul(0x1000_0000_01b3)
}

fn l1_milli(values: &[f32]) -> u64 {
    values
        .iter()
        .map(|value| (value.abs() * 1000.0).round() as u64)
        .sum()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn engram_context_reference_computes_expected_small_prefix() {
        let hidden_size = ENGRAM_CONTEXT_HIDDEN_SIZE;
        let table_rows = ENGRAM_CONTEXT_INDICES_PER_BATCH;
        let mut table = vec![0.0f32; table_rows * hidden_size];
        for row in 0..table_rows {
            table[row * hidden_size] = row as f32;
        }
        let hidden = vec![1.0f32; hidden_size];
        let gate_weight = vec![0.0f32; hidden_size];
        let indices = (0..ENGRAM_CONTEXT_INDICES_PER_BATCH as i32).collect::<Vec<_>>();

        let op = EngramContextOp {
            table: &table,
            table_rows,
            indices: &indices,
            hidden: &hidden,
            gate_weight: &gate_weight,
            batch: 1,
            hidden_size,
        };

        let output = run_engram_context_reference(&op).expect("reference op");

        assert_eq!(output.report.report_kind, "context_augmentation");
        assert_eq!(output.gate_values, vec![0.5]);
        assert!((output.output[0] - 2.75).abs() < 0.0001);
        assert!((output.output[1] - 1.0).abs() < 0.0001);
    }

    #[test]
    fn engram_context_fixture_checksum_is_deterministic() {
        let first = deterministic_engram_context_fixture(4, 16).expect("first fixture");
        let second = deterministic_engram_context_fixture(4, 16).expect("second fixture");

        assert_eq!(first.report, second.report);
        assert_eq!(first.output, second.output);
    }

    #[test]
    fn engram_context_rejects_unsupported_batch() {
        let err = deterministic_engram_context_fixture(2, 16).expect_err("batch should fail");
        assert!(err.contains("unsupported_engram_context_batch"));
    }

    #[test]
    fn engram_context_rejects_out_of_bounds_index() {
        let hidden_size = ENGRAM_CONTEXT_HIDDEN_SIZE;
        let table = vec![0.0f32; ENGRAM_CONTEXT_INDICES_PER_BATCH * hidden_size];
        let hidden = vec![0.0f32; hidden_size];
        let gate_weight = vec![0.0f32; hidden_size];
        let mut indices = vec![0i32; ENGRAM_CONTEXT_INDICES_PER_BATCH];
        indices[3] = ENGRAM_CONTEXT_INDICES_PER_BATCH as i32;
        let op = EngramContextOp {
            table: &table,
            table_rows: ENGRAM_CONTEXT_INDICES_PER_BATCH,
            indices: &indices,
            hidden: &hidden,
            gate_weight: &gate_weight,
            batch: 1,
            hidden_size,
        };

        let err = run_engram_context_reference(&op).expect_err("index should fail");
        assert!(err.contains("engram_context_index_out_of_bounds"));
    }
}
