#pragma once

#include "deepseek_v4_flash_pto_weights.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <span>
#include <stdexcept>
#include <vector>

#include <pto/pto-inst.hpp>

namespace lingqu::deepseek_v4_flash_pto {

constexpr uint32_t kVectorCapacity = 4096;
constexpr uint32_t kQ8BlockElements = 32;
constexpr uint32_t kQ8BlockBytes = 34;
constexpr uint32_t kQ8KBlockElements = 256;
constexpr uint32_t kQ2KBlockBytes = 84;
constexpr uint32_t kIq2XxsBlockBytes = 66;
constexpr uint32_t kGgufTypeF16 = 1;
constexpr uint32_t kGgufTypeQ8_0 = 8;
constexpr uint32_t kGgufTypeQ2_K = 10;
constexpr uint32_t kGgufTypeIq2Xxs = 16;

struct OperatorCounts {
    uint64_t tload = 0;
    uint64_t tstore = 0;
    uint64_t q8_linear = 0;
    uint64_t q2_k_linear = 0;
    uint64_t iq2_xxs_linear = 0;
    uint64_t f16_linear = 0;
    uint64_t grouped_q8_linear = 0;
    uint64_t fp8_kv_roundtrip = 0;
    uint64_t f16_roundtrip = 0;
    uint64_t rms_norm = 0;
    uint64_t head_rms_norm = 0;
    uint64_t rope_tail = 0;
    uint64_t kv_write = 0;
    uint64_t kv_gather = 0;
    uint64_t sink_attention = 0;
    uint64_t compressor_update = 0;
    uint64_t indexer_qat = 0;
    uint64_t sparse_indexer = 0;
    uint64_t sparse_indexer_topk = 0;
    uint64_t router = 0;
    uint64_t router_argmax = 0;
    uint64_t routed_experts = 0;
    uint64_t shared_expert = 0;
    uint64_t hc_split = 0;
    uint64_t hc_weighted_sum = 0;
    uint64_t hc_post = 0;
    uint64_t residual = 0;
    uint64_t swiglu = 0;
};

inline thread_local OperatorCounts operator_counts;

struct F32View {
    uintptr_t address;
    uint32_t rows;
    uint32_t columns;
    uint32_t row_stride;
    uint32_t column_stride;

    void validate() const {
        if (!address || !rows || !columns || !row_stride || !column_stride) {
            throw std::invalid_argument("deepseek_pto_f32_view");
        }
        const uint64_t last = uint64_t(rows - 1) * row_stride +
                              uint64_t(columns - 1) * column_stride;
        if (last > (std::numeric_limits<uintptr_t>::max() - address) / sizeof(float)) {
            throw std::invalid_argument("deepseek_pto_f32_view_overflow");
        }
    }

