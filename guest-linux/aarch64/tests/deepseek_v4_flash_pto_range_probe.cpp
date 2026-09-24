#include "deepseek_v4_flash_pto_range.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ds = lingqu::deepseek_v4_flash_pto;

static uint32_t parse_u32(std::string_view value) {
    uint32_t result = 0;
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
    if (parsed.ec != std::errc {} || parsed.ptr != value.data() + value.size()) {
        throw std::invalid_argument("invalid_unsigned_integer");
    }
    return result;
}

static uint64_t state_bytes(uint32_t end, uint32_t positions = 1) {
    if (!end || end > ds::kLayers || !positions) {
        throw std::invalid_argument("invalid_state_geometry");
    }
    uint64_t bytes = 32;
    for (uint32_t layer = 0; layer < end; ++layer) {
        const uint32_t ratio = ds::compression_ratio(layer);
        bytes += 8 + 8 + uint64_t(std::min(positions, ds::kSlidingWindow)) *
                             ds::kHeadDim * sizeof(float);
        if (!ratio) continue;
        const uint32_t compressed_rows =
            std::min(positions / ratio, ds::kIndexerTopK);
        bytes += 8 + uint64_t(compressed_rows) * ds::kHeadDim * sizeof(float);
        if (ratio == 4) {
            bytes += 8 + uint64_t(compressed_rows) * ds::kIndexerHeadDim *
                             sizeof(float);
        }
        const uint64_t lanes = ratio == 4 ? 2 : 1;
        const auto compressor_bytes = [&](uint32_t head_dim) {
            return 32 + uint64_t(ratio) * lanes * head_dim * lanes * 8;
        };
        bytes += compressor_bytes(ds::kHeadDim);
        if (ratio == 4) bytes += compressor_bytes(ds::kIndexerHeadDim);
    }
    return bytes;
}

static std::vector<int32_t> parse_tokens(std::string_view value) {
    std::vector<int32_t> tokens;
    while (!value.empty()) {
        const size_t separator = value.find(',');
        const std::string_view field = value.substr(0, separator);
        const uint32_t token = parse_u32(field);
        if (token >= ds::kVocab || tokens.size() >= 32) {
            throw std::invalid_argument("invalid_token_sequence");
        }
        tokens.push_back(static_cast<int32_t>(token));
        if (separator == std::string_view::npos) break;
        value.remove_prefix(separator + 1);
        if (value.empty()) {
            throw std::invalid_argument("invalid_token_sequence");
        }
    }
    if (tokens.empty()) throw std::invalid_argument("invalid_token_sequence");
    return tokens;
}

static std::vector<uint8_t> read_file(const char *path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("weight_index_read_failed");
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(input), {});
}

static void write_file(const char *path, const void *data, uint64_t bytes) {
    std::ofstream output(path, std::ios::binary);
    if (!output) throw std::runtime_error("output_open_failed");
    output.write(static_cast<const char *>(data),
                 static_cast<std::streamsize>(bytes));
    if (!output) throw std::runtime_error("output_write_failed");
}

