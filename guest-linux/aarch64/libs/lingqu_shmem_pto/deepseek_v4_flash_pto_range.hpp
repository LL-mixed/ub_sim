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

inline std::vector<uint32_t> token_ids(const I32View &source,
                                       uint32_t count) {
    source.validate();
    if (!count || count > source.elements || count > kVectorCapacity) {
        throw std::invalid_argument("deepseek_pto_token_geometry");
    }
    I32Row tile(count);
    std::vector<int32_t> private_ids(count);
    auto input = global_row<int32_t>(source.address, count);
    auto output = global_row<int32_t>(
        reinterpret_cast<uintptr_t>(private_ids.data()), count);
    pto::TLOAD(tile, input);
    pto::TSTORE(output, tile);
    ++operator_counts.tload;
    ++operator_counts.tstore;
    std::vector<uint32_t> result;
    result.reserve(count);
    for (int32_t value : private_ids) {
        if (value < 0 || uint32_t(value) >= kVocab) {
            throw std::invalid_argument("deepseek_pto_token_id");
        }
        result.push_back(uint32_t(value));
    }
    return result;
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

struct RopeFactors {
    std::vector<float> cosine;
    std::vector<float> sine;

    F32View cos_view() {
        return F32View {reinterpret_cast<uintptr_t>(cosine.data()), 1,
                        static_cast<uint32_t>(cosine.size()),
                        static_cast<uint32_t>(cosine.size()), 1};
    }
    F32View sin_view() {
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
    const uint8_t *row =
        table.payload + uint64_t(token) * kExpertsUsed * sizeof(int32_t);
    std::vector<uint32_t> result;
    result.reserve(kExpertsUsed);
    for (uint32_t slot = 0; slot < kExpertsUsed; ++slot) {
        int32_t expert;
        std::memcpy(&expert, row + slot * sizeof(expert), sizeof(expert));
        if (expert < 0 || uint32_t(expert) >= kExperts) {
            throw std::invalid_argument("deepseek_pto_hash_router_expert");
        }
        result.push_back(uint32_t(expert));
    }
    return result;
}

} // namespace lingqu::deepseek_v4_flash_pto
