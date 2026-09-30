#include "deepseek_v4_flash_pto_ops.hpp"

#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace ds = lingqu::deepseek_v4_flash_pto;

#define CHECK(condition) do { if (!(condition)) return __LINE__; } while (0)

int main() {
    // Token-42 first-layer heads on opposite sides of a Q8 rounding boundary.
    std::array<float, 64> boundary_input {};
    boundary_input[0] = 1.04024875f;
    boundary_input[27] = 0.520124376f;
    boundary_input[32] = 1.04024887f;
    boundary_input[59] = 0.520124435f;
    const ds::F32View boundary_view {
        reinterpret_cast<uintptr_t>(boundary_input.data()), 2, 32, 32, 1};
    const ds::QuantizedQ8Block ds4_input_quantized =
        ds::quantize_q8_0(boundary_view, 0, 0);
    const ds::QuantizedQ8Block pto_input_quantized =
        ds::quantize_q8_0(boundary_view, 1, 0);
    CHECK(ds4_input_quantized.values[27] == 63);
    CHECK(pto_input_quantized.values[27] == 64);

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
    const uint64_t base_tstores = ds::operator_counts.tstore;
    std::array<float, 2> trace_output {};
    const ds::F32View trace_output_view {
        reinterpret_cast<uintptr_t>(trace_output.data()), 1, 2, 2, 1};
    ds::operator_counts = {};
    ds::q8_0_linear(input_view, weight, output_view, &trace_output_view);
    CHECK(trace_output == output);
    CHECK(ds::operator_counts.tstore == base_tstores + output_view.columns);

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

    const auto q2_block = [](const std::array<uint8_t, 16> &scales,
                             uint8_t quant, uint16_t scale,
                             uint16_t minimum) {
        std::vector<uint8_t> block(ds::kQ2KBlockBytes, quant);
        std::copy(scales.begin(), scales.end(), block.begin());
        block[80] = static_cast<uint8_t>(scale & 0xff);
        block[81] = static_cast<uint8_t>(scale >> 8);
        block[82] = static_cast<uint8_t>(minimum & 0xff);
        block[83] = static_cast<uint8_t>(minimum >> 8);
        return block;
    };
    const std::vector<uint8_t> q2_zero = q2_block({}, 0, 0x3c00, 0);
    std::array<uint8_t, 16> q2_quant_scales {};
    q2_quant_scales.fill(2);
    const std::vector<uint8_t> q2_quant =
        q2_block(q2_quant_scales, 0xff, 0x3c00, 0);
    std::array<uint8_t, 16> q2_min_scales {};
    q2_min_scales.fill(0x10);
    const std::vector<uint8_t> q2_minimum =
        q2_block(q2_min_scales, 0, 0, 0x3c00);
    std::vector<uint8_t> q2_payload;
    q2_payload.reserve(4 * ds::kQ2KBlockBytes);
    q2_payload.insert(q2_payload.end(), q2_zero.begin(), q2_zero.end());
    q2_payload.insert(q2_payload.end(), q2_zero.begin(), q2_zero.end());
    q2_payload.insert(q2_payload.end(), q2_quant.begin(), q2_quant.end());
    q2_payload.insert(q2_payload.end(), q2_minimum.begin(), q2_minimum.end());
    std::array<float, 256> expert_input {};
    expert_input.fill(1.0f);
    std::array<float, 2> q2_output {};
    const ds::F32View expert_input_view {
        reinterpret_cast<uintptr_t>(expert_input.data()), 1, 256, 256, 1};
    const ds::F32View q2_output_view {
        reinterpret_cast<uintptr_t>(q2_output.data()), 1, 2, 2, 1};
    const ds::WeightTensor q2_weight {
        .payload = q2_payload.data(),
        .bytes = q2_payload.size(),
        .elements = 1024,
        .dimensions = {256, 2, 2},
        .tensor_type = ds::kGgufTypeQ2_K,
        .rank = 3,
    };
    ds::q2_k_expert_linear(expert_input_view, q2_weight, 0,
                           q2_output_view);
    CHECK(std::abs(q2_output[0]) < 1.0e-6f);
    CHECK(std::abs(q2_output[1]) < 1.0e-6f);
    ds::q2_k_expert_linear(expert_input_view, q2_weight, 1,
                           q2_output_view);
    CHECK(std::abs(q2_output[0] - 1536.0f) < 1.0e-4f);
    CHECK(std::abs(q2_output[1] + 256.0f) < 1.0e-4f);

    std::vector<uint8_t> iq2_payload(2 * ds::kIq2XxsBlockBytes, 0);
    iq2_payload[0] = 0x00;
    iq2_payload[1] = 0x3c;
    iq2_payload[ds::kIq2XxsBlockBytes] = 0x00;
    iq2_payload[ds::kIq2XxsBlockBytes + 1] = 0x3c;
    for (uint32_t group = 0; group < 8; ++group) {
        const uint32_t sign_and_scale = 127u | (15u << 28);
        const size_t offset = ds::kIq2XxsBlockBytes + 2 + group * 8 + 4;
        iq2_payload[offset] = static_cast<uint8_t>(sign_and_scale & 0xff);
        iq2_payload[offset + 1] =
            static_cast<uint8_t>((sign_and_scale >> 8) & 0xff);
        iq2_payload[offset + 2] =
            static_cast<uint8_t>((sign_and_scale >> 16) & 0xff);
        iq2_payload[offset + 3] =
            static_cast<uint8_t>((sign_and_scale >> 24) & 0xff);
    }
    std::array<uint64_t, 256> iq2_grid {};
    std::array<uint8_t, 128> iq2_signs {};
    iq2_grid[0] = 0x0808080808080808ULL;
    iq2_signs[127] = 0xff;
    const ds::Iq2Codebook iq2_codebook {
        std::span<const uint64_t>(iq2_grid),
        std::span<const uint8_t>(iq2_signs)};
    std::array<float, 1> iq2_output {};
    const ds::F32View iq2_output_view {
        reinterpret_cast<uintptr_t>(iq2_output.data()), 1, 1, 1, 1};
    const ds::WeightTensor iq2_weight {
        .payload = iq2_payload.data(),
        .bytes = iq2_payload.size(),
        .elements = 512,
        .dimensions = {256, 1, 2},
        .tensor_type = ds::kGgufTypeIq2Xxs,
        .rank = 3,
    };
    ds::iq2_xxs_expert_linear(expert_input_view, iq2_weight, 0,
                              iq2_codebook, iq2_output_view);
    CHECK(std::abs(iq2_output[0] - 256.0f) < 1.0e-4f);
    ds::iq2_xxs_expert_linear(expert_input_view, iq2_weight, 1,
                              iq2_codebook, iq2_output_view);
    CHECK(std::isfinite(iq2_output[0]));
    CHECK(std::abs(iq2_output[0] - 256.0f) > 1.0e-4f);

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

    std::array<float, 4> head_norm_input {3.0f, 4.0f, 0.0f, 5.0f};
    std::array<float, 4> head_norm_output {};
    const ds::F32View head_norm_input_view {
        reinterpret_cast<uintptr_t>(head_norm_input.data()), 2, 2, 2, 1};
    const ds::F32View head_norm_output_view {
        reinterpret_cast<uintptr_t>(head_norm_output.data()), 2, 2, 2, 1};
    ds::head_rms_norm(head_norm_input_view, nullptr, head_norm_output_view,
                      1.0e-6f);
    const float head_inv_rms = 1.0f / std::sqrt(12.5f + 1.0e-6f);
    CHECK(std::abs(head_norm_output[0] - 3.0f * head_inv_rms) < 1.0e-6f);
    CHECK(std::abs(head_norm_output[1] - 4.0f * head_inv_rms) < 1.0e-6f);
    CHECK(std::abs(head_norm_output[2]) < 1.0e-6f);
    CHECK(std::abs(head_norm_output[3] - 5.0f * head_inv_rms) < 1.0e-6f);

    const std::array<float, 6> rope_original {10.0f, 20.0f, 1.0f,
                                               2.0f, 3.0f, 4.0f};
    std::array<float, 6> rope_values = rope_original;
    const float angle0 = 0.25f;
    const float angle1 = -0.5f;
    std::array<float, 2> rope_cos {std::cos(angle0), std::cos(angle1)};
    std::array<float, 2> rope_sin {std::sin(angle0), std::sin(angle1)};
    const ds::F32View rope_view {
        reinterpret_cast<uintptr_t>(rope_values.data()), 1, 6, 6, 1};
    const ds::F32View rope_cos_view {
        reinterpret_cast<uintptr_t>(rope_cos.data()), 1, 2, 2, 1};
    const ds::F32View rope_sin_view {
        reinterpret_cast<uintptr_t>(rope_sin.data()), 1, 2, 2, 1};
    ds::rope_tail(rope_view, 4, rope_cos_view, rope_sin_view, false,
                  rope_view);
    CHECK(rope_values[0] == rope_original[0]);
    CHECK(rope_values[1] == rope_original[1]);
    ds::rope_tail(rope_view, 4, rope_cos_view, rope_sin_view, true,
                  rope_view);
    for (size_t index = 0; index < rope_values.size(); ++index) {
        CHECK(std::abs(rope_values[index] - rope_original[index]) < 1.0e-5f);
    }

    std::array<float, 8> kv_cache {};
    std::array<float, 2> kv_first {5.0f, 6.0f};
    std::array<float, 2> kv_second {1.0f, 2.0f};
    std::array<float, 4> kv_gathered {};
    const ds::F32View kv_cache_view {
        reinterpret_cast<uintptr_t>(kv_cache.data()), 4, 2, 2, 1};
    const ds::F32View kv_first_view {
        reinterpret_cast<uintptr_t>(kv_first.data()), 1, 2, 2, 1};
    const ds::F32View kv_second_view {
        reinterpret_cast<uintptr_t>(kv_second.data()), 1, 2, 2, 1};
    const ds::F32View kv_gathered_view {
        reinterpret_cast<uintptr_t>(kv_gathered.data()), 2, 2, 2, 1};
    ds::write_kv_row(kv_cache_view, 2, kv_first_view);
    ds::write_kv_row(kv_cache_view, 0, kv_second_view);
    const std::array<uint32_t, 2> kv_rows {2, 0};
    ds::gather_kv_rows(kv_cache_view, std::span<const uint32_t>(kv_rows),
                       kv_gathered_view);
    CHECK((kv_gathered == std::array<float, 4> {5.0f, 6.0f, 1.0f, 2.0f}));
    bool kv_rejected = false;
    try {
        ds::write_kv_row(kv_cache_view, 4, kv_first_view);
    } catch (const std::invalid_argument &) {
        kv_rejected = true;
    }
    CHECK(kv_rejected);

    std::array<float, 4> attention_q {1.0f, 0.0f, 0.0f, 1.0f};
    std::array<float, 4> attention_kv {1.0f, 0.0f, 0.0f, 1.0f};
    std::array<float, 2> attention_sinks {};
    std::array<float, 4> attention_output {};
    const ds::F32View attention_q_view {
        reinterpret_cast<uintptr_t>(attention_q.data()), 2, 2, 2, 1};
    const ds::F32View attention_kv_view {
        reinterpret_cast<uintptr_t>(attention_kv.data()), 2, 2, 2, 1};
    const ds::F32View attention_sinks_view {
        reinterpret_cast<uintptr_t>(attention_sinks.data()), 1, 2, 2, 1};
    const ds::F32View attention_output_view {
        reinterpret_cast<uintptr_t>(attention_output.data()), 2, 2, 2, 1};
    ds::sink_attention(attention_q_view, attention_kv_view,
                       attention_sinks_view, attention_output_view);
    const float attention_high = std::exp(1.0f / std::sqrt(2.0f));
    const float attention_denominator = attention_high + 2.0f;
    const std::array<float, 4> attention_expected {
        attention_high / attention_denominator,
        1.0f / attention_denominator,
        1.0f / attention_denominator,
        attention_high / attention_denominator};
    for (size_t index = 0; index < attention_output.size(); ++index) {
        CHECK(std::abs(attention_output[index] - attention_expected[index]) <
              1.0e-6f);
    }

    std::array<float, 2> large_q {10000.0f, 10000.0f};
    std::array<float, 2> large_kv {10000.0f, 10000.0f};
    std::array<float, 1> large_sink {100000000.0f};
    std::array<float, 2> large_output {};
    const ds::F32View large_q_view {
        reinterpret_cast<uintptr_t>(large_q.data()), 1, 2, 2, 1};
    const ds::F32View large_kv_view {
        reinterpret_cast<uintptr_t>(large_kv.data()), 1, 2, 2, 1};
    const ds::F32View large_sink_view {
        reinterpret_cast<uintptr_t>(large_sink.data()), 1, 1, 1, 1};
    const ds::F32View large_output_view {
        reinterpret_cast<uintptr_t>(large_output.data()), 1, 2, 2, 1};
    ds::sink_attention(large_q_view, large_kv_view, large_sink_view,
                       large_output_view);
    CHECK(std::isfinite(large_output[0]));
    CHECK(std::isfinite(large_output[1]));

    std::array<float, 32> compressor_kv {};
    std::array<float, 32> compressor_score {};
    compressor_score.fill(-std::numeric_limits<float>::infinity());
    std::array<float, 2> compressor_output {};
    const ds::F32View compressor_kv_state_view {
        reinterpret_cast<uintptr_t>(compressor_kv.data()), 8, 4, 4, 1};
    const ds::F32View compressor_score_state_view {
        reinterpret_cast<uintptr_t>(compressor_score.data()), 8, 4, 4, 1};
    const ds::F32View compressor_output_view {
        reinterpret_cast<uintptr_t>(compressor_output.data()), 1, 2, 2, 1};
    const ds::CompressorStateView compressor_state {
        compressor_kv_state_view, compressor_score_state_view, 2, 4};
    const std::array<std::array<float, 4>, 4> compressor_projected {{
        {10.0f, 20.0f, 1.0f, 2.0f},
        {30.0f, 40.0f, 3.0f, 4.0f},
        {50.0f, 60.0f, 5.0f, 6.0f},
        {70.0f, 80.0f, 7.0f, 8.0f},
    }};
    std::array<float, 4> compressor_zero_score {};
    const ds::F32View compressor_zero_score_view {
        reinterpret_cast<uintptr_t>(compressor_zero_score.data()), 1, 4, 4,
        1};
    for (uint32_t position = 0; position < 4; ++position) {
        const ds::F32View projected_view {
            reinterpret_cast<uintptr_t>(compressor_projected[position].data()),
            1, 4, 4, 1};
        const bool emitted = ds::compressor_update_pre_scored(
            position, projected_view, compressor_zero_score_view,
            compressor_state, compressor_output_view);
        CHECK(emitted == (position == 3));
    }
    CHECK(std::abs(compressor_output[0] - 4.0f) < 1.0e-6f);
    CHECK(std::abs(compressor_output[1] - 5.0f) < 1.0e-6f);
    for (uint32_t row = 0; row < 4; ++row) {
        for (uint32_t column = 0; column < 4; ++column) {
            CHECK(compressor_kv[row * 4 + column] ==
                  compressor_kv[(row + 4) * 4 + column]);
            CHECK(compressor_score[row * 4 + column] ==
                  compressor_score[(row + 4) * 4 + column]);
        }
    }

    compressor_kv.fill(0.0f);
    compressor_score.fill(-std::numeric_limits<float>::infinity());
    for (uint32_t row = 0; row < 4; ++row) {
        compressor_kv[row * 4] = row < 2 ? 1.0e8f : -1.0e8f;
        compressor_kv[(row + 4) * 4 + 2] = 1.0f;
        compressor_score[row * 4] = 0.0f;
        compressor_score[row * 4 + 1] = 0.0f;
        compressor_score[(row + 4) * 4 + 2] = 0.0f;
        compressor_score[(row + 4) * 4 + 3] = 0.0f;
    }
    ds::compressor_pool(compressor_state, compressor_output_view);
    CHECK(compressor_output[0] == 0.125f);
    CHECK(compressor_output[1] == 0.0f);

    std::array<float, 128> qat_values {};
    qat_values[0] = 1.0f;
    const ds::F32View qat_view {
        reinterpret_cast<uintptr_t>(qat_values.data()), 1, 128, 128, 1};
    ds::indexer_qat(qat_view);
    for (float value : qat_values) {
        CHECK(std::isfinite(value));
        CHECK(value != 0.0f);
    }

    std::array<float, 4> indexer_q {1.0f, 0.0f, 0.0f, 2.0f};
    std::array<float, 6> indexer_kv {1.0f, 0.0f, 0.0f,
                                      1.0f, -1.0f, -1.0f};
    std::array<float, 2> indexer_weights {1.0f, 2.0f};
    const ds::F32View indexer_q_view {
        reinterpret_cast<uintptr_t>(indexer_q.data()), 2, 2, 2, 1};
    const ds::F32View indexer_kv_view {
        reinterpret_cast<uintptr_t>(indexer_kv.data()), 3, 2, 2, 1};
    const ds::F32View indexer_weights_view {
        reinterpret_cast<uintptr_t>(indexer_weights.data()), 1, 2, 2, 1};
    const std::vector<uint32_t> selected = ds::sparse_indexer(
        indexer_q_view, indexer_kv_view, indexer_weights_view, 2);
    CHECK((selected == std::vector<uint32_t> {1, 0}));

    std::vector<float> long_indexer_kv(513 * 2, 0.0f);
    for (uint32_t row = 0; row < 513; ++row) {
        long_indexer_kv[row * 2] = static_cast<float>(row);
    }
    const ds::F32View long_indexer_kv_view {
        reinterpret_cast<uintptr_t>(long_indexer_kv.data()), 513, 2, 2, 1};
    const std::vector<uint32_t> long_selected = ds::sparse_indexer(
        indexer_q_view, long_indexer_kv_view, indexer_weights_view, 512);
    CHECK(long_selected.size() == 512);
    CHECK(long_selected.front() == 512);
    CHECK(long_selected.back() == 1);
    CHECK(ds::operator_counts.sparse_indexer == 2);
    CHECK(ds::operator_counts.sparse_indexer_topk == 2);

    std::array<float, 9> router_logits {
        -20.0f, -10.0f, -5.0f, -1.0f, 0.0f, 1.0f, 5.0f, 10.0f, 20.0f};
    std::array<float, 9> router_probabilities {};
    const ds::F32View router_logits_view {
        reinterpret_cast<uintptr_t>(router_logits.data()), 1, 9, 9, 1};
    const ds::F32View router_probabilities_view {
        reinterpret_cast<uintptr_t>(router_probabilities.data()), 1, 9, 9,
        1};
    const ds::RouterSelection router_selection = ds::router(
        router_logits_view, nullptr, std::span<const uint32_t>(), 2, 2.0f,
        router_probabilities_view);
    CHECK((router_selection.expert_indices ==
           std::vector<uint32_t> {8, 7}));
    CHECK(ds::operator_counts.router_argmax == 2);
    for (size_t index = 0; index < router_logits.size(); ++index) {
        const float expected =
            std::sqrt(std::max(router_logits[index], 0.0f) +
                      std::log1p(std::exp(-std::abs(router_logits[index]))));
        const float error = std::abs(router_probabilities[index] - expected);
        if (error >= 1.0e-6f) {
            std::fprintf(stderr, "router_softplus index=%zu input=%g actual=%g expected=%g error=%g\n",
                         index, router_logits[index], router_probabilities[index], expected, error);
        }
        CHECK(error < 1.0e-6f);
    }
    CHECK(std::abs(router_selection.expert_weights[0] +
                       router_selection.expert_weights[1] -
                   2.0f) < 1.0e-6f);

    std::array<float, 4> tied_logits {0.0f, 0.0f, 0.0f, 0.0f};
    std::array<float, 4> selection_bias {0.0f, 1.0f, 1.0f, -1.0f};
    std::array<float, 4> tied_probabilities {};
    const ds::F32View tied_logits_view {
        reinterpret_cast<uintptr_t>(tied_logits.data()), 1, 4, 4, 1};
    const ds::F32View selection_bias_view {
        reinterpret_cast<uintptr_t>(selection_bias.data()), 1, 4, 4, 1};
    const ds::F32View tied_probabilities_view {
        reinterpret_cast<uintptr_t>(tied_probabilities.data()), 1, 4, 4, 1};
    const ds::RouterSelection tied_selection = ds::router(
        tied_logits_view, &selection_bias_view, std::span<const uint32_t>(),
        2, 1.0f, tied_probabilities_view);
    CHECK((tied_selection.expert_indices == std::vector<uint32_t> {1, 2}));
    CHECK(ds::operator_counts.router_argmax == 4);
    const std::array<uint32_t, 2> fixed_experts {3, 0};
    const ds::RouterSelection fixed_selection = ds::router(
        tied_logits_view, nullptr, fixed_experts, 2, 1.0f,
        tied_probabilities_view);
    CHECK((fixed_selection.expert_indices == std::vector<uint32_t> {3, 0}));
    CHECK(ds::operator_counts.router_argmax == 4);

    std::array<float, 256> full_router_logits {};
    std::array<float, 256> full_router_probabilities {};
    for (uint32_t expert = 0; expert < full_router_logits.size(); ++expert) {
        full_router_logits[expert] = -4.0f + expert / 64.0f;
    }
    const ds::F32View full_router_logits_view {
        reinterpret_cast<uintptr_t>(full_router_logits.data()), 1, 256, 256,
        1};
    const ds::F32View full_router_probabilities_view {
        reinterpret_cast<uintptr_t>(full_router_probabilities.data()), 1,
        256, 256, 1};
    const ds::RouterSelection full_selection = ds::router(
        full_router_logits_view, nullptr, std::span<const uint32_t>(), 8,
        1.0f, full_router_probabilities_view);
    CHECK((full_selection.expert_indices ==
           std::vector<uint32_t> {255, 254, 253, 252, 251, 250, 249, 248}));
    CHECK(ds::operator_counts.router_argmax == 12);

    std::vector<uint8_t> zero_iq2(256 * ds::kIq2XxsBlockBytes, 0);
    std::vector<uint8_t> zero_q2(256 * ds::kQ2KBlockBytes, 0);
    std::vector<uint8_t> zero_q8(256 * 8 * ds::kQ8BlockBytes, 0);
    const ds::WeightTensor zero_iq2_weight {
        .payload = zero_iq2.data(),
        .bytes = zero_iq2.size(),
        .elements = 65536,
        .dimensions = {256, 256, 1},
        .tensor_type = ds::kGgufTypeIq2Xxs,
        .rank = 3,
    };
    const ds::WeightTensor zero_q2_weight {
        .payload = zero_q2.data(),
        .bytes = zero_q2.size(),
        .elements = 65536,
        .dimensions = {256, 256, 1},
        .tensor_type = ds::kGgufTypeQ2_K,
        .rank = 3,
    };
    const ds::WeightTensor zero_q8_weight {
        .payload = zero_q8.data(),
        .bytes = zero_q8.size(),
        .elements = 65536,
        .dimensions = {256, 256},
        .tensor_type = ds::kGgufTypeQ8_0,
        .rank = 2,
    };
    std::array<float, 256> routed_output {};
    std::array<float, 256> shared_output {};
    const ds::F32View routed_output_view {
        reinterpret_cast<uintptr_t>(routed_output.data()), 1, 256, 256, 1};
    const ds::F32View shared_output_view {
        reinterpret_cast<uintptr_t>(shared_output.data()), 1, 256, 256, 1};
    const std::array<uint32_t, 1> one_expert {0};
    const std::array<float, 1> one_weight {1.0f};
    ds::routed_experts(expert_input_view, zero_iq2_weight, zero_iq2_weight,
                       zero_q2_weight, iq2_codebook,
                       std::span<const uint32_t>(one_expert),
                       std::span<const float>(one_weight), 10.0f,
                       routed_output_view);
    ds::shared_expert(expert_input_view, zero_q8_weight, zero_q8_weight,
                      zero_q8_weight, 10.0f, shared_output_view);
    for (uint32_t index = 0; index < 256; ++index) {
        CHECK(routed_output[index] == 0.0f);
        CHECK(shared_output[index] == 0.0f);
    }

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
        "deepseek_v4_flash_pto_ops=pass q8_linear=1 f16_linear=1 "
        "q2_k_linear=1 iq2_xxs_linear=1 rms_norm=1 "
        "head_rms_norm=1 rope_tail=1 kv_write=1 kv_gather=1 "
        "sink_attention=1 compressor_update=1 indexer_qat=1 "
        "sparse_indexer=2 router=1 routed_experts=1 shared_expert=1 "
        "hc_split=1 hc_weighted_sum=1 hc_post=1 swiglu=1 "
        "tload=%llu tstore=%llu\n",
        static_cast<unsigned long long>(ds::operator_counts.tload),
        static_cast<unsigned long long>(ds::operator_counts.tstore));
    return 0;
}
