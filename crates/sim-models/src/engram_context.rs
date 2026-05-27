use serde::{Deserialize, Serialize};

pub const ENGRAM_CONTEXT_HIDDEN_SIZE: usize = 1024;
pub const ENGRAM_CONTEXT_INDICES_PER_BATCH: usize = 8;
pub const ENGRAM_CONTEXT_SUPPORTED_BATCHES: [usize; 4] = [1, 4, 16, 64];
pub const PAPER_ENGRAM_CONTEXT_DEFAULT_ORDERS: [u8; 2] = [2, 3];
pub const PAPER_ENGRAM_CONTEXT_DEFAULT_HEADS_PER_ORDER: usize = 2;

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

#[derive(Clone, Debug)]
pub struct PaperEngramContextTableView<'a> {
    pub order: u8,
    pub head: u16,
    pub table: &'a [f32],
    pub table_rows: usize,
}

#[derive(Clone, Debug, Eq, PartialEq, Serialize, Deserialize)]
pub struct PaperEngramContextLookupRef {
    pub batch_index: usize,
    pub order: u8,
    pub head: u16,
    pub row: u64,
}

#[derive(Clone, Debug)]
pub struct PaperEngramContextOp<'a> {
    pub tables: &'a [PaperEngramContextTableView<'a>],
    pub lookups: &'a [PaperEngramContextLookupRef],
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

