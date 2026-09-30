#pragma once

#include "deepseek_v4_flash_pto_state.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <memory>
#include <numbers>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace lingqu::deepseek_v4_flash_pto {

constexpr uint32_t kLayers = 43;
constexpr uint32_t kHidden = 4096;
constexpr uint32_t kHcMult = 4;
constexpr uint32_t kVocab = 129280;
constexpr uint32_t kAttentionHeads = 64;
constexpr uint32_t kHeadDim = 512;
constexpr uint32_t kRopeDim = 64;
constexpr uint32_t kQLora = 1024;
constexpr uint32_t kOLora = 1024;
constexpr uint32_t kOutputGroups = 8;
constexpr uint32_t kExperts = 256;
constexpr uint32_t kExpertsUsed = 6;
constexpr uint32_t kMoeIntermediate = 2048;
constexpr uint32_t kSlidingWindow = 128;
constexpr uint32_t kIndexerHeads = 64;
constexpr uint32_t kIndexerHeadDim = 128;
constexpr uint32_t kIndexerTopK = 512;
constexpr uint32_t kSinkhornIterations = 20;
constexpr float kEpsilon = 1.0e-6f;
constexpr float kSwigluClamp = 10.0f;
constexpr float kExpertWeightScale = 1.5f;
constexpr uint32_t kGgufTypeF32 = 0;
constexpr uint32_t kGgufTypeI32 = 26;

struct RangeGeometry {
    uint32_t first;
    uint32_t end;
    uint32_t layers;
    uint32_t past;
    uint32_t tokens;
    uint32_t hidden;
    uint32_t hc_mult;
    uint32_t vocab;
    uint32_t attention_heads;
    uint32_t kv_heads;
    uint32_t head_dim;
    uint32_t rope_head_dim;
    uint32_t q_lora_rank;
    uint32_t o_lora_rank;
    uint32_t output_groups;
    uint32_t experts;
    uint32_t experts_used;
    uint32_t shared_experts;
    uint32_t moe_intermediate;
    uint32_t sliding_window;
    uint32_t indexer_heads;
    uint32_t indexer_head_dim;
    uint32_t indexer_top_k;

    void validate() const {
        if (first >= end || end > layers || layers != kLayers || !tokens ||
            uint64_t(past) + tokens > (1u << 20) || hidden != kHidden ||
            hc_mult != kHcMult || vocab != kVocab ||
            attention_heads != kAttentionHeads || kv_heads != 1 ||
            head_dim != kHeadDim || rope_head_dim != kRopeDim ||
            q_lora_rank != kQLora || o_lora_rank != kOLora ||
            output_groups != kOutputGroups || experts != kExperts ||
            experts_used != kExpertsUsed || shared_experts != 1 ||
            moe_intermediate != kMoeIntermediate ||
            sliding_window != kSlidingWindow ||
            indexer_heads != kIndexerHeads ||
            indexer_head_dim != kIndexerHeadDim ||
            indexer_top_k != kIndexerTopK) {
            throw std::invalid_argument("deepseek_pto_range_geometry");
        }
    }
};

struct I32View {
    uintptr_t address;
    uint32_t elements;

    void validate() const {
        if (!address || !elements ||
            address > UINTPTR_MAX - uint64_t(elements) * sizeof(int32_t)) {
            throw std::invalid_argument("deepseek_pto_i32_view");
        }
    }
};

using I32Row = pto::Tile<pto::TileType::Vec, int32_t, 1, kVectorCapacity,
                         pto::BLayout::RowMajor, 1, pto::DYNAMIC>;

inline uint32_t token_id_at(const I32View &source, uint32_t offset) {
    source.validate();
    if (offset >= source.elements) {
        throw std::invalid_argument("deepseek_pto_token_geometry");
    }
    I32Row tile(1);
    auto input = global_row<int32_t>(
        source.address + uint64_t(offset) * sizeof(int32_t), 1);
    pto::TLOAD(tile, input);
    ++operator_counts.tload;
    const int32_t value = tile.GetValue(0);
    if (value < 0 || uint32_t(value) >= kVocab) {
        throw std::invalid_argument("deepseek_pto_token_id");
    }
    return uint32_t(value);
}

class LocalMatrix {
public:
    LocalMatrix(uint32_t rows, uint32_t columns)
        : rows_(rows), columns_(columns), values_(uint64_t(rows) * columns) {
        if (!rows || !columns) {
            throw std::invalid_argument("deepseek_pto_local_matrix");
        }
    }

