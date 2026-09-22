#include "deepseek_v4_flash_pto_ops.hpp"

#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace ds = lingqu::deepseek_v4_flash_pto;

#define CHECK(condition) do { if (!(condition)) return __LINE__; } while (0)

int main() {
    std::array<float, 32> input {};
    input.fill(1.0f);
    std::array<uint8_t, 68> encoded {};
    encoded[0] = 0x00;
    encoded[1] = 0x3c;
    encoded[34] = 0x00;
    encoded[35] = 0x38;
    for (size_t lane = 0; lane < 32; ++lane) {
        encoded[2 + lane] = 1;
        encoded[36 + lane] = static_cast<uint8_t>(int8_t(-2));
    }
    std::array<float, 2> output {};
    const ds::F32View input_view {
        reinterpret_cast<uintptr_t>(input.data()), 1, 32, 32, 1};
    const ds::F32View output_view {
        reinterpret_cast<uintptr_t>(output.data()), 1, 2, 2, 1};
    const ds::WeightTensor weight {
        .payload = encoded.data(),
        .bytes = encoded.size(),
        .elements = 64,
        .dimensions = {32, 2},
        .tensor_type = ds::kGgufTypeQ8_0,
        .rank = 2,
    };
    ds::operator_counts = {};
    ds::q8_0_linear(input_view, weight, output_view);
    CHECK(std::abs(output[0] - 32.0f) < 1.0e-5f);
    CHECK(std::abs(output[1] + 32.0f) < 1.0e-5f);
    CHECK(ds::operator_counts.q8_linear == 1);
    CHECK(ds::operator_counts.tload > 0);
    CHECK(ds::operator_counts.tstore > 0);

    std::array<uint8_t, 16> f16_weight {
        0x00, 0x3c, 0x00, 0x40, 0x00, 0x42, 0x00, 0x44,
        0x00, 0xbc, 0x00, 0x38, 0x00, 0x00, 0x00, 0x48};
    std::array<float, 4> f16_input {1.0f, 2.0f, 3.0f, 4.0f};
    std::array<float, 2> f16_output {};
    const ds::F32View f16_input_view {
        reinterpret_cast<uintptr_t>(f16_input.data()), 1, 4, 4, 1};
    const ds::F32View f16_output_view {
        reinterpret_cast<uintptr_t>(f16_output.data()), 1, 2, 2, 1};
    const ds::WeightTensor f16_matrix {
        .payload = f16_weight.data(),
        .bytes = f16_weight.size(),
        .elements = 8,
        .dimensions = {4, 2},
        .tensor_type = ds::kGgufTypeF16,
        .rank = 2,
    };
    ds::f16_linear(f16_input_view, f16_matrix, f16_output_view);
    CHECK(std::abs(f16_output[0] - 30.0f) < 1.0e-5f);
    CHECK(std::abs(f16_output[1] - 32.0f) < 1.0e-5f);

    std::array<float, 4> norm_input {3.0f, 4.0f, 0.0f, 0.0f};
    std::array<float, 4> norm_output {};
    const ds::F32View norm_input_view {
        reinterpret_cast<uintptr_t>(norm_input.data()), 1, 4, 4, 1};
    const ds::F32View norm_output_view {
        reinterpret_cast<uintptr_t>(norm_output.data()), 1, 4, 4, 1};
    ds::rms_norm(norm_input_view, nullptr, norm_output_view, 1.0e-6f);
    const float inv_rms = 1.0f / std::sqrt(6.25f + 1.0e-6f);
    CHECK(std::abs(norm_output[0] - 3.0f * inv_rms) < 1.0e-6f);
    CHECK(std::abs(norm_output[1] - 4.0f * inv_rms) < 1.0e-6f);

    std::array<float, 8> hc_mix {0.2f, -0.4f, 0.3f, 0.7f,
                                 0.1f, -0.2f, 0.4f, -0.3f};
    std::array<float, 3> hc_scale {0.5f, 0.75f, 1.25f};
    std::array<float, 8> hc_base {-0.1f, 0.2f, 0.0f, -0.2f,
                                  0.3f, 0.1f, -0.1f, 0.25f};
    std::array<float, 2> hc_pre {};
    std::array<float, 2> hc_post_values {};
    std::array<float, 4> hc_combine {};
    const ds::F32View hc_mix_view {
        reinterpret_cast<uintptr_t>(hc_mix.data()), 1, 8, 8, 1};
    const ds::F32View hc_scale_view {
        reinterpret_cast<uintptr_t>(hc_scale.data()), 1, 3, 3, 1};
    const ds::F32View hc_base_view {
        reinterpret_cast<uintptr_t>(hc_base.data()), 1, 8, 8, 1};
    const ds::F32View hc_pre_view {
        reinterpret_cast<uintptr_t>(hc_pre.data()), 1, 2, 2, 1};
    const ds::F32View hc_post_values_view {
        reinterpret_cast<uintptr_t>(hc_post_values.data()), 1, 2, 2, 1};
    const ds::F32View hc_combine_view {
        reinterpret_cast<uintptr_t>(hc_combine.data()), 2, 2, 2, 1};
    ds::hc_split(hc_mix_view, hc_scale_view, hc_base_view, 2, 2, 1.0e-6f,
                 hc_pre_view, hc_post_values_view, hc_combine_view);
    const auto sigmoid = [](float value) {
        return 1.0f / (1.0f + std::exp(-value));
    };
    CHECK(std::abs(hc_pre[0] -
                   (sigmoid(hc_mix[0] * hc_scale[0] + hc_base[0]) +
                    1.0e-6f)) < 1.0e-6f);
    CHECK(std::abs(hc_pre[1] -
                   (sigmoid(hc_mix[1] * hc_scale[0] + hc_base[1]) +
                    1.0e-6f)) < 1.0e-6f);
    CHECK(std::abs(hc_post_values[0] -
                   2.0f * sigmoid(hc_mix[2] * hc_scale[1] + hc_base[2])) <
          1.0e-6f);
    CHECK(std::abs(hc_post_values[1] -
                   2.0f * sigmoid(hc_mix[3] * hc_scale[1] + hc_base[3])) <
          1.0e-6f);
    for (size_t source = 0; source < 2; ++source) {
        const float column_sum = hc_combine[source] + hc_combine[2 + source];
        CHECK(std::abs(column_sum - 1.0f) < 5.0e-6f);
    }

    std::array<float, 8> residual_hc {1, 2, 3, 4, 5, 6, 7, 8};
    std::array<float, 2> hc_weights {0.25f, 0.75f};
    std::array<float, 4> weighted {};
    const ds::F32View residual_hc_view {
        reinterpret_cast<uintptr_t>(residual_hc.data()), 2, 4, 4, 1};
    const ds::F32View hc_weights_view {
        reinterpret_cast<uintptr_t>(hc_weights.data()), 1, 2, 2, 1};
    const ds::F32View weighted_view {
        reinterpret_cast<uintptr_t>(weighted.data()), 1, 4, 4, 1};
    ds::hc_weighted_sum(residual_hc_view, hc_weights_view, weighted_view);
    for (size_t index = 0; index < weighted.size(); ++index) {
        CHECK(std::abs(weighted[index] -
                       (0.25f * residual_hc[index] +
                        0.75f * residual_hc[4 + index])) < 1.0e-6f);
    }

    std::array<float, 2> post {0.5f, 1.5f};
    std::array<float, 4> combine {0.75f, 0.25f, 0.125f, 0.875f};
    std::array<float, 8> post_output {};
    const ds::F32View post_view {
        reinterpret_cast<uintptr_t>(post.data()), 1, 2, 2, 1};
    const ds::F32View combine_view {
        reinterpret_cast<uintptr_t>(combine.data()), 2, 2, 2, 1};
    const ds::F32View post_output_view {
        reinterpret_cast<uintptr_t>(post_output.data()), 2, 4, 4, 1};
    ds::hc_post(weighted_view, residual_hc_view, post_view, combine_view,
                post_output_view);
    for (size_t column = 0; column < 4; ++column) {
        const float expected0 = weighted[column] * 0.5f +
                                residual_hc[column] * 0.75f +
                                residual_hc[4 + column] * 0.125f;
        const float expected1 = weighted[column] * 1.5f +
                                residual_hc[column] * 0.25f +
                                residual_hc[4 + column] * 0.875f;
        CHECK(std::abs(post_output[column] - expected0) < 1.0e-6f);
        CHECK(std::abs(post_output[4 + column] - expected1) < 1.0e-6f);
    }

    std::array<float, 4> gate {-20.0f, -1.0f, 1.0f, 20.0f};
    std::array<float, 4> up {-20.0f, 2.0f, 3.0f, 20.0f};
    std::array<float, 4> swiglu_output {};
    const ds::F32View gate_view {
        reinterpret_cast<uintptr_t>(gate.data()), 1, 4, 4, 1};
    const ds::F32View up_view {
        reinterpret_cast<uintptr_t>(up.data()), 1, 4, 4, 1};
    const ds::F32View swiglu_view {
        reinterpret_cast<uintptr_t>(swiglu_output.data()), 1, 4, 4, 1};
    ds::swiglu(gate_view, up_view, swiglu_view, 10.0f);
    for (size_t index = 0; index < 4; ++index) {
        const float bounded_gate = std::min(gate[index], 10.0f);
        const float bounded_up = std::clamp(up[index], -10.0f, 10.0f);
        const float expected = bounded_gate /
                               (1.0f + std::exp(-bounded_gate)) * bounded_up;
        CHECK(std::abs(swiglu_output[index] - expected) < 2.0e-5f);
    }

    bool rejected = false;
    try {
        auto invalid = weight;
        invalid.tensor_type = 1;
        ds::q8_0_linear(input_view, invalid, output_view);
    } catch (const std::invalid_argument &) {
        rejected = true;
    }
    CHECK(rejected);
    std::printf(
        "deepseek_v4_flash_pto_ops=pass q8_linear=1 f16_linear=1 rms_norm=1 "
        "hc_split=1 hc_weighted_sum=1 hc_post=1 swiglu=1 "
        "tload=%llu tstore=%llu\n",
        static_cast<unsigned long long>(ds::operator_counts.tload),
        static_cast<unsigned long long>(ds::operator_counts.tstore));
    return 0;
}
