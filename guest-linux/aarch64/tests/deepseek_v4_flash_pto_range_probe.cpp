#include "deepseek_v4_flash_pto_range.hpp"

#include <charconv>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string_view>
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

static uint64_t state_bytes(uint32_t end) {
    if (!end || end > ds::kLayers) {
        throw std::invalid_argument("invalid_end_layer");
    }
    uint64_t bytes = 32;
    for (uint32_t layer = 0; layer < end; ++layer) {
        const uint32_t ratio = ds::compression_ratio(layer);
        bytes += 8 + 8 + uint64_t(ds::kHeadDim) * sizeof(float);
        if (!ratio) continue;
        bytes += 8;
        if (ratio == 4) bytes += 8;
        const uint64_t lanes = ratio == 4 ? 2 : 1;
        const auto compressor_bytes = [&](uint32_t head_dim) {
            return 32 + uint64_t(ratio) * lanes * head_dim * lanes * 8;
        };
        bytes += compressor_bytes(ds::kHeadDim);
        if (ratio == 4) bytes += compressor_bytes(ds::kIndexerHeadDim);
    }
    return bytes;
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

static int run(int argc, char **argv) {
    if (argc == 3 && std::string_view(argv[1]) == "--state-bytes") {
        std::printf("%llu\n",
                    static_cast<unsigned long long>(state_bytes(parse_u32(argv[2]))));
        return 0;
    }
    if (argc != 6 && argc != 8) {
        throw std::invalid_argument(
            "usage: probe INDEX END_LAYER TOKEN HIDDEN_OUT STATE_OUT "
            "[--attention-out ATTENTION_OUT]");
    }
    const uint32_t end = parse_u32(argv[2]);
    const uint32_t token = parse_u32(argv[3]);
    if (!end || end > ds::kLayers || token >= ds::kVocab ||
        (argc == 8 && (end != 1 || std::string_view(argv[6]) != "--attention-out"))) {
        throw std::invalid_argument("invalid_range_or_token");
    }

    const std::vector<uint8_t> descriptor = read_file(argv[1]);
    const ds::WeightIndex index(descriptor.data(), descriptor.size());
    const ds::MappedWeights weights(index);
    const ds::Iq2Codebook codebook {index.iq2_grid(), index.iq2_signs()};
    const ds::RangeGeometry geometry {
        0, end, ds::kLayers, 0, 1, ds::kHidden, ds::kHcMult,
        ds::kVocab, ds::kAttentionHeads, 1, ds::kHeadDim, ds::kRopeDim,
        ds::kQLora, ds::kOLora, ds::kOutputGroups, ds::kExperts,
        ds::kExpertsUsed, 1, ds::kMoeIntermediate, ds::kSlidingWindow,
        ds::kIndexerHeads, ds::kIndexerHeadDim, ds::kIndexerTopK,
    };
    float unused_input = 0.0f;
    uint8_t unused_state = 0;
    const int32_t token_id = static_cast<int32_t>(token);
    std::vector<float> hidden(ds::kHidden * ds::kHcMult);
    std::vector<uint8_t> next_state(state_bytes(end));
    std::vector<float> logits(end == ds::kLayers ? ds::kVocab : 1);
    ds::decoder_range(
        weights, codebook, geometry,
        ds::F32View {reinterpret_cast<uintptr_t>(&unused_input), 1, 1, 1, 1},
        ds::ByteView {reinterpret_cast<uintptr_t>(&unused_state), 1},
        ds::I32View {reinterpret_cast<uintptr_t>(&token_id), 1},
        ds::F32View {reinterpret_cast<uintptr_t>(hidden.data()), 1,
                     static_cast<uint32_t>(hidden.size()),
                     static_cast<uint32_t>(hidden.size()), 1},
        ds::ByteView {reinterpret_cast<uintptr_t>(next_state.data()),
                      next_state.size()},
        ds::F32View {reinterpret_cast<uintptr_t>(logits.data()), 1,
                     static_cast<uint32_t>(logits.size()),
                     static_cast<uint32_t>(logits.size()), 1});

    write_file(argv[4], hidden.data(), hidden.size() * sizeof(float));
    write_file(argv[5], next_state.data(), next_state.size());
    if (argc == 8) {
        std::vector<uint8_t> attention_state(state_bytes(1));
        const ds::RangeStateGeometry state_geometry {
            0, 1, 1, ds::kHeadDim, ds::kIndexerHeadDim,
            ds::kSlidingWindow, ds::kIndexerTopK,
        };
        ds::RangeStateView stage = ds::range_state(
            ds::ByteView {reinterpret_cast<uintptr_t>(attention_state.data()),
                          attention_state.size()},
            state_geometry, true);
        ds::initialize_compressors(stage);
        ds::LocalMatrix embedded(ds::kHcMult, ds::kHidden);
        ds::embedding(weights, token, embedded.view());
        ds::LocalMatrix attention = ds::attention_layer(
            weights, 0, 0, embedded.view(), stage.layers[0]);
        ds::LocalMatrix final = ds::ffn_layer(
            weights, codebook, 0, token, attention.view());
        if (std::memcmp(reinterpret_cast<const void *>(final.view().address),
                        hidden.data(), hidden.size() * sizeof(float)) != 0) {
            throw std::runtime_error("stage_replay_mismatch");
        }
        write_file(argv[7], reinterpret_cast<const void *>(attention.view().address),
                   hidden.size() * sizeof(float));
    }
    std::printf("direct_range=pass layers=0:%u token=%u hidden_values=%zu state_bytes=%zu\n",
                end, token, hidden.size(), next_state.size());
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
