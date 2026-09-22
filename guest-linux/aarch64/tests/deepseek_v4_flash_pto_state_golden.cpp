#include "deepseek_v4_flash_pto_state.hpp"

#include <cmath>
#include <cstdio>
#include <vector>

namespace ds = lingqu::deepseek_v4_flash_pto;

#define CHECK(condition) do { if (!(condition)) return __LINE__; } while (0)

int main() {
    constexpr uint64_t previous_bytes = 618680;
    constexpr uint64_t next_bytes = 625336;
    std::vector<uint8_t> previous_storage(previous_bytes);
    std::vector<uint8_t> next_storage(next_bytes);
    const ds::RangeStateGeometry previous_geometry {
        2, 4, 3, 512, 128, 128, 512};
    const ds::RangeStateGeometry next_geometry {
        2, 4, 4, 512, 128, 128, 512};
    const ds::ByteView previous_bytes_view {
        reinterpret_cast<uintptr_t>(previous_storage.data()),
        previous_storage.size()};
    const ds::ByteView next_bytes_view {
        reinterpret_cast<uintptr_t>(next_storage.data()), next_storage.size()};

    ds::operator_counts = {};
    ds::RangeStateView previous =
        ds::range_state(previous_bytes_view, previous_geometry, true);
    CHECK(previous.layers.size() == 2);
    CHECK(previous.layers[0].ratio == 4);
    CHECK(previous.layers[0].raw.rows == 3);
    CHECK(!previous.layers[0].compressed);
    CHECK(previous.layers[0].attention_compressor.has_value());
    CHECK(previous.layers[0].indexer_compressor.has_value());
    CHECK(previous.layers[1].ratio == 128);
    CHECK(previous.layers[1].attention_compressor.has_value());
    CHECK(!previous.layers[1].indexer_compressor);
    ds::fill(previous.layers[0].raw, 3.0f);
    ds::fill(previous.layers[1].raw, 5.0f);
    ds::fill(previous.layers[0].attention_compressor->kv, 7.0f);
    ds::fill(previous.layers[0].attention_compressor->score, -2.0f);

    ds::RangeStateView parsed =
        ds::range_state(previous_bytes_view, previous_geometry, false);
    CHECK(parsed.layers.size() == previous.layers.size());
    ds::RangeStateView next =
        ds::range_state(next_bytes_view, next_geometry, true);
    CHECK(next.layers[0].compressed.has_value());
    CHECK(next.layers[0].compressed->rows == 1);
    CHECK(!next.layers[1].compressed);
    ds::copy_range_state(parsed, next);

    const float *next_ratio4_raw =
        reinterpret_cast<const float *>(next.layers[0].raw.address);
    const float *next_ratio128_raw =
        reinterpret_cast<const float *>(next.layers[1].raw.address);
    for (uint32_t row = 0; row < 3; ++row) {
        CHECK(next_ratio4_raw[row * 512] == 3.0f);
        CHECK(next_ratio128_raw[row * 512] == 5.0f);
    }
    CHECK(next_ratio4_raw[3 * 512] == 0.0f);
    CHECK(next_ratio128_raw[3 * 512] == 0.0f);
    const float *next_compressor = reinterpret_cast<const float *>(
        next.layers[0].attention_compressor->kv.address);
    const float *next_score = reinterpret_cast<const float *>(
        next.layers[0].attention_compressor->score.address);
    CHECK(next_compressor[0] == 7.0f);
    CHECK(next_score[0] == -2.0f);

    bool rejected_magic = false;
    previous_storage[0] ^= 1;
    try {
        (void)ds::range_state(previous_bytes_view, previous_geometry, false);
    } catch (const std::invalid_argument &) {
        rejected_magic = true;
    }
    CHECK(rejected_magic);
    previous_storage[0] ^= 1;

    bool rejected_truncated = false;
    try {
        const ds::ByteView truncated {previous_bytes_view.address,
                                      previous_bytes_view.bytes - 1};
        (void)ds::range_state(truncated, previous_geometry, false);
    } catch (const std::out_of_range &) {
        rejected_truncated = true;
    }
    CHECK(rejected_truncated);
    CHECK(ds::operator_counts.tload > 0);
    CHECK(ds::operator_counts.tstore > 0);
    std::printf(
        "deepseek_v4_flash_pto_state=pass previous_bytes=%llu "
        "next_bytes=%llu layers=2 tload=%llu tstore=%llu\n",
        static_cast<unsigned long long>(previous_bytes),
        static_cast<unsigned long long>(next_bytes),
        static_cast<unsigned long long>(ds::operator_counts.tload),
        static_cast<unsigned long long>(ds::operator_counts.tstore));
    return 0;
}
