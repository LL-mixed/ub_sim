#include "deepseek_v4_flash_pto_ops.hpp"

#include <array>
#include <cmath>
#include <cstdio>
#include <limits>

namespace ds = lingqu::deepseek_v4_flash_pto;

#define CHECK(condition) do { \
    if (!(condition)) { \
        std::fprintf(stderr, "fp4 check failed at line %d: %s\n", \
                     __LINE__, #condition); \
        return 1; \
    } \
} while (0)

static float scalar_roundtrip(float value, float scale) {
    constexpr std::array<float, 8> levels {
        0.0f, 0.5f, 1.0f, 1.5f, 2.0f, 3.0f, 4.0f, 6.0f};
    const float magnitude = std::min(std::abs(value / scale), 6.0f);
    uint32_t best = 0;
    for (uint32_t candidate = 1; candidate < levels.size(); ++candidate) {
        const float difference = std::abs(magnitude - levels[candidate]);
        const float best_difference = std::abs(magnitude - levels[best]);
        if (difference < best_difference ||
            (difference == best_difference && candidate % 2 == 0 &&
             best % 2 != 0)) {
            best = candidate;
        }
    }
    return (value < 0.0f ? -1.0f : 1.0f) * levels[best] * scale;
}

int main() {
    constexpr std::array<float, 7> midpoints {
        0.25f, 0.75f, 1.25f, 1.75f, 2.5f, 3.5f, 5.0f};
    std::array<float, 32> input {};
    std::array<float, 32> output {};
    const ds::F32View input_view {
        reinterpret_cast<uintptr_t>(input.data()), 1, 32, 32, 1};
    const ds::F32View output_view {
        reinterpret_cast<uintptr_t>(output.data()), 1, 32, 32, 1};
    for (float scale : {0.125f, 1.0f, 8.0f}) {
        size_t lane = 0;
        for (float midpoint : midpoints) {
            input[lane++] = std::nextafter(midpoint, 0.0f) * scale;
            input[lane++] = midpoint * scale;
            input[lane++] = std::nextafter(midpoint,
                                           std::numeric_limits<float>::infinity()) * scale;
            input[lane++] = -midpoint * scale;
        }
        input[lane++] = 0.0f;
        input[lane++] = -0.0f;
        input[lane++] = 6.5f * scale;
        input[lane++] = -20.0f * scale;
        ds::F32Row block(32);
        ds::load(block, input_view, 0);
        ds::fp4_e2m1_roundtrip(block, scale);
        ds::store(output_view, 0, block);
        for (size_t index = 0; index < input.size(); ++index) {
            CHECK(output[index] == scalar_roundtrip(input[index], scale));
        }
    }
    ds::F32Row invalid_shape(16);
    bool rejected = false;
    try {
        ds::fp4_e2m1_roundtrip(invalid_shape, 1.0f);
    } catch (const std::invalid_argument &) {
        rejected = true;
    }
    CHECK(rejected);
    std::printf("deepseek_v4_flash_fp4_pto=pass\n");
    return 0;
}