    LocalMatrix(LocalMatrix &&) = default;
    LocalMatrix &operator=(LocalMatrix &&) = default;
    LocalMatrix(const LocalMatrix &) = delete;
    LocalMatrix &operator=(const LocalMatrix &) = delete;

    F32View view() {
        return F32View {reinterpret_cast<uintptr_t>(values_.data()), rows_,
                        columns_, columns_, 1};
    }

private:
    uint32_t rows_;
    uint32_t columns_;
    std::vector<float> values_;
};

inline WeightTensor layer_tensor(const MappedWeights &weights, uint32_t layer,
                                 std::string_view suffix) {
    return weights.tensor("blk." + std::to_string(layer) + "." +
                          std::string(suffix) + ".weight");
}

inline std::string layer_tensor_name(uint32_t layer,
                                     std::string_view suffix) {
    return "blk." + std::to_string(layer) + "." + std::string(suffix) +
           ".weight";
}

inline std::string selection_bias_tensor_name(uint32_t layer) {
    return "blk." + std::to_string(layer) + ".exp_probs_b.bias";
}

inline F32View f32_weight_view(const WeightTensor &weight,
                               uint32_t expected_elements) {
    if (weight.tensor_type != kGgufTypeF32 || weight.rank != 1 ||
        weight.dimensions[0] != expected_elements ||
        weight.bytes != uint64_t(expected_elements) * sizeof(float)) {
        throw std::invalid_argument("deepseek_pto_f32_weight");
    }
    return F32View {reinterpret_cast<uintptr_t>(weight.payload), 1,
                    expected_elements, expected_elements, 1};
}

inline WeightTensor f16_vector_row(const WeightTensor &weight, uint32_t row,
                                   uint32_t rows, uint32_t width) {
    const uint64_t elements = uint64_t(rows) * width;
    const bool matrix = weight.rank == 2 && weight.dimensions[0] == width &&
                        weight.dimensions[1] == rows;
    const bool flat = weight.rank == 1 && weight.dimensions[0] == elements;
    if (weight.tensor_type != kGgufTypeF16 || (!matrix && !flat) ||
        row >= rows || weight.bytes != elements * 2) {
        throw std::invalid_argument("deepseek_pto_f16_vector_row");
    }
    WeightTensor result = weight;
    result.payload += uint64_t(row) * width * 2;
    result.bytes = uint64_t(width) * 2;
    result.elements = width;
    result.dimensions[0] = width;
    result.dimensions[1] = 0;
    result.rank = 1;
    return result;
}

inline F32View prefix_rows(const F32View &view, uint32_t rows) {
    view.validate();
    if (!rows || rows > view.rows) {
        throw std::invalid_argument("deepseek_pto_prefix_rows");
    }
    return F32View {view.address, rows, view.columns, view.row_stride,
                    view.column_stride};
}

struct RopeFactors {
    std::vector<float> cosine;
    std::vector<float> sine;

    F32View cos_view() const {
        return F32View {reinterpret_cast<uintptr_t>(cosine.data()), 1,
                        static_cast<uint32_t>(cosine.size()),
                        static_cast<uint32_t>(cosine.size()), 1};
    }
    F32View sin_view() const {
        return F32View {reinterpret_cast<uintptr_t>(sine.data()), 1,
                        static_cast<uint32_t>(sine.size()),
                        static_cast<uint32_t>(sine.size()), 1};
    }
};

inline RopeFactors rope_factors(uint32_t layer, uint32_t position) {
    const bool compressed = compression_ratio(layer) != 0;
    const float frequency_base = compressed ? 160000.0f : 10000.0f;
    const float frequency_scale = compressed ? 1.0f / 16.0f : 1.0f;
    const float theta_scale = std::pow(frequency_base, -2.0f / kRopeDim);
    const auto correction = [&](float rotation) {
        return float(kRopeDim) *
               std::log(65536.0f /
                        (rotation * 2.0f * std::numbers::pi_v<float>)) /
               (2.0f * std::log(frequency_base));
    };
    const float correction_low = std::max(std::floor(correction(32.0f)), 0.0f);
    const float correction_high =
        std::min(std::ceil(correction(1.0f)), float(kRopeDim - 1));
    RopeFactors result;
    result.cosine.reserve(kRopeDim / 2);
    result.sine.reserve(kRopeDim / 2);
    float theta_extrapolated = float(position);
    for (uint32_t pair = 0; pair < kRopeDim / 2; ++pair) {
        float theta = theta_extrapolated;
        if (compressed) {
            const float y =
                (float(pair) - correction_low) /
                std::max(correction_high - correction_low, 0.001f);
            const float ramp_mix = 1.0f - std::clamp(y, 0.0f, 1.0f);
            const float interpolated = frequency_scale * theta_extrapolated;
            theta = interpolated * (1.0f - ramp_mix) +
                    theta_extrapolated * ramp_mix;
        }
        result.cosine.push_back(std::cos(theta));
        result.sine.push_back(std::sin(theta));
        theta_extrapolated *= theta_scale;
    }
    return result;
}