    uintptr_t at(uint32_t row, uint32_t column = 0) const {
        if (row >= rows || column >= columns) {
            throw std::out_of_range("deepseek_pto_f32_view_bounds");
        }
        return address +
               (uint64_t(row) * row_stride + uint64_t(column) * column_stride) *
                   sizeof(float);
    }
};

using F32Row = pto::Tile<pto::TileType::Vec, float, 1, kVectorCapacity,
                         pto::BLayout::RowMajor, 1, pto::DYNAMIC>;
using F32Scalar = pto::Tile<pto::TileType::Vec, float, 1, 8,
                            pto::BLayout::RowMajor, 1, 1>;
using U32Scalar = pto::Tile<pto::TileType::Vec, uint32_t, 1, 8,
                            pto::BLayout::RowMajor, 1, 1>;
using U32Row = pto::Tile<pto::TileType::Vec, uint32_t, 1, kVectorCapacity,
                         pto::BLayout::RowMajor, 1, pto::DYNAMIC>;
using I32Row = pto::Tile<pto::TileType::Vec, int32_t, 1, kVectorCapacity,
                         pto::BLayout::RowMajor, 1, pto::DYNAMIC>;
using F16Scalar = pto::Tile<pto::TileType::Vec, half, 1, 16,
                            pto::BLayout::RowMajor, 1, 1>;
using F16Row = pto::Tile<pto::TileType::Vec, half, 1, kVectorCapacity,
                         pto::BLayout::RowMajor, 1, pto::DYNAMIC>;
using I8Block = pto::Tile<pto::TileType::Vec, int8_t, 1, kQ8BlockElements,
                          pto::BLayout::RowMajor, 1, pto::DYNAMIC>;
using I8SuperBlock =
    pto::Tile<pto::TileType::Vec, int8_t, 1, kQ8KBlockElements,
              pto::BLayout::RowMajor, 1, pto::DYNAMIC>;
using I16Block = pto::Tile<pto::TileType::Vec, int16_t, 1, kQ8BlockElements,
                           pto::BLayout::RowMajor, 1, pto::DYNAMIC>;
using I16Scalar = pto::Tile<pto::TileType::Vec, int16_t, 1, 16,
                            pto::BLayout::RowMajor, 1, 1>;
using U8Block = pto::Tile<pto::TileType::Vec, uint8_t, 1, kQ8BlockElements,
                          pto::BLayout::RowMajor, 1, pto::DYNAMIC>;
using U8Scalar = pto::Tile<pto::TileType::Vec, uint8_t, 1, kQ8BlockElements,
                           pto::BLayout::RowMajor, 1, 1>;
using RowShape = pto::Shape<1, 1, 1, 1, pto::DYNAMIC>;
using RowStride = pto::Stride<pto::DYNAMIC, pto::DYNAMIC, pto::DYNAMIC,
                              pto::DYNAMIC, pto::DYNAMIC>;

template <typename T>
using GlobalRow = pto::GlobalTensor<T, RowShape, RowStride>;

template <typename T>
inline GlobalRow<T> global_row(uintptr_t address, uint32_t columns, uint32_t stride = 1) {
    if (!address || !columns || columns > kVectorCapacity || !stride) {
        throw std::invalid_argument("deepseek_pto_global_row");
    }
    const uint64_t extent = uint64_t(columns - 1) * stride + 1;
    return GlobalRow<T>(reinterpret_cast<T *>(address), RowShape(columns),
                        RowStride(extent, extent, extent, extent, stride));
}

inline void load(F32Row &tile, const F32View &view, uint32_t row,
                 uint32_t column = 0) {
    view.validate();
    const uint32_t count = tile.GetValidCol();
    if (count > view.columns || column > view.columns - count) {
        throw std::invalid_argument("deepseek_pto_load_geometry");
    }
    auto source = global_row<float>(view.at(row, column), count, view.column_stride);
    pto::TLOAD(tile, source);
    ++operator_counts.tload;
}

inline void store(const F32View &view, uint32_t row, F32Row &tile,
                  uint32_t column = 0) {
    view.validate();
    const uint32_t count = tile.GetValidCol();
    if (count > view.columns || column > view.columns - count) {
        throw std::invalid_argument("deepseek_pto_store_geometry");
    }
    auto destination = global_row<float>(view.at(row, column), count, view.column_stride);
    pto::TSTORE(destination, tile);
    ++operator_counts.tstore;
}

inline void load_scalar(F32Scalar &tile, const F32View &view, uint32_t row,
                        uint32_t column) {
    view.validate();
    auto source = global_row<float>(view.at(row, column), 1);
    pto::TLOAD(tile, source);
    ++operator_counts.tload;
}

inline void store_scalar(const F32View &view, uint32_t row, uint32_t column,
                         F32Scalar &tile) {
    view.validate();
    auto destination = global_row<float>(view.at(row, column), 1);
    pto::TSTORE(destination, tile);
    ++operator_counts.tstore;
}

inline void copy(const F32View &input, const F32View &output) {
    input.validate();
    output.validate();
    if (input.rows != output.rows || input.columns != output.columns) {
        throw std::invalid_argument("deepseek_pto_copy_geometry");
    }
    for (uint32_t row = 0; row < input.rows; ++row) {
        for (uint32_t column = 0; column < input.columns;
             column += kVectorCapacity) {
            const uint32_t count =
                std::min(kVectorCapacity, input.columns - column);
            F32Row values(count);
            load(values, input, row, column);
            store(output, row, values, column);
        }
    }
}

inline void rms_norm(const F32View &input, const F32View *weight,
                     const F32View &output, float epsilon) {
    ++operator_counts.rms_norm;
    input.validate();
    output.validate();
    if (input.rows != output.rows || input.columns != output.columns ||
        !std::isfinite(epsilon) || epsilon <= 0.0f ||
        (weight && (weight->rows != 1 || weight->columns != input.columns))) {
        throw std::invalid_argument("deepseek_pto_rms_norm_geometry");
    }
    F32Scalar total, partial, inv_rms, one;
    pto::TEXPANDS(total, 0.0f);
    pto::TEXPANDS(one, 1.0f);
    for (uint32_t row = 0; row < input.rows; ++row) {
        for (uint32_t column = 0; column < input.columns;
             column += kVectorCapacity) {
            const uint32_t count =
                std::min(kVectorCapacity, input.columns - column);
            F32Row values(count), squares(count), scratch(count);
            load(values, input, row, column);
            pto::TMUL(squares, values, values);
            pto::TROWSUM(partial, squares, scratch);
            pto::TADD(total, total, partial);
        }
    }
    const float elements = float(uint64_t(input.rows) * input.columns);
    pto::TMULS(inv_rms, total, 1.0f / elements);
    pto::TADDS(inv_rms, inv_rms, epsilon);
    pto::TSQRT(inv_rms, inv_rms);
    pto::TDIV(inv_rms, one, inv_rms);
    for (uint32_t row = 0; row < input.rows; ++row) {
        for (uint32_t column = 0; column < input.columns;
             column += kVectorCapacity) {
            const uint32_t count =
                std::min(kVectorCapacity, input.columns - column);
            F32Row values(count), scale(count), normalized(count);
            load(values, input, row, column);
            pto::TROWEXPAND(scale, inv_rms);
            pto::TMUL(normalized, values, scale);
            if (weight) {
                F32Row weights(count);
                load(weights, *weight, 0, column);
                pto::TMUL(normalized, normalized, weights);
            }
            store(output, row, normalized, column);
        }
    }
}

inline void head_rms_norm(const F32View &input, const F32View *weight,
                          const F32View &output, float epsilon) {
    ++operator_counts.head_rms_norm;
    input.validate();
    output.validate();
    if (input.rows != output.rows || input.columns != output.columns ||
        (weight && (weight->rows != 1 || weight->columns != input.columns))) {
        throw std::invalid_argument("deepseek_pto_head_rms_norm_geometry");
    }
    for (uint32_t head = 0; head < input.rows; ++head) {
        const F32View input_head {input.at(head), 1, input.columns,
                                  input.row_stride, input.column_stride};
        const F32View output_head {output.at(head), 1, output.columns,
                                   output.row_stride, output.column_stride};
        rms_norm(input_head, weight, output_head, epsilon);
    }
}

inline void rope_tail(const F32View &input, uint32_t rope_dim,
                      const F32View &cos, const F32View &sin, bool inverse,
                      const F32View &output) {
    ++operator_counts.rope_tail;
    input.validate();
    cos.validate();
    sin.validate();
    output.validate();
    const uint32_t pairs = rope_dim / 2;
    if (!rope_dim || rope_dim > input.columns || rope_dim % 2 ||
        pairs > kVectorCapacity || output.rows != input.rows ||
        output.columns != input.columns || cos.rows != 1 || sin.rows != 1 ||
        cos.columns != pairs || sin.columns != pairs) {
        throw std::invalid_argument("deepseek_pto_rope_tail_geometry");
    }
    const uint32_t tail_start = input.columns - rope_dim;
    for (uint32_t head = 0; head < input.rows; ++head) {
        for (uint32_t column = 0; column < tail_start;
             column += kVectorCapacity) {
            const uint32_t count =
                std::min(kVectorCapacity, tail_start - column);
            F32Row prefix(count);
            load(prefix, input, head, column);
            store(output, head, prefix, column);
        }

        const F32View even_input {input.at(head, tail_start), 1, pairs,
                                  input.row_stride, input.column_stride * 2};
        const F32View odd_input {input.at(head, tail_start + 1), 1, pairs,
                                 input.row_stride, input.column_stride * 2};
        const F32View even_output {output.at(head, tail_start), 1, pairs,
                                   output.row_stride,
                                   output.column_stride * 2};
        const F32View odd_output {output.at(head, tail_start + 1), 1, pairs,
                                  output.row_stride,
                                  output.column_stride * 2};
        F32Row even(pairs), odd(pairs), cosine(pairs), sine(pairs);
        F32Row even_cos(pairs), odd_sin(pairs), even_sin(pairs), odd_cos(pairs);
        F32Row rotated_even(pairs), rotated_odd(pairs);
        load(even, even_input, 0);
        load(odd, odd_input, 0);
        load(cosine, cos, 0);
        load(sine, sin, 0);
        if (inverse) pto::TMULS(sine, sine, -1.0f);
        pto::TMUL(even_cos, even, cosine);
        pto::TMUL(odd_sin, odd, sine);
        pto::TSUB(rotated_even, even_cos, odd_sin);
        pto::TMUL(even_sin, even, sine);
        pto::TMUL(odd_cos, odd, cosine);
        pto::TADD(rotated_odd, even_sin, odd_cos);
        store(even_output, 0, rotated_even);
        store(odd_output, 0, rotated_odd);
    }
}

inline void write_kv_row(const F32View &cache, uint32_t slot,
                         const F32View &kv) {
    ++operator_counts.kv_write;
    cache.validate();
    kv.validate();
    if (kv.rows != 1 || kv.columns != cache.columns || slot >= cache.rows) {
        throw std::invalid_argument("deepseek_pto_kv_write_geometry");
    }
    const F32View destination {cache.at(slot), 1, cache.columns,
                               cache.row_stride, cache.column_stride};
    copy(kv, destination);
}

inline void gather_kv_rows(const F32View &cache,
                           std::span<const uint32_t> row_indices,
                           const F32View &output) {
    ++operator_counts.kv_gather;
    cache.validate();
    output.validate();
    if (row_indices.empty() || output.rows != row_indices.size() ||
        output.columns != cache.columns) {
        throw std::invalid_argument("deepseek_pto_kv_gather_geometry");
    }
    for (uint32_t destination = 0; destination < output.rows;
         ++destination) {
        const uint32_t source = row_indices[destination];
        if (source >= cache.rows) {
            throw std::invalid_argument("deepseek_pto_kv_gather_row");
        }
        const F32View source_row {cache.at(source), 1, cache.columns,
                                  cache.row_stride, cache.column_stride};
        const F32View destination_row {
            output.at(destination), 1, output.columns, output.row_stride,
            output.column_stride};
        copy(source_row, destination_row);
    }
}

inline void sink_attention(const F32View &q, const F32View &kv_rows,
                           const F32View &sinks, const F32View &output) {
    ++operator_counts.sink_attention;
    q.validate();
    kv_rows.validate();
    sinks.validate();
    output.validate();
    if (q.rows != output.rows || q.columns != output.columns ||
        kv_rows.columns != q.columns || sinks.rows != 1 ||
        sinks.columns != q.rows) {
        throw std::invalid_argument("deepseek_pto_sink_attention_geometry");
    }

    const float scale = 1.0f / std::sqrt(float(q.columns));
    std::vector<float> score_storage(kv_rows.rows);
    const F32View scores {reinterpret_cast<uintptr_t>(score_storage.data()), 1,
                          kv_rows.rows, kv_rows.rows, 1};
    for (uint32_t head = 0; head < q.rows; ++head) {
        for (uint32_t row = 0; row < kv_rows.rows; ++row) {
            F32Scalar dot;
            pto::TEXPANDS(dot, 0.0f);
            for (uint32_t column = 0; column < q.columns;
                 column += kVectorCapacity) {
                const uint32_t count =
                    std::min(kVectorCapacity, q.columns - column);
                F32Row query(count), key(count), product(count), scratch(count);
                F32Scalar partial;
                load(query, q, head, column);
                load(key, kv_rows, row, column);
                pto::TMUL(product, query, key);
                pto::TROWSUM(partial, product, scratch);
                pto::TADD(dot, dot, partial);
            }
            pto::TMULS(dot, dot, scale);
            store_scalar(scores, 0, row, dot);
        }

        F32Scalar sink, maximum;
        load_scalar(sink, sinks, 0, head);
        pto::TMAX(maximum, sink, sink);
        for (uint32_t row = 0; row < kv_rows.rows;
             row += kVectorCapacity) {
            const uint32_t count =
                std::min(kVectorCapacity, kv_rows.rows - row);
            F32Row score_values(count), scratch(count);
            F32Scalar local_maximum;
            load(score_values, scores, 0, row);
            pto::TROWMAX(local_maximum, score_values, scratch);
            pto::TMAX(maximum, maximum, local_maximum);
        }

        F32Scalar sink_delta, denominator;
        pto::TSUB(sink_delta, sink, maximum);
        pto::TEXP(denominator, sink_delta);
        for (uint32_t row = 0; row < kv_rows.rows;
             row += kVectorCapacity) {
            const uint32_t count =
                std::min(kVectorCapacity, kv_rows.rows - row);
            F32Row score_values(count), expanded(count), weights(count),
                scratch(count);
            F32Scalar weight_sum;
            load(score_values, scores, 0, row);
            pto::TROWEXPAND(expanded, maximum);
            pto::TSUB(score_values, score_values, expanded);
            pto::TEXP(weights, score_values);
            pto::TROWSUM(weight_sum, weights, scratch);
            pto::TADD(denominator, denominator, weight_sum);
            store(scores, 0, weights, row);
        }

        F32Scalar one, inverse_denominator;
        pto::TEXPANDS(one, 1.0f);
        pto::TDIV(inverse_denominator, one, denominator);
        for (uint32_t column = 0; column < q.columns;
             column += kVectorCapacity) {
            const uint32_t count =
                std::min(kVectorCapacity, q.columns - column);
            F32Row accumulated(count), expanded(count), normalized(count);
            pto::TEXPANDS(accumulated, 0.0f);
            for (uint32_t row = 0; row < kv_rows.rows; ++row) {
                F32Scalar weight;
                F32Row values(count), weighted(count);
                load_scalar(weight, scores, 0, row);
                load(values, kv_rows, row, column);
                pto::TROWEXPAND(expanded, weight);
                pto::TMUL(weighted, values, expanded);
                pto::TADD(accumulated, accumulated, weighted);
            }
            pto::TROWEXPAND(expanded, inverse_denominator);
            pto::TMUL(normalized, accumulated, expanded);
            store(output, head, normalized, column);
        }
    }
}

struct CompressorStateView {
    F32View kv;
    F32View score;
    uint32_t head_dim;
    uint32_t compress_ratio;

    uint32_t width() const {
        return compress_ratio == 4 ? head_dim * 2 : head_dim;
    }

