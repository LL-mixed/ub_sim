#include "deepseek_v4_flash_pto_ops.hpp"

#include <array>
#include <cmath>
#include <cstdio>
#include <limits>
#include <vector>

namespace ds = lingqu::deepseek_v4_flash_pto;

#define CHECK(condition) do { \
    if (!(condition)) { \
        std::fprintf(stderr, "fp8 check failed at line %d: %s\n", \
                     __LINE__, #condition); \
        return 1; \
    } \
} while (0)

static float level(uint32_t index) {
    const uint32_t exponent = index / 8;
    const uint32_t mantissa = index % 8;
    if (exponent == 0) return float(mantissa) * 0.001953125f;
    return std::ldexp(1.0f + float(mantissa) * 0.125f,
                      int(exponent) - 7);
}

static float scalar_roundtrip(float value, float scale) {
    const float magnitude = std::min(std::abs(value / scale), 448.0f);
    uint32_t best = 0;
    float best_distance = std::numeric_limits<float>::infinity();
    for (uint32_t candidate = 0; candidate <= 126; ++candidate) {
        const float distance = std::abs(magnitude - level(candidate));
        if (distance < best_distance ||
            (distance == best_distance && candidate % 2 == 0 &&
             best % 2 != 0)) {
            best = candidate;
            best_distance = distance;
        }
    }
    return (value < 0.0f ? -1.0f : 1.0f) * level(best) * scale;
}

int main() {
    std::vector<float> samples {0.0f, -0.0f, 500.0f, -500.0f};
    for (uint32_t index = 0; index < 126; ++index) {
        const float midpoint = 0.5f * (level(index) + level(index + 1));
        samples.push_back(std::nextafter(midpoint, 0.0f));
        samples.push_back(midpoint);
        samples.push_back(std::nextafter(
            midpoint, std::numeric_limits<float>::infinity()));
        samples.push_back(-midpoint);
    }
    while (samples.size() % 64) samples.push_back(0.0f);

    std::array<float, 64> input {};
    std::array<float, 64> output {};
    const ds::F32View input_view {
        reinterpret_cast<uintptr_t>(input.data()), 1, 64, 64, 1};
    const ds::F32View output_view {
        reinterpret_cast<uintptr_t>(output.data()), 1, 64, 64, 1};
    for (float scale : {0.125f, 1.0f, 8.0f}) {
        for (size_t offset = 0; offset < samples.size(); offset += 64) {
            for (size_t lane = 0; lane < 64; ++lane) {
                input[lane] = samples[offset + lane] * scale;
            }
            ds::F32Row block(64);
            ds::load(block, input_view, 0);
            ds::e4m3fn_roundtrip(block, scale);
            ds::store(output_view, 0, block);
            for (size_t lane = 0; lane < 64; ++lane) {
                if (output[lane] != scalar_roundtrip(input[lane], scale)) {
                    std::fprintf(stderr,
                                 "fp8 mismatch scale=%g sample=%zu input=%g "
                                 "actual=%g expected=%g\n", scale,
                                 offset + lane, input[lane], output[lane],
                                 scalar_roundtrip(input[lane], scale));
                    return 1;
                }
            }
        }
    }

    std::array<float, 256> kv {};
    std::array<float, 256> original {};
    for (uint32_t row = 0; row < 2; ++row) {
        for (uint32_t column = 0; column < 128; ++column) {
            kv[row * 128 + column] = column < 64
                ? float(int(column) - 31) * (row ? 0.125f : 0.0078125f)
                : float(column) + float(row);
        }
    }
    original = kv;
    const ds::F32View kv_view {
        reinterpret_cast<uintptr_t>(kv.data()), 2, 128, 128, 1};
    ds::fp8_kv_roundtrip(kv_view, 64);
    for (uint32_t row = 0; row < 2; ++row) {
        float maximum = 1.0e-4f;
        for (uint32_t column = 0; column < 64; ++column) {
            maximum = std::max(maximum,
                               std::abs(original[row * 128 + column]));
        }
        const float scale =
            std::exp2(std::ceil(std::log2(maximum / 448.0f)));
        for (uint32_t column = 0; column < 128; ++column) {
            const uint32_t offset = row * 128 + column;
            const float expected = column < 64
                ? scalar_roundtrip(original[offset], scale)
                : original[offset];
            CHECK(kv[offset] == expected);
        }
    }

    ds::F32Row invalid_shape(32);
    bool rejected = false;
    try {
        ds::e4m3fn_roundtrip(invalid_shape, 1.0f);
    } catch (const std::invalid_argument &) {
        rejected = true;
    }
    CHECK(rejected);
    std::printf("deepseek_v4_flash_fp8_pto=pass\n");
    return 0;
}