inline void embedding(const MappedWeights &weights, uint32_t token,
                      const F32View &output_hc) {
    output_hc.validate();
    const WeightTensor table = weights.tensor("token_embd.weight");
    if (table.tensor_type != kGgufTypeF16 || table.rank != 2 ||
        table.dimensions[0] != kHidden || table.dimensions[1] != kVocab ||
        output_hc.rows != kHcMult || output_hc.columns != kHidden ||
        token >= kVocab) {
        throw std::invalid_argument("deepseek_pto_embedding_geometry");
    }
    WeightTensor row = table;
    row.payload += uint64_t(token) * kHidden * 2;
    row.bytes = uint64_t(kHidden) * 2;
    row.elements = kHidden;
    row.dimensions[0] = kHidden;
    row.dimensions[1] = 0;
    row.rank = 1;
    LocalMatrix decoded(1, kHidden);
    decode_f16_vector(row, decoded.view());
    for (uint32_t hc = 0; hc < kHcMult; ++hc) {
        const F32View destination {output_hc.at(hc), 1, kHidden,
                                   output_hc.row_stride,
                                   output_hc.column_stride};
        copy(decoded.view(), destination);
    }
}

struct HcPrepared {
    LocalMatrix normalized;
    LocalMatrix post;
    LocalMatrix combine;
};

inline HcPrepared prepare_hc(const MappedWeights &weights, uint32_t layer,
                             std::string_view function_suffix,
                             std::string_view scale_suffix,
                             std::string_view base_suffix,
                             std::string_view norm_suffix,
                             const F32View &residual_hc) {
    residual_hc.validate();
    if (residual_hc.rows != kHcMult || residual_hc.columns != kHidden ||
        residual_hc.row_stride != kHidden || residual_hc.column_stride != 1) {
        throw std::invalid_argument("deepseek_pto_hc_residual_geometry");
    }
    LocalMatrix control_input(kHcMult, kHidden);
    rms_norm(residual_hc, nullptr, control_input.view(), kEpsilon);
    const F32View flat_control {control_input.view().address, 1,
                                kHcMult * kHidden, kHcMult * kHidden, 1};
    LocalMatrix control(1, (2 + kHcMult) * kHcMult);
    f16_linear(flat_control, layer_tensor(weights, layer, function_suffix),
               control.view());
    LocalMatrix pre(1, kHcMult);
    LocalMatrix post(1, kHcMult);
    LocalMatrix combine(kHcMult, kHcMult);
    const F32View scale = f32_weight_view(
        layer_tensor(weights, layer, scale_suffix), 3);
    const F32View base = f32_weight_view(
        layer_tensor(weights, layer, base_suffix),
        (2 + kHcMult) * kHcMult);
    hc_split(control.view(), scale, base, kHcMult, kSinkhornIterations,
             kEpsilon, pre.view(), post.view(), combine.view());
    LocalMatrix mixed(1, kHidden);
    hc_weighted_sum(residual_hc, pre.view(), mixed.view());
    LocalMatrix normalized(1, kHidden);
    const F32View norm =
        f32_weight_view(layer_tensor(weights, layer, norm_suffix), kHidden);
    rms_norm(mixed.view(), &norm, normalized.view(), kEpsilon);
    return HcPrepared {std::move(normalized), std::move(post),
                       std::move(combine)};
}