    void validate() const {
        kv.validate();
        score.validate();
        const uint32_t lanes = compress_ratio == 4 ? 2 : 1;
        const uint64_t rows = uint64_t(compress_ratio) * lanes;
        if (!head_dim || (compress_ratio != 4 && compress_ratio != 128) ||
            rows > std::numeric_limits<uint32_t>::max() ||
            kv.rows != rows || score.rows != rows || kv.columns != width() ||
            score.columns != width()) {
            throw std::invalid_argument("deepseek_pto_compressor_state_geometry");
        }
    }
};

inline void compressor_pool(const CompressorStateView &state,
                            const F32View &output) {
    state.validate();
    output.validate();
    if (output.rows != 1 || output.columns != state.head_dim) {
        throw std::invalid_argument("deepseek_pto_compressor_pool_geometry");
    }
    const uint32_t pair_count = state.compress_ratio == 4 ? 8 : 128;
    for (uint32_t dimension = 0; dimension < state.head_dim; ++dimension) {
        F32Scalar maximum;
        pto::TEXPANDS(maximum, -std::numeric_limits<float>::infinity());
        for (uint32_t pair = 0; pair < pair_count; ++pair) {
            const bool current_lane = state.compress_ratio == 4 && pair >= 4;
            const uint32_t row = pair;
            const uint32_t column =
                dimension + (current_lane ? state.head_dim : 0);
            F32Scalar score;
            load_scalar(score, state.score, row, column);
            pto::TMAX(maximum, maximum, score);
        }

        F32Scalar denominator, weighted_sum;
        pto::TEXPANDS(denominator, 0.0f);
        pto::TEXPANDS(weighted_sum, 0.0f);
        if (state.compress_ratio == 4) {
            for (uint32_t row = 0; row < 4; ++row) {
                F32Scalar previous_value, current_value;
                F32Scalar previous_score, current_score;
                F32Scalar previous_delta, current_delta;
                F32Scalar previous_weight, current_weight;
                F32Scalar pair_weight, weighted;
                load_scalar(previous_value, state.kv, row, dimension);
                load_scalar(previous_score, state.score, row, dimension);
                load_scalar(current_value, state.kv, 4 + row,
                            state.head_dim + dimension);
                load_scalar(current_score, state.score, 4 + row,
                            state.head_dim + dimension);
                pto::TSUB(previous_delta, previous_score, maximum);
                pto::TSUB(current_delta, current_score, maximum);
                pto::TEXP(previous_weight, previous_delta);
                pto::TEXP(current_weight, current_delta);
                pto::TADD(pair_weight, previous_weight, current_weight);
                pto::TADD(denominator, denominator, pair_weight);
                pto::TMUL(weighted, previous_value, previous_weight);
                pto::TADD(weighted_sum, weighted_sum, weighted);
                pto::TMUL(weighted, current_value, current_weight);
                pto::TADD(weighted_sum, weighted_sum, weighted);
            }
        } else {
            for (uint32_t row = 0; row < state.compress_ratio; ++row) {
                F32Scalar value, score, delta, weight, weighted;
                load_scalar(value, state.kv, row, dimension);
                load_scalar(score, state.score, row, dimension);
                pto::TSUB(delta, score, maximum);
                pto::TEXP(weight, delta);
                pto::TADD(denominator, denominator, weight);
                pto::TMUL(weighted, value, weight);
                pto::TADD(weighted_sum, weighted_sum, weighted);
            }
        }
        F32Scalar pooled;
        pto::TDIV(pooled, weighted_sum, denominator);
        store_scalar(output, 0, dimension, pooled);
    }
}

inline bool compressor_update_pre_scored(uint32_t position,
                                         const F32View &projected_kv,
                                         const F32View &projected_score,
                                         const CompressorStateView &state,
                                         const F32View &pooled_output) {
    ++operator_counts.compressor_update;
    state.validate();
    projected_kv.validate();
    projected_score.validate();
    pooled_output.validate();
    if (projected_kv.rows != 1 || projected_score.rows != 1 ||
        projected_kv.columns != state.width() ||
        projected_score.columns != state.width() ||
        pooled_output.rows != 1 ||
        pooled_output.columns != state.head_dim) {
        throw std::invalid_argument("deepseek_pto_compressor_update_geometry");
    }
    const uint32_t position_mod = position % state.compress_ratio;
    const uint32_t row = state.compress_ratio == 4
                             ? state.compress_ratio + position_mod
                             : position_mod;
    const F32View kv_destination {state.kv.at(row), 1, state.width(),
                                  state.kv.row_stride,
                                  state.kv.column_stride};
    const F32View score_destination {state.score.at(row), 1, state.width(),
                                     state.score.row_stride,
                                     state.score.column_stride};
    copy(projected_kv, kv_destination);
    copy(projected_score, score_destination);
    if ((uint64_t(position) + 1) % state.compress_ratio != 0) return false;

    compressor_pool(state, pooled_output);
    if (state.compress_ratio == 4) {
        for (uint32_t row_index = 0; row_index < 4; ++row_index) {
            const F32View current_kv {state.kv.at(4 + row_index), 1,
                                      state.width(), state.kv.row_stride,
                                      state.kv.column_stride};
            const F32View previous_kv {state.kv.at(row_index), 1,
                                       state.width(), state.kv.row_stride,
                                       state.kv.column_stride};
            const F32View current_score {
                state.score.at(4 + row_index), 1, state.width(),
                state.score.row_stride, state.score.column_stride};
            const F32View previous_score {
                state.score.at(row_index), 1, state.width(),
                state.score.row_stride, state.score.column_stride};
            copy(current_kv, previous_kv);
            copy(current_score, previous_score);
        }
        for (uint32_t row_index = 0; row_index < 4; ++row_index) {
            const F32View previous_kv {state.kv.at(row_index), 1,
                                       state.width(), state.kv.row_stride,
                                       state.kv.column_stride};
            const F32View current_kv {state.kv.at(4 + row_index), 1,
                                      state.width(), state.kv.row_stride,
                                      state.kv.column_stride};
            const F32View previous_score {
                state.score.at(row_index), 1, state.width(),
                state.score.row_stride, state.score.column_stride};
            const F32View current_score {
                state.score.at(4 + row_index), 1, state.width(),
                state.score.row_stride, state.score.column_stride};
            copy(previous_kv, current_kv);
            copy(previous_score, current_score);
        }
    }
    return true;
}

inline void fp4_e2m1_roundtrip(F32Row &block, float scale) {
    if (block.GetValidCol() != 32 || !std::isfinite(scale) || scale <= 0.0f) {
        throw std::invalid_argument("deepseek_pto_fp4_roundtrip_geometry");
    }
    F32Row magnitude(32), selected_a(32), selected_b(32);
    F32Row negative(32), signed_values(32), scratch(32);
    U8Block mask(4);
    pto::TABS(magnitude, block);
    pto::TDIVS(magnitude, magnitude, scale);
    pto::TMINS(magnitude, magnitude, 6.0f);
    pto::TEXPANDS(selected_a, 0.0f);
    F32Row *selected = &selected_a;
    F32Row *next = &selected_b;
    constexpr std::array<float, 8> fp4_values {
        0.0f, 0.5f, 1.0f, 1.5f, 2.0f, 3.0f, 4.0f, 6.0f};
    for (uint32_t upper = 1; upper < fp4_values.size(); ++upper) {
        const float midpoint =
            0.5f * (fp4_values[upper - 1] + fp4_values[upper]);
        const pto::CmpMode keep =
            upper % 2 == 0 ? pto::CmpMode::LT : pto::CmpMode::LE;
        pto::TCMPS(mask, magnitude, midpoint, keep);
        pto::TSELS(*next, mask, *selected, scratch, fp4_values[upper]);
        std::swap(selected, next);
    }
    pto::TCMPS(mask, block, 0.0f, pto::CmpMode::LT);
    pto::TNEG(negative, *selected);
    pto::TSEL(signed_values, mask, negative, *selected, scratch);
    pto::TMULS(block, signed_values, scale);
}

inline void indexer_qat(const F32View &values) {
    ++operator_counts.indexer_qat;
    values.validate();
    if (values.columns != 128) {
        throw std::invalid_argument("deepseek_pto_indexer_qat_geometry");
    }
    for (uint32_t head = 0; head < values.rows; ++head) {
        for (uint32_t stride = 1; stride < values.columns; stride *= 2) {
            for (uint32_t base = 0; base < values.columns;
                 base += 2 * stride) {
                for (uint32_t index = 0; index < stride; ++index) {
                    F32Scalar a, b, sum, difference;
                    load_scalar(a, values, head, base + index);
                    load_scalar(b, values, head, base + stride + index);
                    pto::TADD(sum, a, b);
                    pto::TSUB(difference, a, b);
                    store_scalar(values, head, base + index, sum);
                    store_scalar(values, head, base + stride + index,
                                 difference);
                }
            }
        }
        F32Row normalized(values.columns);
        load(normalized, values, head);
        pto::TMULS(normalized, normalized, 0.08838834764831845f);
        store(values, head, normalized);

        for (uint32_t column = 0; column < values.columns; column += 32) {
            F32Row block(32), absolute(32), scratch_tile(32);
            F32Scalar maximum;
            load(block, values, head, column);
            pto::TABS(absolute, block);
            pto::TROWMAX(maximum, absolute, scratch_tile);
            float absolute_maximum = 0.0f;
            const F32View maximum_view {
                reinterpret_cast<uintptr_t>(&absolute_maximum), 1, 1, 1, 1};
            store_scalar(maximum_view, 0, 0, maximum);
            const float bounded_maximum =
                std::max(absolute_maximum, 7.052966104933725e-38f);
            const float scale =
                std::exp2(std::ceil(std::log2(bounded_maximum / 6.0f)));
            fp4_e2m1_roundtrip(block, scale);
            store(values, head, block, column);
        }
    }
}

inline std::vector<uint32_t>
sparse_indexer(const F32View &q, const F32View &compressed_kv,
               const F32View &projected_head_weights, uint32_t top_k) {
    ++operator_counts.sparse_indexer;
    q.validate();
    compressed_kv.validate();
    projected_head_weights.validate();
    if (!top_k || compressed_kv.columns != q.columns ||
        projected_head_weights.rows != 1 ||
        projected_head_weights.columns != q.rows) {
        throw std::invalid_argument("deepseek_pto_sparse_indexer_geometry");
    }
    const uint32_t selection_count = std::min(top_k, compressed_kv.rows);
    if (selection_count == compressed_kv.rows) {
        std::vector<uint32_t> all(compressed_kv.rows);
        for (uint32_t row = 0; row < compressed_kv.rows; ++row) all[row] = row;
        return all;
    }
    ++operator_counts.sparse_indexer_topk;

    std::vector<float> score_storage(compressed_kv.rows);
    const F32View scores {reinterpret_cast<uintptr_t>(score_storage.data()), 1,
                          compressed_kv.rows, compressed_kv.rows, 1};
    const float scale =
        1.0f / std::sqrt(float(uint64_t(q.rows) * q.columns));
    F32Scalar zero;
    pto::TEXPANDS(zero, 0.0f);
    for (uint32_t row = 0; row < compressed_kv.rows; ++row) {
        F32Scalar score;
        pto::TEXPANDS(score, 0.0f);
        for (uint32_t head = 0; head < q.rows; ++head) {
            F32Scalar dot;
            pto::TEXPANDS(dot, 0.0f);
            for (uint32_t column = 0; column < q.columns;
                 column += kVectorCapacity) {
                const uint32_t count =
                    std::min(kVectorCapacity, q.columns - column);
                F32Row query(count), key(count), product(count), scratch(count);
                F32Scalar partial;
                load(query, q, head, column);
                load(key, compressed_kv, row, column);
                pto::TMUL(product, query, key);
                pto::TROWSUM(partial, product, scratch);
                pto::TADD(dot, dot, partial);
            }
            pto::TMAX(dot, dot, zero);
            F32Scalar head_weight, weighted;
            load_scalar(head_weight, projected_head_weights, 0, head);
            pto::TMUL(weighted, dot, head_weight);
            pto::TADD(score, score, weighted);
        }
        pto::TMULS(score, score, scale);
        store_scalar(scores, 0, row, score);
    }

    std::vector<uint32_t> selected;
    selected.reserve(selection_count);
    std::vector<bool> used(compressed_kv.rows, false);
    for (uint32_t selection = 0; selection < selection_count; ++selection) {
        uint32_t best_row = compressed_kv.rows;
        float best_score = -std::numeric_limits<float>::infinity();
        for (uint32_t row = 0; row < compressed_kv.rows; ++row) {
            if (!used[row] && score_storage[row] > best_score) {
                best_row = row;
                best_score = score_storage[row];
            }
        }
        if (best_row == compressed_kv.rows) {
            throw std::runtime_error("deepseek_pto_sparse_indexer_selection");
        }
        used[best_row] = true;
        selected.push_back(best_row);
    }
    return selected;
}

inline void sigmoid_affine(const F32View &mix, const F32View &base,
                           uint32_t input_offset, uint32_t count,
                           F32Scalar &scale, float multiplier, float epsilon,
                           const F32View &output, uint32_t output_offset) {
    F32Row values(count), base_values(count), expanded(count), denominator(count),
        numerator(count), result(count);
    load(values, mix, 0, input_offset);
    load(base_values, base, 0, input_offset);
    pto::TROWEXPAND(expanded, scale);
    pto::TMUL(values, values, expanded);
    pto::TADD(values, values, base_values);
    pto::TMULS(denominator, values, -1.0f);
    pto::TEXP(denominator, denominator);
    pto::TADDS(denominator, denominator, 1.0f);
    pto::TEXPANDS(numerator, 1.0f);
    pto::TDIV(result, numerator, denominator);
    if (multiplier != 1.0f) pto::TMULS(result, result, multiplier);
    if (epsilon != 0.0f) pto::TADDS(result, result, epsilon);
    store(output, 0, result, output_offset);
}

inline void normalize_sum(const F32View &view, float epsilon) {
    if (view.rows != 1 || view.columns > kVectorCapacity) {
        throw std::invalid_argument("deepseek_pto_normalize_sum_geometry");
    }
    F32Row values(view.columns), scratch(view.columns), expanded(view.columns);
    F32Scalar sum, inverse, one;
    load(values, view, 0);
    pto::TROWSUM(sum, values, scratch);
    pto::TADDS(sum, sum, epsilon);
    pto::TEXPANDS(one, 1.0f);
    pto::TDIV(inverse, one, sum);
    pto::TROWEXPAND(expanded, inverse);
    pto::TMUL(values, values, expanded);
    store(view, 0, values);
}

inline void hc_split(const F32View &mix, const F32View &scale,
                     const F32View &base, uint32_t hc_mult,
                     uint32_t sinkhorn_iterations, float epsilon,
                     const F32View &pre, const F32View &post,
                     const F32View &combine) {
    ++operator_counts.hc_split;
    mix.validate();
    scale.validate();
    base.validate();
    pre.validate();
    post.validate();
    combine.validate();
    const uint64_t mix_columns = uint64_t(2 + hc_mult) * hc_mult;
    if (!hc_mult || hc_mult > 8 || !sinkhorn_iterations ||
        !std::isfinite(epsilon) || epsilon <= 0.0f || mix.rows != 1 ||
        base.rows != 1 || mix.columns != mix_columns ||
        base.columns != mix_columns || scale.rows != 1 || scale.columns != 3 ||
        pre.rows != 1 || pre.columns != hc_mult || post.rows != 1 ||
        post.columns != hc_mult || combine.rows != hc_mult ||
        combine.columns != hc_mult) {
        throw std::invalid_argument("deepseek_pto_hc_split_geometry");
    }
    F32Scalar scale_pre, scale_post, scale_combine;
    load_scalar(scale_pre, scale, 0, 0);
    load_scalar(scale_post, scale, 0, 1);
    load_scalar(scale_combine, scale, 0, 2);
    sigmoid_affine(mix, base, 0, hc_mult, scale_pre, 1.0f, epsilon,
                   pre, 0);
    sigmoid_affine(mix, base, hc_mult, hc_mult, scale_post, 2.0f, 0.0f,
                   post, 0);

    const uint32_t combine_offset = 2 * hc_mult;
    for (uint32_t destination = 0; destination < hc_mult; ++destination) {
        F32Row values(hc_mult), base_values(hc_mult), expanded(hc_mult),
            exponent(hc_mult), scratch(hc_mult);
        F32Scalar maximum, sum;
        const uint32_t offset = combine_offset + destination * hc_mult;
        load(values, mix, 0, offset);
        load(base_values, base, 0, offset);
        pto::TROWEXPAND(expanded, scale_combine);
        pto::TMUL(values, values, expanded);
        pto::TADD(values, values, base_values);
        pto::TROWMAX(maximum, values, scratch);
        pto::TROWEXPAND(expanded, maximum);
        pto::TSUB(exponent, values, expanded);
        pto::TEXP(exponent, exponent);
        pto::TROWSUM(sum, exponent, scratch);
        pto::TROWEXPAND(expanded, sum);
        pto::TDIV(values, exponent, expanded);
        pto::TADDS(values, values, epsilon);
        store(combine, destination, values);
    }

    const auto normalize_columns = [&]() {
        for (uint32_t source = 0; source < hc_mult; ++source) {
            F32View column {combine.at(0, source), 1, hc_mult, hc_mult,
                            hc_mult};
            normalize_sum(column, epsilon);
        }
    };
    const auto normalize_rows = [&]() {
        for (uint32_t destination = 0; destination < hc_mult; ++destination) {
            F32View row {combine.at(destination), 1, hc_mult, hc_mult, 1};
            normalize_sum(row, epsilon);
        }
    };
    normalize_columns();
    for (uint32_t iteration = 1; iteration < sinkhorn_iterations;
         ++iteration) {
        normalize_rows();
        normalize_columns();
    }
}

inline void hc_weighted_sum(const F32View &residual_hc,
                            const F32View &weights,
                            const F32View &output) {
    ++operator_counts.hc_weighted_sum;
    residual_hc.validate();
    weights.validate();
    output.validate();
    if (weights.rows != 1 || weights.columns != residual_hc.rows ||
        output.rows != 1 || output.columns != residual_hc.columns) {
        throw std::invalid_argument("deepseek_pto_hc_weighted_sum_geometry");
    }
    for (uint32_t column = 0; column < residual_hc.columns;
         column += kVectorCapacity) {
        const uint32_t count =
            std::min(kVectorCapacity, residual_hc.columns - column);
        F32Row accumulated(count);
        pto::TEXPANDS(accumulated, 0.0f);
        for (uint32_t source = 0; source < residual_hc.rows; ++source) {
            F32Scalar scalar;
            F32Row values(count), expanded(count), weighted(count);
            load_scalar(scalar, weights, 0, source);
            load(values, residual_hc, source, column);
            pto::TROWEXPAND(expanded, scalar);
            pto::TMUL(weighted, values, expanded);
            pto::TADD(accumulated, accumulated, weighted);
        }
        store(output, 0, accumulated, column);
    }
}

inline void hc_post(const F32View &sublayer, const F32View &residual_hc,
                    const F32View &post, const F32View &combine,
                    const F32View &output) {
    ++operator_counts.hc_post;
    sublayer.validate();
    residual_hc.validate();
    post.validate();
    combine.validate();
    output.validate();
    const uint32_t hc = residual_hc.rows;
    if (sublayer.rows != 1 || sublayer.columns != residual_hc.columns ||
        post.rows != 1 || post.columns != hc || combine.rows != hc ||
        combine.columns != hc || output.rows != hc ||
        output.columns != residual_hc.columns) {
        throw std::invalid_argument("deepseek_pto_hc_post_geometry");
    }
    for (uint32_t destination = 0; destination < hc; ++destination) {
        F32Scalar post_scalar;
        load_scalar(post_scalar, post, 0, destination);
        for (uint32_t column = 0; column < sublayer.columns;
             column += kVectorCapacity) {
            const uint32_t count =
                std::min(kVectorCapacity, sublayer.columns - column);
            F32Row sublayer_values(count), expanded(count), accumulated(count);
            load(sublayer_values, sublayer, 0, column);
            pto::TROWEXPAND(expanded, post_scalar);
            pto::TMUL(accumulated, sublayer_values, expanded);
            for (uint32_t source = 0; source < hc; ++source) {
                F32Scalar combine_scalar;
                F32Row residual(count), weighted(count);
                // DS4 HC post consumes combine transposed: [source, destination].
                load_scalar(combine_scalar, combine, source, destination);
                load(residual, residual_hc, source, column);
                pto::TROWEXPAND(expanded, combine_scalar);
                pto::TMUL(weighted, residual, expanded);
                pto::TADD(accumulated, accumulated, weighted);
            }
            store(output, destination, accumulated, column);
        }
    }
}

inline void residual(const F32View &left, const F32View &right,
                     const F32View &output) {
    ++operator_counts.residual;
    left.validate();
    right.validate();
    output.validate();
    if (left.rows != right.rows || left.columns != right.columns ||
        left.rows != output.rows || left.columns != output.columns) {
        throw std::invalid_argument("deepseek_pto_residual_geometry");
    }
    for (uint32_t row = 0; row < left.rows; ++row) {
        for (uint32_t column = 0; column < left.columns;
             column += kVectorCapacity) {
            const uint32_t count =
                std::min(kVectorCapacity, left.columns - column);
            F32Row a(count), b(count), sum(count);
            load(a, left, row, column);
            load(b, right, row, column);
            pto::TADD(sum, a, b);
            store(output, row, sum, column);
        }
    }
}

inline void fill(const F32View &output, float value) {
    output.validate();
    for (uint32_t row = 0; row < output.rows; ++row) {
        for (uint32_t column = 0; column < output.columns;
             column += kVectorCapacity) {
            const uint32_t count =
                std::min(kVectorCapacity, output.columns - column);
            F32Row values(count);
            pto::TEXPANDS(values, value);
            store(output, row, values, column);
        }
    }
}

inline void scale_in_place(const F32View &values, float scale) {
    values.validate();
    if (!std::isfinite(scale)) {
        throw std::invalid_argument("deepseek_pto_scale_non_finite");
    }
    for (uint32_t row = 0; row < values.rows; ++row) {
        for (uint32_t column = 0; column < values.columns;
             column += kVectorCapacity) {
            const uint32_t count =
                std::min(kVectorCapacity, values.columns - column);
            F32Row tile(count);
            load(tile, values, row, column);
            pto::TMULS(tile, tile, scale);
            store(values, row, tile, column);
        }
    }
}

inline void swiglu(const F32View &gate, const F32View &up,
                   const F32View &output, float clamp) {
    ++operator_counts.swiglu;
    gate.validate();
    up.validate();
    output.validate();
    if (gate.rows != up.rows || gate.columns != up.columns ||
        gate.rows != output.rows || gate.columns != output.columns ||
        !std::isfinite(clamp) || clamp < 0.0f) {
        throw std::invalid_argument("deepseek_pto_swiglu_geometry");
    }
    for (uint32_t row = 0; row < gate.rows; ++row) {
        for (uint32_t column = 0; column < gate.columns;
             column += kVectorCapacity) {
            const uint32_t count =
                std::min(kVectorCapacity, gate.columns - column);
            F32Row gate_values(count), up_values(count), denominator(count),
                result(count);
            load(gate_values, gate, row, column);
            load(up_values, up, row, column);
            if (clamp > 1.0e-6f) {
                pto::TMINS(gate_values, gate_values, clamp);
                pto::TMINS(up_values, up_values, clamp);
                pto::TMAXS(up_values, up_values, -clamp);
            }
            pto::TMULS(denominator, gate_values, -1.0f);
            pto::TEXP(denominator, denominator);
            pto::TADDS(denominator, denominator, 1.0f);
            pto::TDIV(result, gate_values, denominator);
            pto::TMUL(result, result, up_values);
            store(output, row, result, column);
        }
    }
}

struct RouterSelection {
    std::vector<uint32_t> expert_indices;
    std::vector<float> expert_weights;
};

inline RouterSelection router(const F32View &logits,
                              const F32View *selection_bias,
                              std::span<const uint32_t> selected_experts,
                              uint32_t top_k, float weight_scale,
                              const F32View &probabilities) {
    ++operator_counts.router;
    logits.validate();
    probabilities.validate();
    if (logits.rows != 1 || probabilities.rows != 1 ||
        probabilities.columns != logits.columns || !top_k ||
        top_k > logits.columns || !std::isfinite(weight_scale) ||
        weight_scale <= 0.0f ||
        (selection_bias &&
         (selection_bias->rows != 1 ||
          selection_bias->columns != logits.columns)) ||
        (!selected_experts.empty() && selected_experts.size() != top_k)) {
        throw std::invalid_argument("deepseek_pto_router_geometry");
    }

    for (uint32_t column = 0; column < logits.columns;
         column += kVectorCapacity) {
        const uint32_t count =
            std::min(kVectorCapacity, logits.columns - column);
        F32Row values(count), positive(count), absolute(count), negative(count);
        F32Row exponential(count), denominator(count), ratio(count);
        F32Row ratio_squared(count), power(count), term(count), sum(count);
        F32Row result(count);
        load(values, logits, 0, column);
        pto::TMAXS(positive, values, 0.0f);
        pto::TABS(absolute, values);
        pto::TNEG(negative, absolute);
        pto::TEXP(exponential, negative);
        pto::TADDS(denominator, exponential, 2.0f);
        pto::TDIV(ratio, exponential, denominator);
        pto::TMUL(ratio_squared, ratio, ratio);
        pto::TADDS(power, ratio, 0.0f);
        pto::TADDS(sum, ratio, 0.0f);
        for (uint32_t degree = 3; degree <= 15; degree += 2) {
            pto::TMUL(power, power, ratio_squared);
            pto::TDIVS(term, power, static_cast<float>(degree));
            pto::TADD(sum, sum, term);
        }
        pto::TMULS(sum, sum, 2.0f);
        pto::TADD(result, positive, sum);
        pto::TSQRT(result, result);
        store(probabilities, 0, result, column);
    }

    RouterSelection selection;
    selection.expert_indices.reserve(top_k);
    if (!selected_experts.empty()) {
        for (uint32_t expert : selected_experts) {
            if (expert >= logits.columns) {
                throw std::invalid_argument("deepseek_pto_hash_router_expert");
            }
            selection.expert_indices.push_back(expert);
        }
    } else {
        std::vector<float> scores(logits.columns);
        const F32View score_view {reinterpret_cast<uintptr_t>(scores.data()), 1,
                                  logits.columns, logits.columns, 1};
        for (uint32_t column = 0; column < logits.columns;
             column += kVectorCapacity) {
            const uint32_t count =
                std::min(kVectorCapacity, logits.columns - column);
            F32Row values(count);
            load(values, probabilities, 0, column);
            if (selection_bias) {
                F32Row bias(count);
                load(bias, *selection_bias, 0, column);
                pto::TADD(values, values, bias);
            }
            store(score_view, 0, values, column);
        }
        F32Row scores_tile(logits.columns);
        U32Row argmax_scratch(logits.columns);
        for (uint32_t slot = 0; slot < top_k; ++slot) {
            U32Scalar argmax;
            load(scores_tile, score_view, 0);
            pto::TROWARGMAX(argmax, scores_tile, argmax_scratch);
            ++operator_counts.router_argmax;
            uint32_t best_expert = logits.columns;
            auto index_output = global_row<uint32_t>(
                reinterpret_cast<uintptr_t>(&best_expert), 1);
            pto::TSTORE(index_output, argmax);
            ++operator_counts.tstore;
            if (best_expert >= logits.columns) {
                throw std::runtime_error("deepseek_pto_router_selection");
            }
            selection.expert_indices.push_back(best_expert);
            if (slot + 1 < top_k) {
                F32Scalar masked;
                pto::TEXPANDS(masked,
                              -std::numeric_limits<float>::infinity());
                auto masked_score = global_row<float>(
                    score_view.at(0, best_expert), 1);
                pto::TSTORE(masked_score, masked);
                ++operator_counts.tstore;
            }
        }
    }

    F32Scalar probability_sum;
    pto::TEXPANDS(probability_sum, 0.0f);
    for (uint32_t expert : selection.expert_indices) {
        F32Scalar probability;
        load_scalar(probability, probabilities, 0, expert);
        pto::TADD(probability_sum, probability_sum, probability);
    }
    F32Scalar floor;
    pto::TEXPANDS(floor, 6.1035156e-5f);
    pto::TMAX(probability_sum, probability_sum, floor);
    selection.expert_weights.resize(top_k);
    const F32View weight_view {
        reinterpret_cast<uintptr_t>(selection.expert_weights.data()), 1, top_k,
        top_k, 1};
    for (uint32_t slot = 0; slot < top_k; ++slot) {
        F32Scalar probability, normalized;
        load_scalar(probability, probabilities, 0,
                    selection.expert_indices[slot]);
        pto::TDIV(normalized, probability, probability_sum);
        pto::TMULS(normalized, normalized, weight_scale);
        store_scalar(weight_view, 0, slot, normalized);
    }
    return selection;
}

inline float load_f16_scalar(const uint8_t *source) {
    F16Scalar encoded;
    F32Scalar decoded;
    float value = 0.0f;
    auto input = global_row<half>(reinterpret_cast<uintptr_t>(source), 1);
    auto output = global_row<float>(reinterpret_cast<uintptr_t>(&value), 1);
    pto::TLOAD(encoded, input);
    pto::TCVT(decoded, encoded, pto::RoundMode::CAST_NONE);
    pto::TSTORE(output, decoded);
    ++operator_counts.tload;
    ++operator_counts.tstore;
    if (!std::isfinite(value)) {
        throw std::invalid_argument("deepseek_pto_q8_scale_non_finite");
    }
    return value;
}

inline void validate_f16_matrix(const WeightTensor &weight,
                                uint32_t input_columns,
                                uint32_t output_columns) {
    if (weight.tensor_type != kGgufTypeF16 || weight.rank != 2 ||
        weight.dimensions[0] != input_columns ||
        weight.dimensions[1] != output_columns || !input_columns ||
        !output_columns ||
        weight.bytes != uint64_t(input_columns) * output_columns * 2) {
        throw std::invalid_argument("deepseek_pto_f16_geometry");
    }
}

inline void f16_linear(const F32View &input, const WeightTensor &weight,
                       const F32View &output) {
    ++operator_counts.f16_linear;
    input.validate();
    output.validate();
    if (input.rows != output.rows) {
        throw std::invalid_argument("deepseek_pto_f16_rows");
    }
    validate_f16_matrix(weight, input.columns, output.columns);
    for (uint32_t input_row = 0; input_row < input.rows; ++input_row) {
        for (uint32_t output_row = 0; output_row < output.columns;
             ++output_row) {
            F32Scalar accumulator;
            pto::TEXPANDS(accumulator, 0.0f);
            for (uint32_t column = 0; column < input.columns;
                 column += kVectorCapacity) {
                const uint32_t count =
                    std::min(kVectorCapacity, input.columns - column);
                F32Row activation(count), decoded_weight(count), product(count),
                    scratch(count);
                F16Row encoded_weight(count);
                F32Scalar partial;
                load(activation, input, input_row, column);
                const uint64_t element =
                    uint64_t(output_row) * input.columns + column;
                auto weight_source = global_row<half>(
                    reinterpret_cast<uintptr_t>(weight.payload + element * 2),
                    count);
                pto::TLOAD(encoded_weight, weight_source);
                ++operator_counts.tload;
                pto::TCVT(decoded_weight, encoded_weight,
                          pto::RoundMode::CAST_NONE);
                pto::TMUL(product, activation, decoded_weight);
                pto::TROWSUM(partial, product, scratch);
                pto::TADD(accumulator, accumulator, partial);
            }
            auto destination =
                global_row<float>(output.at(input_row, output_row), 1);
            pto::TSTORE(destination, accumulator);
            ++operator_counts.tstore;
        }
    }
}

inline void decode_f16_vector(const WeightTensor &weight,
                              const F32View &output) {
    output.validate();
    if (weight.tensor_type != kGgufTypeF16 || weight.rank != 1 ||
        output.rows != 1 || weight.dimensions[0] != output.columns ||
        weight.bytes != uint64_t(output.columns) * 2) {
        throw std::invalid_argument("deepseek_pto_f16_vector_geometry");
    }
    for (uint32_t column = 0; column < output.columns;
         column += kVectorCapacity) {
        const uint32_t count =
            std::min(kVectorCapacity, output.columns - column);
        F16Row encoded(count);
        F32Row decoded(count);
        auto source = global_row<half>(
            reinterpret_cast<uintptr_t>(weight.payload + uint64_t(column) * 2),
            count);
        pto::TLOAD(encoded, source);
        ++operator_counts.tload;
        pto::TCVT(decoded, encoded, pto::RoundMode::CAST_NONE);
        store(output, 0, decoded, column);
    }
}

inline void q8_0_linear(const F32View &input, const WeightTensor &weight,
                        const F32View &output,
                        const F32View *trace_output = nullptr);

inline void grouped_q8_0_linear(const F32View &input,
                                const WeightTensor &weight, uint32_t groups,
                                const F32View &output) {
    ++operator_counts.grouped_q8_linear;
    input.validate();
    output.validate();
    if (!groups || input.rows != output.rows || weight.rank != 2 ||
        weight.tensor_type != kGgufTypeQ8_0 ||
        weight.dimensions[0] * groups != input.columns ||
        weight.dimensions[1] != output.columns ||
        output.columns % groups) {
        throw std::invalid_argument("deepseek_pto_grouped_q8_geometry");
    }
    const uint32_t group_input = static_cast<uint32_t>(weight.dimensions[0]);
    const uint32_t group_output = output.columns / groups;
    const uint64_t row_bytes =
        uint64_t(group_input / kQ8BlockElements) * kQ8BlockBytes;
    const uint64_t group_bytes = uint64_t(group_output) * row_bytes;
    if (!group_input || group_input % kQ8BlockElements ||
        weight.bytes != uint64_t(groups) * group_bytes) {
        throw std::invalid_argument("deepseek_pto_grouped_q8_payload");
    }
    for (uint32_t row = 0; row < input.rows; ++row) {
        for (uint32_t group = 0; group < groups; ++group) {
            const F32View group_input_view {
                input.at(row, group * group_input), 1, group_input,
                input.row_stride, input.column_stride};
            const F32View group_output_view {
                output.at(row, group * group_output), 1, group_output,
                output.row_stride, output.column_stride};
            WeightTensor group_weight = weight;
            group_weight.payload += uint64_t(group) * group_bytes;
            group_weight.bytes = group_bytes;
            group_weight.elements = uint64_t(group_input) * group_output;
            group_weight.dimensions[0] = group_input;
            group_weight.dimensions[1] = group_output;
            q8_0_linear(group_input_view, group_weight, group_output_view);
        }
    }
}

inline void e4m3fn_roundtrip(F32Row &block, float scale) {
    if (block.GetValidCol() != 64 || !std::isfinite(scale) || scale <= 0.0f) {
        throw std::invalid_argument("deepseek_pto_fp8_roundtrip_geometry");
    }
    F32Row magnitude(64), base_a(64), base_b(64), step_a(64), step_b(64);
    F32Row scaled(64), rounded(64), quantized(64), negative(64);
    F32Row signed_values(64), scratch(64);
    I32Row code(64);
    U8Block mask(8);
    pto::TABS(magnitude, block);
    pto::TDIVS(magnitude, magnitude, scale);
    pto::TMINS(magnitude, magnitude, 448.0f);
    pto::TEXPANDS(base_a, 0.0f);
    pto::TEXPANDS(step_a, 0.001953125f);
    F32Row *base = &base_a;
    F32Row *next_base = &base_b;
    F32Row *step = &step_a;
    F32Row *next_step = &step_b;
    for (uint32_t exponent = 1; exponent <= 15; ++exponent) {
        const float exponent_base = std::ldexp(1.0f, int(exponent) - 7);
        const float cutoff = exponent_base *
                             (exponent == 1 ? 0.9375f : 0.96875f);
        pto::TCMPS(mask, magnitude, cutoff, pto::CmpMode::LT);
        pto::TSELS(*next_base, mask, *base, scratch, exponent_base);
        pto::TSELS(*next_step, mask, *step, scratch,
                   exponent_base * 0.125f);
        std::swap(base, next_base);
        std::swap(step, next_step);
    }
    pto::TSUB(scaled, magnitude, *base);
    pto::TDIV(scaled, scaled, *step);
    pto::TCVT(code, scaled, pto::RoundMode::CAST_RINT);
    pto::TCVT(rounded, code, pto::RoundMode::CAST_NONE);
    pto::TMUL(rounded, rounded, *step);
    pto::TADD(quantized, *base, rounded);
    pto::TCMPS(mask, block, 0.0f, pto::CmpMode::LT);
    pto::TNEG(negative, quantized);
    pto::TSEL(signed_values, mask, negative, quantized, scratch);
    pto::TMULS(block, signed_values, scale);
}

inline void fp8_kv_roundtrip(const F32View &values, uint32_t rope_dim) {
    ++operator_counts.fp8_kv_roundtrip;
    values.validate();
    if (rope_dim > values.columns || (values.columns - rope_dim) % 64) {
        throw std::invalid_argument("deepseek_pto_fp8_kv_geometry");
    }
    const uint32_t nope_dim = values.columns - rope_dim;
    for (uint32_t row = 0; row < values.rows; ++row) {
        for (uint32_t column = 0; column < nope_dim; column += 64) {
            F32Row block(64), absolute(64), scratch_tile(64);
            F32Scalar maximum;
            load(block, values, row, column);
            pto::TABS(absolute, block);
            pto::TROWMAX(maximum, absolute, scratch_tile);
            float absolute_maximum = 0.0f;
            const F32View maximum_view {
                reinterpret_cast<uintptr_t>(&absolute_maximum), 1, 1, 1, 1};
            store_scalar(maximum_view, 0, 0, maximum);
            const float scale =
                std::exp2(std::ceil(std::log2(
                    std::max(absolute_maximum, 1.0e-4f) / 448.0f)));
            e4m3fn_roundtrip(block, scale);
            store(values, row, block, column);
        }
    }
}

inline void f16_roundtrip(const F32View &values) {
    ++operator_counts.f16_roundtrip;
    values.validate();
    for (uint32_t row = 0; row < values.rows; ++row) {
        for (uint32_t column = 0; column < values.columns;
             column += kVectorCapacity) {
            const uint32_t count =
                std::min(kVectorCapacity, values.columns - column);
            F32Row input(count), output(count);
            F16Row encoded(count);
            load(input, values, row, column);
            pto::TCVT(encoded, input, pto::RoundMode::CAST_RINT);
            pto::TCVT(output, encoded, pto::RoundMode::CAST_NONE);
            store(values, row, output, column);
        }
    }
}

struct QuantizedQ8Block {
    std::array<int8_t, kQ8BlockElements> values {};
    float scale = 0.0f;
};

inline QuantizedQ8Block quantize_q8_0(const F32View &input, uint32_t row,
                                      uint32_t column) {
    F32Row values(kQ8BlockElements), absolute(kQ8BlockElements), scratch(kQ8BlockElements);
    F32Scalar maximum;
    load(values, input, row, column);
    pto::TABS(absolute, values);
    pto::TROWMAX(maximum, absolute, scratch);
    float absolute_maximum = 0.0f;
    auto maximum_output =
        global_row<float>(reinterpret_cast<uintptr_t>(&absolute_maximum), 1);
    pto::TSTORE(maximum_output, maximum);
    ++operator_counts.tstore;

    QuantizedQ8Block output;
    if (absolute_maximum == 0.0f) return output;
    output.scale = absolute_maximum / 127.0f;
    pto::TMULS(values, values, output.scale == 0.0f ? 0.0f : 1.0f / output.scale);
    I8Block quantized(kQ8BlockElements);
    pto::TCVT(quantized, values, pto::RoundMode::CAST_RINT);
    auto destination = global_row<int8_t>(
        reinterpret_cast<uintptr_t>(output.values.data()), kQ8BlockElements);
    pto::TSTORE(destination, quantized);
    ++operator_counts.tstore;
    return output;
}

inline void validate_q8_matrix(const WeightTensor &weight, uint32_t input_columns,
                               uint32_t output_columns) {
    if (weight.tensor_type != kGgufTypeQ8_0 || weight.rank != 2 ||
        weight.dimensions[0] != input_columns ||
        weight.dimensions[1] != output_columns ||
        input_columns == 0 || input_columns % kQ8BlockElements != 0) {
        throw std::invalid_argument("deepseek_pto_q8_geometry");
    }
    const uint64_t expected = uint64_t(output_columns) *
                              (input_columns / kQ8BlockElements) * kQ8BlockBytes;
    if (weight.bytes != expected) {
        throw std::invalid_argument("deepseek_pto_q8_payload");
    }
}

inline void q8_0_linear(const F32View &input, const WeightTensor &weight,
                        const F32View &output,
                        const F32View *trace_output) {
    ++operator_counts.q8_linear;
    input.validate();
    output.validate();
    if (trace_output) {
        trace_output->validate();
        if (trace_output->rows != output.rows ||
            trace_output->columns != output.columns) {
            throw std::invalid_argument("deepseek_pto_trace_output_geometry");
        }
    }
    if (input.rows != output.rows) {
        throw std::invalid_argument("deepseek_pto_q8_rows");
    }
    validate_q8_matrix(weight, input.columns, output.columns);
    const uint32_t blocks = input.columns / kQ8BlockElements;
    const uint64_t row_bytes = uint64_t(blocks) * kQ8BlockBytes;
    std::vector<QuantizedQ8Block> activation(blocks);
    std::array<int8_t, kQ8BlockElements> weight_values {};
    for (uint32_t input_row = 0; input_row < input.rows; ++input_row) {
        for (uint32_t block = 0; block < blocks; ++block) {
            activation[block] =
                quantize_q8_0(input, input_row, block * kQ8BlockElements);
        }
        for (uint32_t output_row = 0; output_row < output.columns; ++output_row) {
            F32Scalar accumulator;
            pto::TEXPANDS(accumulator, 0.0f);
            for (uint32_t block = 0; block < blocks; ++block) {
                const uint8_t *encoded =
                    weight.payload + uint64_t(output_row) * row_bytes +
                    uint64_t(block) * kQ8BlockBytes;
                const float weight_scale = load_f16_scalar(encoded);
                for (uint32_t lane = 0; lane < kQ8BlockElements; ++lane) {
                    weight_values[lane] = static_cast<int8_t>(encoded[2 + lane]);
                }
                I8Block activation_i8(kQ8BlockElements), weight_i8(kQ8BlockElements);
                auto activation_source = global_row<int8_t>(
                    reinterpret_cast<uintptr_t>(activation[block].values.data()),
                    kQ8BlockElements);
                auto weight_source = global_row<int8_t>(
                    reinterpret_cast<uintptr_t>(weight_values.data()), kQ8BlockElements);
                pto::TLOAD(activation_i8, activation_source);
                pto::TLOAD(weight_i8, weight_source);
                operator_counts.tload += 2;
                F32Row activation_f32(kQ8BlockElements), weight_f32(kQ8BlockElements);
                F32Row product(kQ8BlockElements), scratch(kQ8BlockElements);
                F32Scalar dot, scaled;
                pto::TCVT(activation_f32, activation_i8, pto::RoundMode::CAST_NONE);
                pto::TCVT(weight_f32, weight_i8, pto::RoundMode::CAST_NONE);
                pto::TMUL(product, activation_f32, weight_f32);
                pto::TROWSUM(dot, product, scratch);
                pto::TMULS(
                    scaled, dot, activation[block].scale * weight_scale);
                pto::TADD(accumulator, accumulator, scaled);
            }
            auto destination = global_row<float>(output.at(input_row, output_row), 1);
            pto::TSTORE(destination, accumulator);
            ++operator_counts.tstore;
            if (trace_output) {
                auto trace_destination = global_row<float>(
                    trace_output->at(input_row, output_row), 1);
                pto::TSTORE(trace_destination, accumulator);
                ++operator_counts.tstore;
            }
        }
    }
}

struct QuantizedQ8KBlock {
    std::array<int8_t, kQ8KBlockElements> values {};
    std::array<int16_t, 16> sums {};
    float scale = 0.0f;
};

inline QuantizedQ8KBlock quantize_q8_k(const F32View &input, uint32_t row,
                                       uint32_t column) {
    if (column > input.columns ||
        kQ8KBlockElements > input.columns - column) {
        throw std::invalid_argument("deepseek_pto_q8_k_geometry");
    }
    F32Row values(kQ8KBlockElements), absolute(kQ8KBlockElements);
    load(values, input, row, column);
    U8Block above_min(kQ8KBlockElements / 8), below_max(kQ8KBlockElements / 8);
    U8Block finite_mask(kQ8KBlockElements / 8), finite_scratch(kQ8KBlockElements / 8);
    U8Scalar finite_result;
    const float finite_bound = std::numeric_limits<float>::max();
    pto::TCMPS(above_min, values, -finite_bound, pto::CmpMode::GE);
    pto::TCMPS(below_max, values, finite_bound, pto::CmpMode::LE);
    pto::TAND(finite_mask, above_min, below_max);
    pto::TROWMIN(finite_result, finite_mask, finite_scratch);
    uint8_t finite = 0;
    auto finite_output = global_row<uint8_t>(reinterpret_cast<uintptr_t>(&finite), 1);
    pto::TSTORE(finite_output, finite_result);
    ++operator_counts.tstore;
    if (finite != 0xff) {
        throw std::invalid_argument("deepseek_pto_q8_k_non_finite");
    }

    pto::TABS(absolute, values);
    F32Scalar maximum;
    F32Row maximum_scratch(kQ8KBlockElements);
    U32Scalar maximum_index;
    pto::TROWARGMAX(maximum, maximum_index, absolute, maximum_scratch);
    float absolute_maximum = 0.0f;
    uint32_t index = kQ8KBlockElements;
    auto maximum_output = global_row<float>(
        reinterpret_cast<uintptr_t>(&absolute_maximum), 1);
    auto index_output = global_row<uint32_t>(reinterpret_cast<uintptr_t>(&index), 1);
    pto::TSTORE(maximum_output, maximum);
    pto::TSTORE(index_output, maximum_index);
    operator_counts.tstore += 2;
    if (index >= kQ8KBlockElements) {
        throw std::runtime_error("deepseek_pto_q8_k_argmax");
    }
    float signed_maximum = 0.0f;
    F32Scalar signed_tile;
    load_scalar(signed_tile, input, row, column + index);
    auto signed_output = global_row<float>(
        reinterpret_cast<uintptr_t>(&signed_maximum), 1);
    pto::TSTORE(signed_output, signed_tile);
    ++operator_counts.tstore;

    QuantizedQ8KBlock output;
    if (absolute_maximum != 0.0f) {
        const float inverse_scale = -127.0f / signed_maximum;
        output.scale = 1.0f / inverse_scale;
        pto::TMULS(values, values, inverse_scale);
        I8SuperBlock quantized(kQ8KBlockElements);
        pto::TCVT(quantized, values, pto::RoundMode::CAST_RINT);
        auto destination = global_row<int8_t>(
            reinterpret_cast<uintptr_t>(output.values.data()),
            kQ8KBlockElements);
        pto::TSTORE(destination, quantized);
        ++operator_counts.tstore;
    }
    for (uint32_t group = 0; group < 16; ++group) {
        I8Block codes(16);
        I16Block widened(16), scratch(16);
        I16Scalar sum;
        auto source = global_row<int8_t>(
            reinterpret_cast<uintptr_t>(output.values.data() + group * 16), 16);
        pto::TLOAD(codes, source);
        ++operator_counts.tload;
        pto::TCVT(widened, codes, pto::RoundMode::CAST_NONE);
        pto::TROWSUM(sum, widened, scratch);
        auto destination = global_row<int16_t>(
            reinterpret_cast<uintptr_t>(&output.sums[group]), 1);
        pto::TSTORE(destination, sum);
        ++operator_counts.tstore;
    }
    return output;
}

inline void validate_expert_matrix(const WeightTensor &weight,
                                   uint32_t expected_type,
                                   uint32_t block_bytes,
                                   uint32_t input_columns,
                                   uint32_t output_columns,
                                   uint32_t expert) {
    if (weight.tensor_type != expected_type || weight.rank != 3 ||
        weight.dimensions[0] != input_columns ||
        weight.dimensions[1] != output_columns || !input_columns ||
        input_columns % kQ8KBlockElements || !output_columns ||
        !weight.dimensions[2] || expert >= weight.dimensions[2]) {
        throw std::invalid_argument("deepseek_pto_expert_matrix_geometry");
    }
    const uint64_t expected =
        uint64_t(weight.dimensions[2]) * output_columns *
        (input_columns / kQ8KBlockElements) * block_bytes;
    if (weight.bytes != expected) {
        throw std::invalid_argument("deepseek_pto_expert_matrix_payload");
    }
}

inline bool same_weight_shape(const WeightTensor &left,
                              const WeightTensor &right) {
    if (left.rank != right.rank) return false;
    for (uint32_t dimension = 0; dimension < left.rank; ++dimension) {
        if (left.dimensions[dimension] != right.dimensions[dimension]) {
            return false;
        }
    }
    return true;
}

inline F32Scalar i8_dot(const int8_t *left, const int8_t *right,
                        uint32_t count) {
    if (!left || !right || !count || count > kQ8BlockElements) {
        throw std::invalid_argument("deepseek_pto_i8_dot_geometry");
    }
    I8Block left_i8(count), right_i8(count);
    auto left_source =
        global_row<int8_t>(reinterpret_cast<uintptr_t>(left), count);
    auto right_source =
        global_row<int8_t>(reinterpret_cast<uintptr_t>(right), count);
    pto::TLOAD(left_i8, left_source);
    pto::TLOAD(right_i8, right_source);
    operator_counts.tload += 2;
    F32Row left_f32(count), right_f32(count), product(count), scratch(count);
    F32Scalar dot;
    pto::TCVT(left_f32, left_i8, pto::RoundMode::CAST_NONE);
    pto::TCVT(right_f32, right_i8, pto::RoundMode::CAST_NONE);
    pto::TMUL(product, left_f32, right_f32);
    pto::TROWSUM(dot, product, scratch);
    return dot;
}

inline void q2_k_expert_linear(const F32View &input,
                               const WeightTensor &weight, uint32_t expert,
                               const F32View &output) {
    ++operator_counts.q2_k_linear;
    input.validate();
    output.validate();
    if (input.rows != output.rows) {
        throw std::invalid_argument("deepseek_pto_q2_k_rows");
    }
    validate_expert_matrix(weight, kGgufTypeQ2_K, kQ2KBlockBytes,
                           input.columns, output.columns, expert);
    const uint32_t blocks = input.columns / kQ8KBlockElements;
    const uint64_t row_bytes = uint64_t(blocks) * kQ2KBlockBytes;
    const uint64_t expert_bytes = uint64_t(output.columns) * row_bytes;
    const uint8_t *expert_payload = weight.payload + expert * expert_bytes;
    std::array<int8_t, kQ8BlockElements> decoded_weight {};
    for (uint32_t input_row = 0; input_row < input.rows; ++input_row) {
        std::vector<QuantizedQ8KBlock> activation;
        activation.reserve(blocks);
        for (uint32_t block = 0; block < blocks; ++block) {
            activation.push_back(quantize_q8_k(
                input, input_row, block * kQ8KBlockElements));
        }
        for (uint32_t output_row = 0; output_row < output.columns;
             ++output_row) {
            F32Scalar accumulator;
            pto::TEXPANDS(accumulator, 0.0f);
            for (uint32_t block_index = 0; block_index < blocks;
                 ++block_index) {
                const uint8_t *block =
                    expert_payload + uint64_t(output_row) * row_bytes +
                    uint64_t(block_index) * kQ2KBlockBytes;
                const uint8_t *scales = block;
                const uint8_t *quants = block + 16;
                const float weight_scale = load_f16_scalar(block + 80);
                const float weight_minimum = load_f16_scalar(block + 82);
                int32_t minimum_sum = 0;
                for (uint32_t group = 0; group < 16; ++group) {
                    minimum_sum +=
                        int32_t(activation[block_index].sums[group]) *
                        int32_t(scales[group] >> 4);
                }
                for (uint32_t group = 0; group < 8; ++group) {
                    const uint32_t quant_base = (group / 4) * 32;
                    const uint32_t shift = (group % 4) * 2;
                    for (uint32_t lane = 0; lane < kQ8BlockElements;
                         ++lane) {
                        const uint8_t local_scale =
                            scales[group * 2 + lane / 16] & 0x0f;
                        const uint8_t quant =
                            (quants[quant_base + lane] >> shift) & 0x03;
                        decoded_weight[lane] =
                            static_cast<int8_t>(quant * local_scale);
                    }
                    F32Scalar dot = i8_dot(
                        activation[block_index].values.data() +
                            group * kQ8BlockElements,
                        decoded_weight.data(), kQ8BlockElements);
                    F32Scalar scaled;
                    pto::TMULS(scaled, dot,
                               activation[block_index].scale * weight_scale);
                    pto::TADD(accumulator, accumulator, scaled);
                }
                pto::TADDS(
                    accumulator, accumulator,
                    -activation[block_index].scale * weight_minimum *
                        float(minimum_sum));
            }
            store_scalar(output, input_row, output_row, accumulator);
        }
    }
}

struct Iq2Codebook {
    std::span<const uint64_t> grid;
    std::span<const uint8_t> signs;