pub fn run_paper_engram_context_reference(
    op: &PaperEngramContextOp<'_>,
) -> Result<EngramContextOutput, String> {
    validate_paper_engram_context_op(op)?;

    let hidden_size = op.hidden_size;
    let mut output = vec![0.0f32; op.batch * hidden_size];
    let mut gate_values = vec![0.0f32; op.batch];
    let mut table_sums = vec![0.0f32; op.batch * hidden_size];
    let mut lookup_counts = vec![0usize; op.batch];

    for lookup in op.lookups {
        let table = paper_engram_context_table(op.tables, lookup.order, lookup.head)
            .ok_or_else(|| paper_engram_table_missing_error(lookup.order, lookup.head))?;
        let row = usize::try_from(lookup.row).map_err(|_| {
            format!(
                "paper_engram_context_row_out_of_bounds:order={}:head={}:row={}:rows={}",
                lookup.order, lookup.head, lookup.row, table.table_rows
            )
        })?;
        let sum_base = lookup.batch_index * hidden_size;
        let table_base = row * hidden_size;
        for dim in 0..hidden_size {
            table_sums[sum_base + dim] += table.table[table_base + dim];
        }
        lookup_counts[lookup.batch_index] += 1;
    }

    for batch_index in 0..op.batch {
        let vector_base = batch_index * hidden_size;
        let hidden = &op.hidden[vector_base..vector_base + hidden_size];
        let gate_weight = &op.gate_weight[vector_base..vector_base + hidden_size];
        let gate = sigmoid(dot(hidden, gate_weight));
        gate_values[batch_index] = gate;

        if lookup_counts[batch_index] == 0 {
            output[vector_base..vector_base + hidden_size].copy_from_slice(hidden);
            continue;
        }

        let scale = gate / lookup_counts[batch_index] as f32;
        for dim in 0..hidden_size {
            output[vector_base + dim] = hidden[dim] + scale * table_sums[vector_base + dim];
        }
    }

    let report = EngramContextReport {
        op: "PaperEngramContextOp",
        report_kind: "context_augmentation",
        mode: "cpu-reference-multi-table",
        batch: op.batch,
        hidden_size,
        table_rows: op.tables.iter().map(|table| table.table_rows).sum(),
        indices_per_batch: *lookup_counts.iter().max().unwrap_or(&0),
        output_checksum: checksum_f32_words(&output),
        gate_checksum: checksum_f32_words(&gate_values),
        index_checksum: checksum_paper_engram_lookup_refs(op.lookups),
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

pub fn deterministic_paper_engram_context_fixture(
    batch: usize,
    table_rows: usize,
) -> Result<EngramContextOutput, String> {
    if table_rows == 0 {
        return Err("paper_engram_context_table_rows_must_be_positive".to_string());
    }
    if !ENGRAM_CONTEXT_SUPPORTED_BATCHES.contains(&batch) {
        return Err(format!("unsupported_paper_engram_context_batch:{batch}"));
    }

    let hidden_size = ENGRAM_CONTEXT_HIDDEN_SIZE;
    let mut table_payloads = Vec::new();
    for &order in &PAPER_ENGRAM_CONTEXT_DEFAULT_ORDERS {
        for head in 0..PAPER_ENGRAM_CONTEXT_DEFAULT_HEADS_PER_ORDER {
            let mut table = vec![0.0f32; table_rows * hidden_size];
            for row in 0..table_rows {
                for dim in 0..hidden_size {
                    let raw =
                        ((usize::from(order) * 41 + head * 29 + row * 17 + dim * 13) % 257) as f32;
                    table[row * hidden_size + dim] = (raw - 128.0) / 512.0;
                }
            }
            table_payloads.push((order, head as u16, table));
        }
    }

    let tables = table_payloads
        .iter()
        .map(|(order, head, table)| PaperEngramContextTableView {
            order: *order,
            head: *head,
            table,
            table_rows,
        })
        .collect::<Vec<_>>();

    let mut lookups = Vec::new();
    for batch_index in 0..batch {
        for &order in &PAPER_ENGRAM_CONTEXT_DEFAULT_ORDERS {
            for head in 0..PAPER_ENGRAM_CONTEXT_DEFAULT_HEADS_PER_ORDER {
                let row = (batch_index * 7 + usize::from(order) * 11 + head * 13) % table_rows;
                lookups.push(PaperEngramContextLookupRef {
                    batch_index,
                    order,
                    head: head as u16,
                    row: row as u64,
                });
            }
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

    let op = PaperEngramContextOp {
        tables: &tables,
        lookups: &lookups,
        hidden: &hidden,
        gate_weight: &gate_weight,
        batch,
        hidden_size,
    };
    run_paper_engram_context_reference(&op)
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

pub fn validate_paper_engram_context_op(op: &PaperEngramContextOp<'_>) -> Result<(), String> {
    if !ENGRAM_CONTEXT_SUPPORTED_BATCHES.contains(&op.batch) {
        return Err(format!(
            "unsupported_paper_engram_context_batch:{}",
            op.batch
        ));
    }

    let hidden_size = op.hidden_size;
    if hidden_size == 0 {
        return Err("paper_engram_context_hidden_size_must_be_positive".to_string());
    }
    let expected_vectors = op.batch * hidden_size;
    if op.hidden.len() != expected_vectors {
        return Err(format!(
            "paper_engram_context_hidden_len_mismatch:expected={expected_vectors}:actual={}",
            op.hidden.len()
        ));
    }
    if op.gate_weight.len() != expected_vectors {
        return Err(format!(
            "paper_engram_context_gate_weight_len_mismatch:expected={expected_vectors}:actual={}",
            op.gate_weight.len()
        ));
    }

    let mut seen_tables = std::collections::BTreeSet::new();
    for table in op.tables {
        if table.order == 0 {
            return Err("paper_engram_context_table_order_must_be_positive".to_string());
        }
        if table.table_rows == 0 {
            return Err(format!(
                "paper_engram_context_table_rows_must_be_positive:order={}:head={}",
                table.order, table.head
            ));
        }
        if !seen_tables.insert((table.order, table.head)) {
            return Err(format!(
                "paper_engram_context_table_duplicate:order={}:head={}",
                table.order, table.head
            ));
        }
        let expected_table = table.table_rows * hidden_size;
        if table.table.len() != expected_table {
            return Err(format!(
                "paper_engram_context_table_len_mismatch:order={}:head={}:expected={expected_table}:actual={}",
                table.order,
                table.head,
                table.table.len()
            ));
        }
    }

    for lookup in op.lookups {
        if lookup.batch_index >= op.batch {
            return Err(format!(
                "paper_engram_context_lookup_batch_out_of_bounds:batch_index={}:batch={}",
                lookup.batch_index, op.batch
            ));
        }
        let table = paper_engram_context_table(op.tables, lookup.order, lookup.head)
            .ok_or_else(|| paper_engram_table_missing_error(lookup.order, lookup.head))?;
        let row = usize::try_from(lookup.row).map_err(|_| {
            format!(
                "paper_engram_context_row_out_of_bounds:order={}:head={}:row={}:rows={}",
                lookup.order, lookup.head, lookup.row, table.table_rows
            )
        })?;
        if row >= table.table_rows {
            return Err(format!(
                "paper_engram_context_row_out_of_bounds:order={}:head={}:row={}:rows={}",
                lookup.order, lookup.head, lookup.row, table.table_rows
            ));
        }
    }

    Ok(())
}

fn paper_engram_context_table<'a>(
    tables: &'a [PaperEngramContextTableView<'a>],
    order: u8,
    head: u16,
) -> Option<&'a PaperEngramContextTableView<'a>> {
    tables
        .iter()
        .find(|table| table.order == order && table.head == head)
}

fn paper_engram_table_missing_error(order: u8, head: u16) -> String {
    format!("paper_engram_context_table_missing:order={order}:head={head}")
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

fn checksum_paper_engram_lookup_refs(values: &[PaperEngramContextLookupRef]) -> u64 {
    values.iter().fold(0xcbf2_9ce4_8422_2325u64, |acc, value| {
        let acc = mix_checksum(acc, value.batch_index as u64);
        let acc = mix_checksum(acc, u64::from(value.order));
        let acc = mix_checksum(acc, u64::from(value.head));
        mix_checksum(acc, value.row)
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

    #[test]
    fn paper_engram_context_reference_aggregates_multi_order_multi_head_tables() {
        let hidden_size = 4;
        let order2_head0 = vec![2.0, 0.0, 0.0, 0.0, 4.0, 0.0, 0.0, 0.0];
        let order3_head1 = vec![10.0, 0.0, 0.0, 0.0, 20.0, 0.0, 0.0, 0.0];
        let tables = vec![
            PaperEngramContextTableView {
                order: 2,
                head: 0,
                table: &order2_head0,
                table_rows: 2,
            },
            PaperEngramContextTableView {
                order: 3,
                head: 1,
                table: &order3_head1,
                table_rows: 2,
            },
        ];
        let lookups = vec![
            PaperEngramContextLookupRef {
                batch_index: 0,
                order: 2,
                head: 0,
                row: 1,
            },
            PaperEngramContextLookupRef {
                batch_index: 0,
                order: 3,
                head: 1,
                row: 0,
            },
        ];
        let hidden = vec![1.0f32; hidden_size];
        let gate_weight = vec![0.0f32; hidden_size];
        let op = PaperEngramContextOp {
            tables: &tables,
            lookups: &lookups,
            hidden: &hidden,
            gate_weight: &gate_weight,
            batch: 1,
            hidden_size,
        };

        let output = run_paper_engram_context_reference(&op).expect("paper context op");

        assert_eq!(output.report.op, "PaperEngramContextOp");
        assert_eq!(output.report.indices_per_batch, 2);
        assert_eq!(output.report.table_rows, 4);
        assert_eq!(output.gate_values, vec![0.5]);
        assert!((output.output[0] - 4.5).abs() < 0.0001);
        assert_eq!(&output.output[1..], &[1.0, 1.0, 1.0]);
    }

    #[test]
    fn paper_engram_context_allows_empty_lookup_step() {
        let hidden_size = 4;
        let table = vec![0.0f32; hidden_size];
        let tables = vec![PaperEngramContextTableView {
            order: 2,
            head: 0,
            table: &table,
            table_rows: 1,
        }];
        let hidden = vec![1.0, 2.0, 3.0, 4.0];
        let gate_weight = vec![0.0f32; hidden_size];
        let op = PaperEngramContextOp {
            tables: &tables,
            lookups: &[],
            hidden: &hidden,
            gate_weight: &gate_weight,
            batch: 1,
            hidden_size,
        };

        let output = run_paper_engram_context_reference(&op).expect("empty lookup step");

        assert_eq!(output.output, hidden);
        assert_eq!(output.report.indices_per_batch, 0);
    }

    #[test]
    fn paper_engram_context_fixture_checksum_is_deterministic() {
        let first = deterministic_paper_engram_context_fixture(4, 16).expect("first paper fixture");
        let second =
            deterministic_paper_engram_context_fixture(4, 16).expect("second paper fixture");

        assert_eq!(first.report, second.report);
        assert_eq!(
            first.report.indices_per_batch,
            PAPER_ENGRAM_CONTEXT_DEFAULT_ORDERS.len()
                * PAPER_ENGRAM_CONTEXT_DEFAULT_HEADS_PER_ORDER
        );
        assert_eq!(first.output, second.output);
    }

    #[test]
    fn paper_engram_context_rejects_missing_table_view() {
        let hidden_size = 4;
        let table = vec![0.0f32; hidden_size];
        let tables = vec![PaperEngramContextTableView {
            order: 2,
            head: 0,
            table: &table,
            table_rows: 1,
        }];
        let lookups = vec![PaperEngramContextLookupRef {
            batch_index: 0,
            order: 3,
            head: 0,
            row: 0,
        }];
        let hidden = vec![0.0f32; hidden_size];
        let gate_weight = vec![0.0f32; hidden_size];
        let op = PaperEngramContextOp {
            tables: &tables,
            lookups: &lookups,
            hidden: &hidden,
            gate_weight: &gate_weight,
            batch: 1,
            hidden_size,
        };

        let err = run_paper_engram_context_reference(&op).expect_err("missing table should fail");

        assert_eq!(err, "paper_engram_context_table_missing:order=3:head=0");
    }

    #[test]
    fn paper_engram_context_rejects_out_of_bounds_row() {
        let hidden_size = 4;
        let table = vec![0.0f32; hidden_size];
        let tables = vec![PaperEngramContextTableView {
            order: 2,
            head: 0,
            table: &table,
            table_rows: 1,
        }];
        let lookups = vec![PaperEngramContextLookupRef {
            batch_index: 0,
            order: 2,
            head: 0,
            row: 1,
        }];
        let hidden = vec![0.0f32; hidden_size];
        let gate_weight = vec![0.0f32; hidden_size];
        let op = PaperEngramContextOp {
            tables: &tables,
            lookups: &lookups,
            hidden: &hidden,
            gate_weight: &gate_weight,
            batch: 1,
            hidden_size,
        };

        let err = run_paper_engram_context_reference(&op).expect_err("row should fail");

        assert!(err.contains("paper_engram_context_row_out_of_bounds"));
    }
}