inline void append_bounded_row(const F32View &cache, uint32_t current_rows,
                               uint32_t capacity, const F32View &row) {
    cache.validate();
    row.validate();
    if (row.rows != 1 || row.columns != cache.columns ||
        current_rows > capacity || cache.rows > capacity ||
        cache.rows < std::min(current_rows + 1, capacity)) {
        throw std::invalid_argument("deepseek_pto_append_geometry");
    }
    if (current_rows == capacity) {
        for (uint32_t index = 1; index < capacity; ++index) {
            const F32View source {cache.at(index), 1, cache.columns,
                                  cache.row_stride, cache.column_stride};
            const F32View destination {cache.at(index - 1), 1, cache.columns,
                                       cache.row_stride,
                                       cache.column_stride};
            copy(source, destination);
        }
        const F32View destination {cache.at(capacity - 1), 1, cache.columns,
                                   cache.row_stride, cache.column_stride};
        copy(row, destination);
    } else {
        const F32View destination {cache.at(current_rows), 1, cache.columns,
                                   cache.row_stride, cache.column_stride};
        copy(row, destination);
    }
}

inline std::vector<uint32_t>
hash_router_experts(const MappedWeights &weights, uint32_t layer,
                    uint32_t token) {
    const WeightTensor table = layer_tensor(weights, layer, "ffn_gate_tid2eid");
    if (table.tensor_type != kGgufTypeI32 || table.rank != 2 ||
        table.dimensions[0] != kExpertsUsed ||
        table.dimensions[1] != kVocab || token >= kVocab) {
        throw std::invalid_argument("deepseek_pto_hash_router_geometry");
    }
    std::array<int32_t, kExpertsUsed> private_experts {};
    I32Row tile(kExpertsUsed);
    auto source = global_row<int32_t>(
        reinterpret_cast<uintptr_t>(
            table.payload + uint64_t(token) * kExpertsUsed * sizeof(int32_t)),
        kExpertsUsed);
    auto destination = global_row<int32_t>(
        reinterpret_cast<uintptr_t>(private_experts.data()), kExpertsUsed);
    pto::TLOAD(tile, source);
    pto::TSTORE(destination, tile);
    ++operator_counts.tload;
    ++operator_counts.tstore;
    std::vector<uint32_t> result;
    result.reserve(kExpertsUsed);
    for (int32_t expert : private_experts) {
        if (expert < 0 || uint32_t(expert) >= kExperts) {
            throw std::invalid_argument("deepseek_pto_hash_router_expert");
        }
        result.push_back(uint32_t(expert));
    }
    return result;
}

inline bool compressor_step(const MappedWeights &weights, uint32_t layer,
                            std::string_view prefix, uint32_t position,
                            const F32View &normalized_hidden,
                            const CompressorStateView &state,
                            const F32View &output) {
    state.validate();
    normalized_hidden.validate();
    output.validate();
    if (normalized_hidden.rows != 1 ||
        normalized_hidden.columns != kHidden || output.rows != 1 ||
        output.columns != state.head_dim) {
        throw std::invalid_argument("deepseek_pto_compressor_step_geometry");
    }
    const std::string base(prefix);
    LocalMatrix projected_kv(1, state.width());
    LocalMatrix projected_gate(1, state.width());
    f16_linear(normalized_hidden,
               layer_tensor(weights, layer, base + "_kv"),
               projected_kv.view());
    f16_linear(normalized_hidden,
               layer_tensor(weights, layer, base + "_gate"),
               projected_gate.view());
    const uint32_t ape_row = position % state.compress_ratio;
    const WeightTensor ape = f16_vector_row(
        layer_tensor(weights, layer, base + "_ape"), ape_row,
        state.compress_ratio, state.width());
    LocalMatrix ape_values(1, state.width());
    decode_f16_vector(ape, ape_values.view());
    LocalMatrix projected_score(1, state.width());
    residual(projected_gate.view(), ape_values.view(),
             projected_score.view());
    LocalMatrix pooled(1, state.head_dim);
    if (!compressor_update_pre_scored(position, projected_kv.view(),
                                      projected_score.view(), state,
                                      pooled.view())) {
        return false;
    }

    const F32View norm = f32_weight_view(
        layer_tensor(weights, layer, base + "_norm"), state.head_dim);
    rms_norm(pooled.view(), &norm, output, kEpsilon);
    const uint32_t compressed_position =
        position + 1 - state.compress_ratio;
    RopeFactors rope = rope_factors(layer, compressed_position);
    rope_tail(output, kRopeDim, rope.cos_view(), rope.sin_view(), false,
              output);
    if (state.head_dim == kHeadDim) {
        fp8_kv_roundtrip(output, kRopeDim);
    } else if (state.head_dim == kIndexerHeadDim) {
        indexer_qat(output);
    } else {
        throw std::invalid_argument("deepseek_pto_compressor_head_dim");
    }
    return true;
}