static ds::LocalMatrix attention_layer_0_trace(const ds::MappedWeights &weights,
                                                const ds::F32View &residual_hc,
                                                const char *raw_output_path) {
    ds::HcPrepared prepared = ds::prepare_hc(
        weights, 0, "hc_attn_fn", "hc_attn_scale", "hc_attn_base",
        "attn_norm", residual_hc);
    ds::LocalMatrix q_lora(1, ds::kQLora);
    ds::q8_0_linear(prepared.normalized.view(),
                    ds::layer_tensor(weights, 0, "attn_q_a"), q_lora.view());
    const ds::F32View q_lora_norm = ds::f32_weight_view(
        ds::layer_tensor(weights, 0, "attn_q_a_norm"), ds::kQLora);
    ds::rms_norm(q_lora.view(), &q_lora_norm, q_lora.view(), ds::kEpsilon);

    ds::LocalMatrix q_flat(1, ds::kAttentionHeads * ds::kHeadDim);
    ds::q8_0_linear(q_lora.view(), ds::layer_tensor(weights, 0, "attn_q_b"),
                    q_flat.view());
    const ds::F32View q {q_flat.view().address, ds::kAttentionHeads,
                         ds::kHeadDim, ds::kHeadDim, 1};
    ds::head_rms_norm(q, nullptr, q, ds::kEpsilon);
    ds::RopeFactors rope = ds::rope_factors(0, 0);
    ds::rope_tail(q, ds::kRopeDim, rope.cos_view(), rope.sin_view(), false, q);
    const std::string q_path = std::string(raw_output_path) + ".q.f32";
    write_file(q_path.c_str(), reinterpret_cast<const void *>(q.address),
               ds::kAttentionHeads * ds::kHeadDim * sizeof(float));

    ds::LocalMatrix current_kv(1, ds::kHeadDim);
    ds::q8_0_linear(prepared.normalized.view(),
                    ds::layer_tensor(weights, 0, "attn_kv"), current_kv.view());
    const ds::F32View kv_norm = ds::f32_weight_view(
        ds::layer_tensor(weights, 0, "attn_kv_a_norm"), ds::kHeadDim);
    ds::rms_norm(current_kv.view(), &kv_norm, current_kv.view(), ds::kEpsilon);
    ds::rope_tail(current_kv.view(), ds::kRopeDim, rope.cos_view(),
                  rope.sin_view(), false, current_kv.view());
    ds::fp8_kv_roundtrip(current_kv.view(), ds::kRopeDim);
    ds::f16_roundtrip(current_kv.view());

    ds::LocalMatrix heads(ds::kAttentionHeads, ds::kHeadDim);
    const ds::F32View sinks = ds::f32_weight_view(
        ds::layer_tensor(weights, 0, "attn_sinks"), ds::kAttentionHeads);
    ds::sink_attention(q, current_kv.view(), sinks, heads.view());
    ds::rope_tail(heads.view(), ds::kRopeDim, rope.cos_view(),
                  rope.sin_view(), true, heads.view());
    const std::string heads_path = std::string(raw_output_path) + ".heads.f32";
    write_file(heads_path.c_str(),
               reinterpret_cast<const void *>(heads.view().address),
               ds::kAttentionHeads * ds::kHeadDim * sizeof(float));

    ds::LocalMatrix output_a(1, ds::kOutputGroups * ds::kOLora);
    const ds::F32View flat_heads {heads.view().address, 1,
                                  ds::kAttentionHeads * ds::kHeadDim,
                                  ds::kAttentionHeads * ds::kHeadDim, 1};
    ds::grouped_q8_0_linear(flat_heads,
                            ds::layer_tensor(weights, 0, "attn_output_a"),
                            ds::kOutputGroups, output_a.view());
    const std::string output_a_path = std::string(raw_output_path) +
                                      ".output-a.f32";
    write_file(output_a_path.c_str(),
               reinterpret_cast<const void *>(output_a.view().address),
               ds::kOutputGroups * ds::kOLora * sizeof(float));
    ds::LocalMatrix attention_output(1, ds::kHidden);
    ds::q8_0_linear(output_a.view(),
                    ds::layer_tensor(weights, 0, "attn_output_b"),
                    attention_output.view());
    write_file(raw_output_path,
               reinterpret_cast<const void *>(attention_output.view().address),
               ds::kHidden * sizeof(float));
    ds::LocalMatrix output_hc(ds::kHcMult, ds::kHidden);
    ds::hc_post(attention_output.view(), residual_hc, prepared.post.view(),
                prepared.combine.view(), output_hc.view());
    return output_hc;
}

static int sink_attention_fixture() {
    std::vector<float> query(ds::kHeadDim, 0.0f);
    std::vector<float> key(ds::kHeadDim);
    std::vector<float> result(ds::kHeadDim, 0.0f);
    const float sink = 0.25f;
    for (uint32_t column = 0; column < ds::kHeadDim; ++column) {
        key[column] = 0.12345f + float(column) * 0.001f;
    }
    const ds::F32View q {reinterpret_cast<uintptr_t>(query.data()), 1,
                         ds::kHeadDim, ds::kHeadDim, 1};
    const ds::F32View kv {reinterpret_cast<uintptr_t>(key.data()), 1,
                          ds::kHeadDim, ds::kHeadDim, 1};
    const ds::F32View sinks {reinterpret_cast<uintptr_t>(&sink), 1, 1, 1, 1};
    const ds::F32View output {reinterpret_cast<uintptr_t>(result.data()), 1,
                              ds::kHeadDim, ds::kHeadDim, 1};
    ds::sink_attention(q, kv, sinks, output);
    const float weight = std::exp(-sink);
    const float denominator = 1.0f + weight;
    const float inverse_denominator = 1.0f / denominator;
    bool distinguishes_direct_division = false;
    for (uint32_t column = 0; column < ds::kHeadDim; ++column) {
        const float weighted = key[column] * weight;
        const float expected = weighted * inverse_denominator;
        const float directly_divided = weighted / denominator;
        distinguishes_direct_division |=
            std::memcmp(&directly_divided, &expected, sizeof(float)) != 0;
        if (std::memcmp(&result[column], &expected, sizeof(float)) != 0) {
            std::fprintf(stderr, "sink_attention_mismatch column=%u\n", column);
            return 1;
        }
    }
    if (!distinguishes_direct_division) {
        std::fputs("sink_attention_fixture_has_no_rounding_difference\n", stderr);
        return 1;
    }
    std::puts("sink_attention_fixture=pass");
    return 0;
}

