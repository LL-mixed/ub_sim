#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <vector>

#include <pto/pto-inst.hpp>

namespace lingqu::qwen3_pto {

constexpr uint32_t kRowCapacity = 4096;
enum class DType : uint32_t { F32 = 0, F16 = 1 };
enum class Operation : uint32_t {
    Copy = 1, RmsNorm = 2, Linear = 3, Rope = 4,
    Attention = 5, Residual = 6, SwiGlu = 7, Softmax = 8,
};

struct OperatorCounts {
    uint64_t copy = 0, rms_norm = 0, linear = 0, rope = 0, attention = 0;
    uint64_t residual = 0, swiglu = 0, softmax = 0;
    uint64_t embedding = 0, decoder_layer = 0, terminal_logits = 0;
};
// A worker can run many requests; decoder_range resets this for each request.
inline thread_local OperatorCounts operator_counts;

// Numerical geometry only. Object ownership and access permissions belong to
// the dispatch binding. Addresses may name ordinary GM or a UB_GM aperture.
struct View {
    uintptr_t address;
    uint32_t rows;
    uint32_t cols;
    uint32_t row_stride;
    uint32_t col_stride;
    DType dtype;

    void validate() const {
        if (!address || !rows || !cols || !row_stride || !col_stride ||
            (dtype != DType::F32 && dtype != DType::F16)) {
            throw std::invalid_argument("qwen3_pto_invalid_view");
        }
        const uint64_t last = uint64_t(rows - 1) * row_stride +
                              uint64_t(cols - 1) * col_stride;
        const uint64_t bytes = dtype == DType::F32 ? 4 : 2;
        const uint64_t available = std::numeric_limits<uintptr_t>::max() - address;
        if (available < bytes || last >= available / bytes) {
            throw std::invalid_argument("qwen3_pto_view_overflow");
        }
    }