inline std::vector<uint32_t>
selected_compressed_rows(const MappedWeights &weights, uint32_t layer,
                         const F32View &normalized_q_lora,
                         const F32View &normalized_hidden,
                         const LayerStateView &state,
                         uint32_t compressed_rows,
                         const RopeFactors &rope) {
    if (!compressed_rows) return {};
    if (!state.compressed || compressed_rows > state.compressed->rows) {
        throw std::invalid_argument("deepseek_pto_compressed_frontier");
    }
    if (state.ratio != 4) {
        std::vector<uint32_t> result(compressed_rows);
        for (uint32_t row = 0; row < compressed_rows; ++row) {
            result[row] = row;
        }
        return result;
    }
    if (!state.indexer_compressed ||
        compressed_rows > state.indexer_compressed->rows) {
        throw std::invalid_argument("deepseek_pto_indexer_frontier");
    }

    LocalMatrix query(1, kIndexerHeads * kIndexerHeadDim);
    const WeightTensor query_weight =
        layer_tensor(weights, layer, "indexer.attn_q_b");
    if (query_weight.tensor_type == kGgufTypeF16) {
        f16_linear(normalized_q_lora, query_weight, query.view());
    } else if (query_weight.tensor_type == kGgufTypeQ8_0) {
        q8_0_linear(normalized_q_lora, query_weight, query.view());
    } else {
        throw std::invalid_argument("deepseek_pto_indexer_query_type");
    }
    const F32View query_heads {query.view().address, kIndexerHeads,
                               kIndexerHeadDim, kIndexerHeadDim, 1};
    rope_tail(query_heads, kRopeDim, rope.cos_view(), rope.sin_view(), false,
              query_heads);
    indexer_qat(query_heads);
    LocalMatrix head_weights(1, kIndexerHeads);
    f16_linear(normalized_hidden,
               layer_tensor(weights, layer, "indexer.proj"),
               head_weights.view());
    const F32View active_indexer =
        prefix_rows(*state.indexer_compressed, compressed_rows);
    return sparse_indexer(query_heads, active_indexer, head_weights.view(),
                          kIndexerTopK);
}

inline LocalMatrix mixed_kv(const LayerStateView &state, uint32_t raw_rows,
                            uint32_t compressed_rows,
                            std::span<const uint32_t> selected_rows) {
    const F32View active_raw = prefix_rows(state.raw, raw_rows);
    if (selected_rows.size() > compressed_rows ||
        (!selected_rows.empty() && !state.compressed)) {
        throw std::invalid_argument("deepseek_pto_mixed_kv_geometry");
    }
    LocalMatrix result(raw_rows + selected_rows.size(), kHeadDim);
    const F32View raw_destination {result.view().address, raw_rows, kHeadDim,
                                   kHeadDim, 1};
    copy(active_raw, raw_destination);
    if (!selected_rows.empty()) {
        const F32View active_compressed =
            prefix_rows(*state.compressed, compressed_rows);
        const F32View compressed_destination {
            result.view().at(raw_rows),
            static_cast<uint32_t>(selected_rows.size()), kHeadDim, kHeadDim,
            1};
        gather_kv_rows(active_compressed, selected_rows,
                       compressed_destination);
    }
    return result;
}