    void validate() const {
        if (grid.size() != 256 || signs.size() != 128) {
            throw std::invalid_argument("deepseek_pto_iq2_codebook");
        }
    }
};

inline void iq2_xxs_expert_linear(const F32View &input,
                                   const WeightTensor &weight,
                                   uint32_t expert,
                                   const Iq2Codebook &codebook,
                                   const F32View &output) {
    ++operator_counts.iq2_xxs_linear;
    input.validate();
    output.validate();
    codebook.validate();
    if (input.rows != output.rows) {
        throw std::invalid_argument("deepseek_pto_iq2_xxs_rows");
    }
    validate_expert_matrix(weight, kGgufTypeIq2Xxs, kIq2XxsBlockBytes,
                           input.columns, output.columns, expert);
    const uint32_t blocks = input.columns / kQ8KBlockElements;
    const uint64_t row_bytes = uint64_t(blocks) * kIq2XxsBlockBytes;
    const uint64_t expert_bytes = uint64_t(output.columns) * row_bytes;
    const uint8_t *expert_payload = weight.payload + expert * expert_bytes;
    std::array<int8_t, kQ8BlockElements> decoded_weight {};
    for (uint32_t input_row = 0; input_row < input.rows; ++input_row) {
        std::vector<QuantizedQ8KBlock> activation;
        activation.reserve(blocks);
        for (uint32_t block = 0; block < blocks; ++block) {
            activation.push_back(quantize_q8_k(
                input, input_row, block * kQ8KBlockElements));
        }
        for (uint32_t output_row = 0; output_row < output.columns;
             ++output_row) {
            F32Scalar accumulator;
            pto::TEXPANDS(accumulator, 0.0f);
            for (uint32_t block_index = 0; block_index < blocks;
                 ++block_index) {
                const uint8_t *block =
                    expert_payload + uint64_t(output_row) * row_bytes +
                    uint64_t(block_index) * kIq2XxsBlockBytes;
                const float block_scale = load_f16_scalar(block);
                for (uint32_t group = 0; group < 8; ++group) {
                    const uint8_t *encoded = block + 2 + group * 8;
                    const uint32_t sign_and_scale =
                        uint32_t(encoded[4]) | (uint32_t(encoded[5]) << 8) |
                        (uint32_t(encoded[6]) << 16) |
                        (uint32_t(encoded[7]) << 24);
                    for (uint32_t pair = 0; pair < 4; ++pair) {
                        const uint64_t grid = codebook.grid[encoded[pair]];
                        const uint8_t signs = codebook.signs[
                            (sign_and_scale >> (7 * pair)) & 127];
                        for (uint32_t lane = 0; lane < 8; ++lane) {
                            const int8_t magnitude = static_cast<int8_t>(
                                (grid >> (8 * lane)) & 0xff);
                            decoded_weight[pair * 8 + lane] =
                                signs & (1u << lane) ? -magnitude : magnitude;
                        }
                    }
                    F32Scalar dot = i8_dot(
                        activation[block_index].values.data() +
                            group * kQ8BlockElements,
                        decoded_weight.data(), kQ8BlockElements);
                    const uint32_t local_scale =
                        2 * (sign_and_scale >> 28) + 1;
                    F32Scalar scaled;
                    pto::TMULS(
                        scaled, dot,
                        0.125f * activation[block_index].scale * block_scale *
                            float(local_scale));
                    pto::TADD(accumulator, accumulator, scaled);
                }
            }
            store_scalar(output, input_row, output_row, accumulator);
        }
    }
}

inline void routed_experts(const F32View &input,
                           const WeightTensor &gate_weight,
                           const WeightTensor &up_weight,
                           const WeightTensor &down_weight,
                           const Iq2Codebook &codebook,
                           std::span<const uint32_t> selected_experts,
                           std::span<const float> expert_weights, float clamp,
                           const F32View &output) {
    ++operator_counts.routed_experts;
    input.validate();
    output.validate();
    if (input.rows != output.rows || input.columns != output.columns ||
        selected_experts.empty() ||
        selected_experts.size() != expert_weights.size() ||
        !same_weight_shape(gate_weight, up_weight) ||
        gate_weight.rank != 3 || down_weight.rank != 3 ||
        gate_weight.dimensions[0] != input.columns ||
        down_weight.dimensions[0] != gate_weight.dimensions[1] ||
        down_weight.dimensions[1] != input.columns ||
        down_weight.dimensions[2] != gate_weight.dimensions[2]) {
        throw std::invalid_argument("deepseek_pto_routed_experts_geometry");
    }
    const uint32_t intermediate =
        static_cast<uint32_t>(gate_weight.dimensions[1]);
    fill(output, 0.0f);
    std::vector<float> gate_storage(uint64_t(input.rows) * intermediate);
    std::vector<float> up_storage(uint64_t(input.rows) * intermediate);
    std::vector<float> activated_storage(uint64_t(input.rows) * intermediate);
    std::vector<float> down_storage(uint64_t(input.rows) * input.columns);
    const F32View gate {reinterpret_cast<uintptr_t>(gate_storage.data()),
                        input.rows, intermediate, intermediate, 1};
    const F32View up {reinterpret_cast<uintptr_t>(up_storage.data()),
                      input.rows, intermediate, intermediate, 1};
    const F32View activated {
        reinterpret_cast<uintptr_t>(activated_storage.data()), input.rows,
        intermediate, intermediate, 1};
    const F32View down {reinterpret_cast<uintptr_t>(down_storage.data()),
                        input.rows, input.columns, input.columns, 1};
    for (uint32_t slot = 0; slot < selected_experts.size(); ++slot) {
        if (!std::isfinite(expert_weights[slot])) {
            throw std::invalid_argument("deepseek_pto_expert_weight");
        }
        const uint32_t expert = selected_experts[slot];
        iq2_xxs_expert_linear(input, gate_weight, expert, codebook, gate);
        iq2_xxs_expert_linear(input, up_weight, expert, codebook, up);
        swiglu(gate, up, activated, clamp);
        scale_in_place(activated, expert_weights[slot]);
        q2_k_expert_linear(activated, down_weight, expert, down);
        residual(output, down, output);
    }
}

inline void shared_expert(const F32View &input,
                          const WeightTensor &gate_weight,
                          const WeightTensor &up_weight,
                          const WeightTensor &down_weight, float clamp,
                          const F32View &output) {
    ++operator_counts.shared_expert;
    input.validate();
    output.validate();
    if (input.rows != output.rows || input.columns != output.columns ||
        !same_weight_shape(gate_weight, up_weight) ||
        gate_weight.rank != 2 || down_weight.rank != 2 ||
        gate_weight.dimensions[0] != input.columns ||
        down_weight.dimensions[0] != gate_weight.dimensions[1] ||
        down_weight.dimensions[1] != input.columns) {
        throw std::invalid_argument("deepseek_pto_shared_expert_geometry");
    }
    const uint32_t intermediate =
        static_cast<uint32_t>(gate_weight.dimensions[1]);
    std::vector<float> gate_storage(uint64_t(input.rows) * intermediate);
    std::vector<float> up_storage(uint64_t(input.rows) * intermediate);
    std::vector<float> activated_storage(uint64_t(input.rows) * intermediate);
    const F32View gate {reinterpret_cast<uintptr_t>(gate_storage.data()),
                        input.rows, intermediate, intermediate, 1};
    const F32View up {reinterpret_cast<uintptr_t>(up_storage.data()),
                      input.rows, intermediate, intermediate, 1};
    const F32View activated {
        reinterpret_cast<uintptr_t>(activated_storage.data()), input.rows,
        intermediate, intermediate, 1};
    q8_0_linear(input, gate_weight, gate);
    q8_0_linear(input, up_weight, up);
    swiglu(gate, up, activated, clamp);
    q8_0_linear(activated, down_weight, output);
}

} // namespace lingqu::deepseek_v4_flash_pto