    uintptr_t at(uint32_t row, uint32_t col = 0) const {
        if (row >= rows || col >= cols) {
            throw std::out_of_range("qwen3_pto_view_bounds");
        }
        return address + (uint64_t(row) * row_stride + uint64_t(col) * col_stride) *
                             (dtype == DType::F32 ? 4 : 2);
    }
};

using Row = pto::Tile<pto::TileType::Vec, float, 1, kRowCapacity,
                      pto::BLayout::RowMajor, 1, pto::DYNAMIC>;
using HalfRow = pto::Tile<pto::TileType::Vec, half, 1, kRowCapacity,
                          pto::BLayout::RowMajor, 1, pto::DYNAMIC>;
using Reduction = pto::Tile<pto::TileType::Vec, float, 1, 8,
                            pto::BLayout::RowMajor, 1, 1>;
using RowShape = pto::Shape<1, 1, 1, 1, pto::DYNAMIC>;
using RowStride = pto::Stride<pto::DYNAMIC, pto::DYNAMIC, pto::DYNAMIC,
                              pto::DYNAMIC, pto::DYNAMIC>;
template <typename T>
using GlobalRow = pto::GlobalTensor<T, RowShape, RowStride>;

inline void require(bool condition) {
    if (!condition) throw std::invalid_argument("qwen3_pto_geometry_mismatch");
}

template <typename T>
inline GlobalRow<T> global_row(uintptr_t address, uint32_t cols, uint32_t stride) {
    require(cols && cols <= kRowCapacity && stride);
    const uint64_t extent = uint64_t(cols - 1) * stride + 1;
    return GlobalRow<T>(reinterpret_cast<T *>(address), RowShape(cols),
                        RowStride(extent, extent, extent, extent, stride));
}

inline void load(Row &tile, const View &view, uint32_t row, uint32_t col = 0) {
    const uint32_t count = tile.GetValidCol();
    require(count <= view.cols && col <= view.cols - count);
    if (view.dtype == DType::F32) {
        auto gm = global_row<float>(view.at(row, col), count, view.col_stride);
        pto::TLOAD(tile, gm);
    } else {
        HalfRow half_tile(count);
        auto gm = global_row<half>(view.at(row, col), count, view.col_stride);
        pto::TLOAD(half_tile, gm);
        pto::TCVT(tile, half_tile, pto::RoundMode::CAST_NONE);
    }
}

inline void store(const View &view, uint32_t row, Row &tile, uint32_t col = 0) {
    const uint32_t count = tile.GetValidCol();
    require(count <= view.cols && col <= view.cols - count);
    if (view.dtype == DType::F32) {
        auto gm = global_row<float>(view.at(row, col), count, view.col_stride);
        pto::TSTORE(gm, tile);
    } else {
        HalfRow half_tile(count);
        pto::TCVT(half_tile, tile, pto::RoundMode::CAST_RINT);
        auto gm = global_row<half>(view.at(row, col), count, view.col_stride);
        pto::TSTORE(gm, half_tile);
    }
}

inline void same_shape(const View &a, const View &b) {
    a.validate();
    b.validate();
    require(a.rows == b.rows && a.cols == b.cols);
}

// Embedding lookup and KV append/reuse use the same bounded row-copy path.
inline void copy_rows(const View &src, const View &dst, uint32_t first_row = 0) {
    ++operator_counts.copy;
    src.validate();
    dst.validate();
    require(first_row < src.rows && dst.rows <= src.rows - first_row && src.cols == dst.cols);
    for (uint32_t r = 0; r < dst.rows; ++r) {
        for (uint32_t c = 0; c < dst.cols; c += kRowCapacity) {
            Row tile(std::min(kRowCapacity, dst.cols - c));
            load(tile, src, first_row + r, c);
            store(dst, r, tile, c);
        }
    }
}

inline void rms_norm(const View &src, const View &weight, const View &dst) {
    ++operator_counts.rms_norm;
    same_shape(src, dst);
    weight.validate();
    require(src.cols <= kRowCapacity && weight.rows == 1 && weight.cols == src.cols);
    Row x(src.cols), w(src.cols), square(src.cols), scratch(src.cols);
    Reduction sum, one;
    pto::TEXPANDS(one, 1.0f);
    load(w, weight, 0);
    for (uint32_t r = 0; r < src.rows; ++r) {
        load(x, src, r);
        pto::TMUL(square, x, x);
        pto::TROWSUM(sum, square, scratch);
        pto::TMULS(sum, sum, 1.0f / float(src.cols));
        pto::TADDS(sum, sum, 1.0e-6f);
        // Match FP32 sqrt followed by FP32 division at the W5 boundary.
        pto::TSQRT(sum, sum);
        pto::TDIV(sum, one, sum);
        pto::TROWEXPAND(scratch, sum);
        pto::TMUL(square, x, scratch);
        pto::TMUL(x, square, w);
        store(dst, r, x);
    }
}

inline void residual(const View &a, const View &b, const View &dst) {
    ++operator_counts.residual;
    same_shape(a, b);
    same_shape(a, dst);
    for (uint32_t r = 0; r < a.rows; ++r) {
        for (uint32_t c = 0; c < a.cols; c += kRowCapacity) {
            const uint32_t n = std::min(kRowCapacity, a.cols - c);
            Row x(n), y(n), z(n);
            load(x, a, r, c);
            load(y, b, r, c);
            pto::TADD(z, x, y);
            store(dst, r, z, c);
        }
    }
}

inline void swiglu(const View &gate, const View &up, const View &dst) {
    ++operator_counts.swiglu;
    same_shape(gate, up);
    same_shape(gate, dst);
    for (uint32_t r = 0; r < gate.rows; ++r) {
        for (uint32_t c = 0; c < gate.cols; c += kRowCapacity) {
            const uint32_t n = std::min(kRowCapacity, gate.cols - c);
            Row g(n), u(n), denominator(n), result(n);
            load(g, gate, r, c);
            load(u, up, r, c);
            pto::TMULS(denominator, g, -1.0f);
            pto::TEXP(denominator, denominator);
            pto::TADDS(denominator, denominator, 1.0f);
            pto::TDIV(result, g, denominator);
            pto::TMUL(result, result, u);
            store(dst, r, result, c);
        }
    }
}

inline void softmax(const View &src, const View &dst, float scale = 1.0f) {
    ++operator_counts.softmax;
    same_shape(src, dst);
    require(src.cols <= kRowCapacity);
    Row x(src.cols), exponent(src.cols), scratch(src.cols);
    Reduction maximum, sum;
    for (uint32_t r = 0; r < src.rows; ++r) {
        load(x, src, r);
        pto::TMULS(x, x, scale);
        pto::TROWMAX(maximum, x, scratch);
        pto::TROWEXPAND(scratch, maximum);
        pto::TSUB(exponent, x, scratch);
        pto::TEXP(exponent, exponent);
        pto::TROWSUM(sum, exponent, scratch);
        pto::TROWEXPAND(scratch, sum);
        pto::TDIV(x, exponent, scratch);
        store(dst, r, x);
    }
}

// Weight geometry is [output_features, input_features], matching safetensors.
// A dot is expressed as TMUL + TROWSUM. Accumulation stays in PTO tile storage.
using LinearObserver = void (*)(Row &, uint32_t, uint32_t, void *);

inline void linear(const View &src, const View &weight, const View &dst,
                   LinearObserver observe = nullptr, void *context = nullptr) {
    ++operator_counts.linear;
    src.validate();
    weight.validate();
    dst.validate();
    require(src.cols <= kRowCapacity && src.cols == weight.cols &&
            src.rows == dst.rows && weight.rows == dst.cols);
    Row x(src.cols), w(src.cols), product(src.cols), scratch(src.cols);
    Reduction sum;
    alignas(64) float block[64];
    for (uint32_t r = 0; r < src.rows; ++r) {
        load(x, src, r);
        for (uint32_t n = 0; n < dst.cols; n += 64) {
            const uint32_t count = std::min(uint32_t(64), dst.cols - n);
            for (uint32_t j = 0; j < count; ++j) {
                load(w, weight, n + j);
                pto::TMUL(product, x, w);
                pto::TROWSUM(sum, product, scratch);
                auto gm = global_row<float>(reinterpret_cast<uintptr_t>(block + j), 1, 1);
                pto::TSTORE(gm, sum);
            }
            Row result(count);
            const View local{reinterpret_cast<uintptr_t>(block), 1, count, count, 1, DType::F32};
            load(result, local, 0);
            store(dst, r, result, n);
            if (observe) observe(result, r, n, context);
        }
    }
}

// Split-half RoPE: [x0*cos - x1*sin, x1*cos + x0*sin]. The factors are
// precomputed constants, with two rows per position (cos followed by sin).
inline void rope(const View &src, const View &factors, const View &dst) {
    ++operator_counts.rope;
    same_shape(src, dst);
    factors.validate();
    require(src.cols % 2 == 0 && src.cols <= kRowCapacity &&
            factors.cols == src.cols / 2 &&
            (factors.rows == 2 || uint64_t(factors.rows) == uint64_t(src.rows) * 2));
    const uint32_t half_dim = src.cols / 2;
    Row lo(half_dim), hi(half_dim), cosine(half_dim), sine(half_dim);
    Row a(half_dim), b(half_dim), out_lo(half_dim), out_hi(half_dim);
    for (uint32_t r = 0; r < src.rows; ++r) {
        load(lo, src, r);
        load(hi, src, r, half_dim);
        const uint32_t factor_row = factors.rows == 2 ? 0 : 2 * r;
        load(cosine, factors, factor_row);
        load(sine, factors, factor_row + 1);
        pto::TMUL(a, lo, cosine);
        pto::TMUL(b, hi, sine);
        pto::TSUB(out_lo, a, b);
        pto::TMUL(a, hi, cosine);
        pto::TMUL(b, lo, sine);
        pto::TADD(out_hi, a, b);
        store(dst, r, out_lo);
        store(dst, r, out_hi, half_dim);
    }
}

// One query head and its selected KV head. Strides preserve the shared
// interleaved head layout. The caller selects GQA groups without repacking KV.
inline void attention(const View &query, const View &key, const View &value,
                      const View &dst, uint32_t past_tokens, float scale) {
    ++operator_counts.attention;
    same_shape(query, dst);
    same_shape(key, value);
    require(query.cols == key.cols && query.cols <= kRowCapacity &&
            uint64_t(past_tokens) + query.rows == key.rows && key.rows <= kRowCapacity);
    std::vector<float> scores(key.rows), probabilities(key.rows);
    for (uint32_t r = 0; r < query.rows; ++r) {
        const uint32_t used = past_tokens + r + 1;
        View q = query;
        q.address = query.at(r);
        q.rows = 1;
        View k = key;
        k.rows = used;
        View out = dst;
        out.address = dst.at(r);
        out.rows = 1;
        const View score{reinterpret_cast<uintptr_t>(scores.data()), 1, used, used, 1, DType::F32};
        const View probability{reinterpret_cast<uintptr_t>(probabilities.data()), 1, used,
                               used, 1, DType::F32};
        const View transposed_value{value.address, value.cols, used, value.col_stride,
                                    value.row_stride, value.dtype};
        linear(q, k, score);
        softmax(score, probability, scale);
        linear(probability, transposed_value, out);
    }
}

inline void execute(Operation op, const View &src, const View &weight,
                    const View &aux, const View &dst, uint32_t parameter, float scale) {
    if (op == Operation::Attention || op == Operation::Softmax)
        require(std::isfinite(scale) && scale > 0);
    switch (op) {
        case Operation::Copy: copy_rows(src, dst, parameter); break;
        case Operation::RmsNorm: rms_norm(src, weight, dst); break;
        case Operation::Linear: linear(src, weight, dst); break;
        case Operation::Rope: rope(src, weight, dst); break;
        case Operation::Attention: attention(src, weight, aux, dst, parameter, scale); break;
        case Operation::Residual: residual(src, weight, dst); break;
        case Operation::SwiGlu: swiglu(src, weight, dst); break;
        case Operation::Softmax: softmax(src, dst, scale); break;
        default: throw std::invalid_argument("qwen3_pto_unknown_operation");
    }
}

} // namespace lingqu::qwen3_pto