inline LocalMatrix attention_layer(const MappedWeights &weights,
                                   uint32_t layer, uint32_t position,
                                   const F32View &residual_hc,
                                   LayerStateView &state) {
    HcPrepared prepared = prepare_hc(
        weights, layer, "hc_attn_fn", "hc_attn_scale", "hc_attn_base",
        "attn_norm", residual_hc);
    LocalMatrix q_lora(1, kQLora);
    q8_0_linear(prepared.normalized.view(),
                layer_tensor(weights, layer, "attn_q_a"), q_lora.view());
    const F32View q_lora_norm = f32_weight_view(
        layer_tensor(weights, layer, "attn_q_a_norm"), kQLora);
    rms_norm(q_lora.view(), &q_lora_norm, q_lora.view(), kEpsilon);

    LocalMatrix q_flat(1, kAttentionHeads * kHeadDim);
    q8_0_linear(q_lora.view(), layer_tensor(weights, layer, "attn_q_b"),
                q_flat.view());
    const F32View q {q_flat.view().address, kAttentionHeads, kHeadDim,
                     kHeadDim, 1};
    head_rms_norm(q, nullptr, q, kEpsilon);
    RopeFactors rope = rope_factors(layer, position);
    rope_tail(q, kRopeDim, rope.cos_view(), rope.sin_view(), false, q);

    LocalMatrix current_kv(1, kHeadDim);
    q8_0_linear(prepared.normalized.view(),
                layer_tensor(weights, layer, "attn_kv"),
                current_kv.view());
    const F32View kv_norm = f32_weight_view(
        layer_tensor(weights, layer, "attn_kv_a_norm"), kHeadDim);
    rms_norm(current_kv.view(), &kv_norm, current_kv.view(), kEpsilon);
    rope_tail(current_kv.view(), kRopeDim, rope.cos_view(), rope.sin_view(),
              false, current_kv.view());
    fp8_kv_roundtrip(current_kv.view(), kRopeDim);
    f16_roundtrip(current_kv.view());
    const uint32_t previous_raw_rows =
        std::min(position, kSlidingWindow);
    append_bounded_row(state.raw, previous_raw_rows, kSlidingWindow,
                       current_kv.view());
    const uint32_t raw_rows =
        std::min(position + 1, kSlidingWindow);

    uint32_t compressed_rows = 0;
    if (state.ratio) {
        if (!state.attention_compressor) {
            throw std::invalid_argument("deepseek_pto_attention_state");
        }
        LocalMatrix compressed(1, kHeadDim);
        if (compressor_step(weights, layer, "attn_compressor", position,
                            prepared.normalized.view(),
                            *state.attention_compressor,
                            compressed.view())) {
            if (!state.compressed) {
                throw std::invalid_argument(
                    "deepseek_pto_attention_compressed_state");
            }
            const uint32_t previous = position / state.ratio;
            append_bounded_row(*state.compressed, previous,
                               state.compressed->rows, compressed.view());
        }
        if (state.ratio == 4) {
            if (!state.indexer_compressor) {
                throw std::invalid_argument("deepseek_pto_indexer_state");
            }
            LocalMatrix indexer_compressed(1, kIndexerHeadDim);
            if (compressor_step(weights, layer, "indexer_compressor",
                                position, prepared.normalized.view(),
                                *state.indexer_compressor,
                                indexer_compressed.view())) {
                if (!state.indexer_compressed) {
                    throw std::invalid_argument(
                        "deepseek_pto_indexer_compressed_state");
                }
                const uint32_t previous = position / state.ratio;
                append_bounded_row(*state.indexer_compressed, previous,
                                   state.indexer_compressed->rows,
                                   indexer_compressed.view());
            }
        }
        compressed_rows = (position + 1) / state.ratio;
    }

    const auto selected = selected_compressed_rows(
        weights, layer, q_lora.view(), prepared.normalized.view(), state,
        compressed_rows, rope);
    LocalMatrix attention_kv =
        mixed_kv(state, raw_rows, compressed_rows, selected);
    LocalMatrix heads(kAttentionHeads, kHeadDim);
    const F32View sinks = f32_weight_view(
        layer_tensor(weights, layer, "attn_sinks"), kAttentionHeads);
    sink_attention(q, attention_kv.view(), sinks, heads.view());
    rope_tail(heads.view(), kRopeDim, rope.cos_view(), rope.sin_view(), true,
              heads.view());

    LocalMatrix output_a(1, kOutputGroups * kOLora);
    const F32View flat_heads {heads.view().address, 1,
                              kAttentionHeads * kHeadDim,
                              kAttentionHeads * kHeadDim, 1};
    grouped_q8_0_linear(flat_heads,
                        layer_tensor(weights, layer, "attn_output_a"),
                        kOutputGroups, output_a.view());
    LocalMatrix attention_output(1, kHidden);
    q8_0_linear(output_a.view(),
                layer_tensor(weights, layer, "attn_output_b"),
                attention_output.view());
    LocalMatrix output_hc(kHcMult, kHidden);
    hc_post(attention_output.view(), residual_hc, prepared.post.view(),
            prepared.combine.view(), output_hc.view());
    return output_hc;
}