static int run(int argc, char **argv) {
    if (argc == 2 && std::string_view(argv[1]) == "--selection-bias-name") {
        std::puts(ds::selection_bias_tensor_name(3).c_str());
        return 0;
    }
    if (argc == 2 && std::string_view(argv[1]) == "--sink-attention-fixture") {
        return sink_attention_fixture();
    }
    if ((argc == 3 || argc == 4) &&
        std::string_view(argv[1]) == "--state-bytes") {
        std::printf("%llu\n",
                    static_cast<unsigned long long>(state_bytes(
                        parse_u32(argv[2]), argc == 4 ? parse_u32(argv[3]) : 1)));
        return 0;
    }
    if (argc != 6 && argc != 8) {
        throw std::invalid_argument(
            "usage: probe INDEX END_LAYER TOKEN[,TOKEN...] HIDDEN_OUT STATE_OUT "
            "[--attention-out ATTENTION_OUT | --logits-out LOGITS_OUT]");
    }
    const uint32_t end = parse_u32(argv[2]);
    const std::vector<int32_t> tokens = parse_tokens(argv[3]);
    const bool attention_out = argc == 8 &&
        std::string_view(argv[6]) == "--attention-out";
    const bool logits_out = argc == 8 &&
        std::string_view(argv[6]) == "--logits-out";
    if (!end || end > ds::kLayers ||
        (argc == 8 && !attention_out && !logits_out) ||
        (attention_out && (end != 1 || tokens.size() != 1)) ||
        (logits_out && end != ds::kLayers)) {
        throw std::invalid_argument("invalid_range_or_token");
    }

    const std::vector<uint8_t> descriptor = read_file(argv[1]);
    const ds::WeightIndex index(descriptor.data(), descriptor.size());
    const ds::MappedWeights weights(index);
    const ds::Iq2Codebook codebook {index.iq2_grid(), index.iq2_signs()};
    float unused_input = 0.0f;
    uint8_t unused_state = 0;
    std::vector<float> hidden(ds::kHidden * ds::kHcMult);
    std::vector<uint8_t> previous_state;
    std::vector<float> logits(end == ds::kLayers ? ds::kVocab : 1);
    for (size_t position = 0; position < tokens.size(); ++position) {
        const ds::RangeGeometry geometry {
            0, end, ds::kLayers, static_cast<uint32_t>(position), 1,
            ds::kHidden, ds::kHcMult, ds::kVocab, ds::kAttentionHeads, 1,
            ds::kHeadDim, ds::kRopeDim, ds::kQLora, ds::kOLora,
            ds::kOutputGroups, ds::kExperts, ds::kExpertsUsed, 1,
            ds::kMoeIntermediate, ds::kSlidingWindow, ds::kIndexerHeads,
            ds::kIndexerHeadDim, ds::kIndexerTopK,
        };
        std::vector<uint8_t> next_state(state_bytes(end, position + 1));
        ds::decoder_range(
            weights, codebook, geometry,
            ds::F32View {reinterpret_cast<uintptr_t>(&unused_input), 1, 1, 1, 1},
            position ? ds::ByteView {reinterpret_cast<uintptr_t>(previous_state.data()),
                                     previous_state.size()}
                     : ds::ByteView {reinterpret_cast<uintptr_t>(&unused_state), 1},
            ds::I32View {reinterpret_cast<uintptr_t>(&tokens[position]), 1},
            ds::F32View {reinterpret_cast<uintptr_t>(hidden.data()), 1,
                         static_cast<uint32_t>(hidden.size()),
                         static_cast<uint32_t>(hidden.size()), 1},
            ds::ByteView {reinterpret_cast<uintptr_t>(next_state.data()),
                          next_state.size()},
            ds::F32View {reinterpret_cast<uintptr_t>(logits.data()), 1,
                         static_cast<uint32_t>(logits.size()),
                         static_cast<uint32_t>(logits.size()), 1});
        previous_state = std::move(next_state);
    }

    write_file(argv[4], hidden.data(), hidden.size() * sizeof(float));
    write_file(argv[5], previous_state.data(), previous_state.size());
    if (attention_out) {
        ds::LocalMatrix embedded(ds::kHcMult, ds::kHidden);
        ds::embedding(weights, static_cast<uint32_t>(tokens.front()),
                      embedded.view());
        const std::string raw_path = std::string(argv[7]) + ".raw.f32";
        ds::LocalMatrix attention = attention_layer_0_trace(
            weights, embedded.view(), raw_path.c_str());
        ds::LocalMatrix final = ds::ffn_layer(
            weights, codebook, 0, static_cast<uint32_t>(tokens.front()),
            attention.view());
        if (std::memcmp(reinterpret_cast<const void *>(final.view().address),
                        hidden.data(), hidden.size() * sizeof(float)) != 0) {
            throw std::runtime_error("stage_replay_mismatch");
        }
        write_file(argv[7], reinterpret_cast<const void *>(attention.view().address),
                   hidden.size() * sizeof(float));
    }
    if (logits_out) {
        write_file(argv[7], logits.data(), logits.size() * sizeof(float));
        const auto top = std::max_element(logits.begin(), logits.end());
        std::printf("direct_terminal_top_token=%zu logit=%g\n",
                    static_cast<size_t>(top - logits.begin()), *top);
    }
    std::printf("direct_range=pass layers=0:%u tokens=%s hidden_values=%zu state_bytes=%zu\n",
                end, argv[3], hidden.size(), previous_state.size());
    return 0;
}

int main(int argc, char **argv) {
    try {
        return run(argc, argv);
    } catch (const std::exception &error) {
        std::fprintf(stderr, "direct_range=fail reason=%s\n", error.what());
        return 1;
    }
}
