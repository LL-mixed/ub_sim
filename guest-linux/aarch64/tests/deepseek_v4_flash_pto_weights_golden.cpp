#include "deepseek_v4_flash_pto_weights.hpp"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <unistd.h>
#include <vector>

namespace ds = lingqu::deepseek_v4_flash_pto;

#define CHECK(condition) do { if (!(condition)) return __LINE__; } while (0)

static void append_u32(std::vector<uint8_t> &output, uint32_t value) {
    for (unsigned shift = 0; shift < 32; shift += 8) output.push_back(value >> shift);
}

static void append_u64(std::vector<uint8_t> &output, uint64_t value) {
    for (unsigned shift = 0; shift < 64; shift += 8) output.push_back(value >> shift);
}

static std::vector<uint8_t> descriptor(
    const std::string &path, uint64_t file_bytes, uint64_t tensor_offset) {
    constexpr char name[] = "blk.0.attn_q_a.weight";
    std::vector<uint8_t> output;
    append_u64(output, ds::kWeightIndexMagic);
    append_u32(output, ds::kWeightIndexVersion);
    append_u32(output, ds::kWeightIndexHeaderBytes);
    append_u64(output, file_bytes);
    append_u64(output, 0x123456789abcdef0ULL);
    append_u32(output, 1);
    append_u32(output, ds::kWeightIndexRecordBytes);
    append_u64(output, ds::kWeightIndexHeaderBytes);
    append_u64(output, ds::kWeightIndexHeaderBytes + ds::kWeightIndexRecordBytes);
    append_u32(output, sizeof(name) - 1);
    append_u32(output, path.size());
    append_u64(output, ds::fnv1a(name));
    append_u64(output, tensor_offset);
    append_u64(output, 16);
    append_u64(output, 8);
    append_u64(output, 4);
    append_u64(output, 2);
    for (int index = 2; index < 8; ++index) append_u64(output, 0);
    append_u32(output, 0);
    append_u32(output, sizeof(name) - 1);
    append_u32(output, 1);
    append_u32(output, 2);
    output.insert(output.end(), name, name + sizeof(name) - 1);
    output.insert(output.end(), path.begin(), path.end());
    for (uint32_t index = 0; index < ds::kIq2GridEntries; ++index) {
        append_u64(output, 0x0808080808080808ULL + index);
    }
    for (uint32_t index = 0; index < ds::kIq2SignEntries; ++index) {
        output.push_back(static_cast<uint8_t>(index));
    }
    return output;
}

int main() {
    std::array<char, 128> path {};
    std::snprintf(path.data(), path.size(), "/tmp/deepseek-pto-weight-%d-XXXXXX", getpid());
    const int fd = mkstemp(path.data());
    CHECK(fd >= 0);
    std::array<uint8_t, 256> model {};
    for (size_t index = 0; index < model.size(); ++index) model[index] = uint8_t(index);
    CHECK(write(fd, model.data(), model.size()) == ssize_t(model.size()));
    CHECK(close(fd) == 0);

    const auto encoded = descriptor(path.data(), model.size(), 96);
    try {
        ds::WeightIndex index(encoded.data(), encoded.size());
        CHECK(index.tensor_count() == 1);
        CHECK(index.model_bytes() == model.size());
        CHECK(index.catalog_fingerprint() == 0x123456789abcdef0ULL);
        CHECK(index.iq2_grid().front() == 0x0808080808080808ULL);
        CHECK(index.iq2_grid().back() ==
              0x0808080808080808ULL + ds::kIq2GridEntries - 1);
        CHECK(index.iq2_signs().front() == 0);
        CHECK(index.iq2_signs().back() == ds::kIq2SignEntries - 1);
        ds::MappedWeights weights(index);
        const ds::WeightTensor tensor = weights.tensor("blk.0.attn_q_a.weight");
        CHECK(tensor.bytes == 16);
        CHECK(tensor.elements == 8);
        CHECK(tensor.rank == 2);
        CHECK(tensor.dimensions[0] == 4 && tensor.dimensions[1] == 2);
        CHECK(tensor.tensor_type == 1);
        CHECK(std::memcmp(tensor.payload, model.data() + 96, 16) == 0);
        bool missing = false;
        try {
            (void)weights.tensor("blk.0.attn_q_b.weight");
        } catch (const std::invalid_argument &) {
            missing = true;
        }
        CHECK(missing);
    } catch (...) {
        unlink(path.data());
        throw;
    }

    auto stale = descriptor(path.data(), model.size() + 1, 96);
    bool rejected = false;
    try {
        ds::WeightIndex index(stale.data(), stale.size());
        ds::MappedWeights weights(index);
    } catch (const std::runtime_error &) {
        rejected = true;
    }
    CHECK(rejected);

    auto out_of_bounds = descriptor(path.data(), model.size(), model.size() - 8);
    rejected = false;
    try {
        ds::WeightIndex index(out_of_bounds.data(), out_of_bounds.size());
    } catch (const std::invalid_argument &) {
        rejected = true;
    }
    CHECK(rejected);
    CHECK(unlink(path.data()) == 0);
    std::puts("deepseek_v4_flash_pto_weights=pass cases=4 payload_copy_bytes=0");
    return 0;
}