inline LocalMatrix ffn_layer(const MappedWeights &weights,
                            const Iq2Codebook &codebook, uint32_t layer,
                            uint32_t token, const F32View &residual_hc) {
    HcPrepared prepared = prepare_hc(
        weights, layer, "hc_ffn_fn", "hc_ffn_scale", "hc_ffn_base",
        "ffn_norm", residual_hc);
    LocalMatrix logits(1, kExperts);
    f16_linear(prepared.normalized.view(),
               layer_tensor(weights, layer, "ffn_gate_inp"),
               logits.view());
    std::optional<F32View> selection_bias;
    const std::string selection_bias_name =
        selection_bias_tensor_name(layer);
    if (weights.contains(selection_bias_name)) {
        selection_bias =
            f32_weight_view(weights.tensor(selection_bias_name), kExperts);
    }
    std::vector<uint32_t> selected_experts;
    if (layer < 3) {
        selected_experts = hash_router_experts(weights, layer, token);
    }
    LocalMatrix probabilities(1, kExperts);
    const RouterSelection selection = router(
        logits.view(), selection_bias ? &*selection_bias : nullptr,
        selected_experts, kExpertsUsed, kExpertWeightScale,
        probabilities.view());

    LocalMatrix routed(1, kHidden);
    routed_experts(prepared.normalized.view(),
                   layer_tensor(weights, layer, "ffn_gate_exps"),
                   layer_tensor(weights, layer, "ffn_up_exps"),
                   layer_tensor(weights, layer, "ffn_down_exps"), codebook,
                   selection.expert_indices, selection.expert_weights,
                   kSwigluClamp, routed.view());
    LocalMatrix shared(1, kHidden);
    shared_expert(prepared.normalized.view(),
                  layer_tensor(weights, layer, "ffn_gate_shexp"),
                  layer_tensor(weights, layer, "ffn_up_shexp"),
                  layer_tensor(weights, layer, "ffn_down_shexp"),
                  kSwigluClamp, shared.view());
    LocalMatrix moe(1, kHidden);
    residual(routed.view(), shared.view(), moe.view());
    LocalMatrix output_hc(kHcMult, kHidden);
    hc_post(moe.view(), residual_hc, prepared.post.view(),
            prepared.combine.view(), output_hc.view());
    return output_hc;
}

inline void output_head(const MappedWeights &weights,
                        const F32View &hidden_hc,
                        const F32View &logits,
                        const F32View *trace_logits = nullptr) {
    hidden_hc.validate();
    logits.validate();
    if (hidden_hc.rows != kHcMult || hidden_hc.columns != kHidden ||
        hidden_hc.row_stride != kHidden || hidden_hc.column_stride != 1 ||
        logits.rows != 1 || logits.columns != kVocab) {
        throw std::invalid_argument("deepseek_pto_output_head_geometry");
    }
    LocalMatrix normalized_hc(kHcMult, kHidden);
    rms_norm(hidden_hc, nullptr, normalized_hc.view(), kEpsilon);
    const F32View flat_hc {normalized_hc.view().address, 1,
                           kHcMult * kHidden, kHcMult * kHidden, 1};
    LocalMatrix projected(1, kHcMult);
    f16_linear(flat_hc, weights.tensor("output_hc_fn.weight"),
               projected.view());
    const F32View scale =
        f32_weight_view(weights.tensor("output_hc_scale.weight"), 1);
    const F32View base =
        f32_weight_view(weights.tensor("output_hc_base.weight"), kHcMult);
    F32Scalar scale_value;
    load_scalar(scale_value, scale, 0, 0);
    LocalMatrix hc_weights(1, kHcMult);
    sigmoid_affine(projected.view(), base, 0, kHcMult, scale_value, 1.0f,
                   kEpsilon, hc_weights.view(), 0);
    LocalMatrix embedding(1, kHidden);
    hc_weighted_sum(hidden_hc, hc_weights.view(), embedding.view());
    const F32View norm =
        f32_weight_view(weights.tensor("output_norm.weight"), kHidden);
    rms_norm(embedding.view(), &norm, embedding.view(), kEpsilon);
    q8_0_linear(embedding.view(), weights.tensor("output.weight"), logits,
                trace_logits);
}

inline void initialize_compressors(RangeStateView &state) {
    for (LayerStateView &layer : state.layers) {
        if (layer.attention_compressor) {
            fill(layer.attention_compressor->kv, 0.0f);
            fill(layer.attention_compressor->score,
                 -std::numeric_limits<float>::infinity());
        }
        if (layer.indexer_compressor) {
            fill(layer.indexer_compressor->kv, 0.0f);
            fill(layer.indexer_compressor->score,
                 -std::numeric_limits<float>::infinity());
        }
    }
}

