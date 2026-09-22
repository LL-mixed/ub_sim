#pragma once

#include "deepseek_v4_flash_pto_weights.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <vector>

#include <pto/pto-inst.hpp>

namespace lingqu::deepseek_v4_flash_pto {

constexpr uint32_t kVectorCapacity = 4096;
constexpr uint32_t kQ8BlockElements = 32;
constexpr uint32_t kQ8BlockBytes = 34;
constexpr uint32_t kGgufTypeF16 = 1;
constexpr uint32_t kGgufTypeQ8_0 = 8;

struct OperatorCounts {
    uint64_t tload = 0;
    uint64_t tstore = 0;
    uint64_t q8_linear = 0;
    uint64_t f16_linear = 0;
    uint64_t rms_norm = 0;
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
using F16Scalar = pto::Tile<pto::TileType::Vec, half, 1, 16,
                            pto::BLayout::RowMajor, 1, 1>;
using F16Row = pto::Tile<pto::TileType::Vec, half, 1, kVectorCapacity,
                         pto::BLayout::RowMajor, 1, pto::DYNAMIC>;
using I8Block = pto::Tile<pto::TileType::Vec, int8_t, 1, kQ8BlockElements,
                          pto::BLayout::RowMajor, 1, pto::DYNAMIC>;
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
                        const F32View &output) {
    ++operator_counts.q8_linear;
    input.validate();
    output.validate();
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
        }
    }
}

} // namespace lingqu::deepseek_v4_flash_pto