inline void validate_range_io(const RangeGeometry &geometry,
                              const F32View &hidden_input,
                              const ByteView &previous_state,
                              const I32View &token_input,
                              const F32View &hidden_output,
                              const ByteView &next_state,
                              const F32View &logits) {
    geometry.validate();
    hidden_input.validate();
    previous_state.validate();
    token_input.validate();
    hidden_output.validate();
    next_state.validate();
    logits.validate();
    const uint32_t hc_width = kHcMult * kHidden;
    const bool first_hidden =
        geometry.first == 0 && hidden_input.rows == 1 &&
        hidden_input.columns == 1 && hidden_input.row_stride == 1 &&
        hidden_input.column_stride == 1;
    const bool intermediate_hidden =
        geometry.first != 0 && hidden_input.rows == geometry.tokens &&
        hidden_input.columns == hc_width &&
        hidden_input.row_stride == hc_width &&
        hidden_input.column_stride == 1;
    const uint32_t expected_tokens = geometry.tokens;
    const uint32_t expected_logits =
        geometry.end == geometry.layers ? kVocab : 1;
    if ((!first_hidden && !intermediate_hidden) ||
        token_input.elements != expected_tokens ||
        hidden_output.rows != geometry.tokens ||
        hidden_output.columns != hc_width ||
        hidden_output.row_stride != hc_width ||
        hidden_output.column_stride != 1 ||
        (geometry.past == 0 && previous_state.bytes != 1) ||
        logits.rows != 1 || logits.columns != expected_logits ||
        logits.row_stride != expected_logits || logits.column_stride != 1) {
        throw std::invalid_argument("deepseek_pto_range_io_geometry");
    }
}

inline void decoder_range(const MappedWeights &weights,
                          const Iq2Codebook &codebook,
                          const RangeGeometry &geometry,
                          const F32View &hidden_input,
                          const ByteView &previous_state,
                          const I32View &token_input,
                          const F32View &hidden_output,
                          const ByteView &next_state,
                          const F32View &logits,
                          const F32View *trace_logits = nullptr) {
    validate_range_io(geometry, hidden_input, previous_state, token_input,
                      hidden_output, next_state, logits);
    codebook.validate();
    const RangeStateGeometry next_geometry {
        geometry.first,
        geometry.end,
        geometry.past + geometry.tokens,
        kHeadDim,
        kIndexerHeadDim,
        kSlidingWindow,
    };
    RangeStateView next = range_state(next_state, next_geometry, true);
    if (geometry.past) {
        const RangeStateGeometry previous_geometry {
            geometry.first,
            geometry.end,
            geometry.past,
            kHeadDim,
            kIndexerHeadDim,
            kSlidingWindow,
        };
        const RangeStateView previous =
            range_state(previous_state, previous_geometry, false);
        copy_range_state(previous, next);
    } else {
        initialize_compressors(next);
    }

    for (uint32_t token_index = 0; token_index < geometry.tokens;
         ++token_index) {
        const uint32_t token = token_id_at(token_input, token_index);
        LocalMatrix hidden_hc(kHcMult, kHidden);
        if (geometry.first == 0) {
            embedding(weights, token, hidden_hc.view());
        } else {
            const F32View source {
                hidden_input.at(token_index), kHcMult, kHidden, kHidden, 1};
            copy(source, hidden_hc.view());
        }
        const uint32_t position = geometry.past + token_index;
        for (uint32_t layer = geometry.first; layer < geometry.end;
             ++layer) {
            LayerStateView &layer_state =
                next.layers.at(layer - geometry.first);
            hidden_hc = attention_layer(weights, layer, position,
                                        hidden_hc.view(), layer_state);
            hidden_hc = ffn_layer(weights, codebook, layer,
                                  token, hidden_hc.view());
        }
        const F32View flattened {hidden_hc.view().address, 1,
                                 kHcMult * kHidden, kHcMult * kHidden, 1};
        const F32View destination {
            hidden_output.at(token_index), 1, kHcMult * kHidden,
            hidden_output.row_stride, hidden_output.column_stride};
        copy(flattened, destination);
        if (geometry.end == geometry.layers &&
            token_index + 1 == geometry.tokens) {
            output_head(weights, hidden_hc.view(), logits, trace_logits);
        }
    }
}

} // namespace lingqu::deepseek_v4_flash_pto
