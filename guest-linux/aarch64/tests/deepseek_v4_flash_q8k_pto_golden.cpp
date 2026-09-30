#include "deepseek_v4_flash_pto_ops.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <limits>

namespace ds = lingqu::deepseek_v4_flash_pto;

#define CHECK(condition) do { \
    if (!(condition)) { \
        std::fprintf(stderr, "q8k check failed at line %d: %s\n", \
                     __LINE__, #condition); \
        return 1; \
    } \
} while (0)

int main() {
    std::array<float, ds::kQ8KBlockElements> input {};
    const ds::F32View view {
        reinterpret_cast<uintptr_t>(input.data()), 1,
        ds::kQ8KBlockElements, ds::kQ8KBlockElements, 1};
    auto block = ds::quantize_q8_k(view, 0, 0);
    CHECK(block.scale == 0.0f);
    CHECK(std::all_of(block.values.begin(), block.values.end(),
                      [](int8_t value) { return value == 0; }));
    CHECK(std::all_of(block.sums.begin(), block.sums.end(),
                      [](int16_t value) { return value == 0; }));

    input.fill(1.0f);
    block = ds::quantize_q8_k(view, 0, 0);
    CHECK(block.scale == -1.0f / 127.0f);
    CHECK(std::all_of(block.values.begin(), block.values.end(),
                      [](int8_t value) { return value == -127; }));
    CHECK(std::all_of(block.sums.begin(), block.sums.end(),
                      [](int16_t value) { return value == -2032; }));

    input.fill(0.0f);
    input[0] = 1.0f;
    input[1] = -1.0f;
    block = ds::quantize_q8_k(view, 0, 0);
    CHECK(block.scale == -1.0f / 127.0f);
    CHECK(block.values[0] == -127 && block.values[1] == 127);
    CHECK(block.sums[0] == 0);
    input[0] = -1.0f;
    input[1] = 1.0f;
    block = ds::quantize_q8_k(view, 0, 0);
    CHECK(block.scale == 1.0f / 127.0f);
    CHECK(block.values[0] == -127 && block.values[1] == 127);
    CHECK(block.sums[0] == 0);

    for (size_t lane = 0; lane < input.size(); ++lane) {
        input[lane] = float(int(lane % 31) - 15) / 15.0f;
    }
    block = ds::quantize_q8_k(view, 0, 0);
    for (size_t group = 0; group < block.sums.size(); ++group) {
        int32_t expected_sum = 0;
        for (size_t lane = 0; lane < 16; ++lane) {
            expected_sum += block.values[group * 16 + lane];
        }
        CHECK(block.sums[group] == expected_sum);
    }

    for (float invalid : {std::numeric_limits<float>::quiet_NaN(),
                          std::numeric_limits<float>::infinity(),
                          -std::numeric_limits<float>::infinity()}) {
        for (size_t lane : {size_t(0), size_t(31), size_t(127), size_t(255)}) {
            const float original = input[lane];
            input[lane] = invalid;
            bool rejected = false;
            try {
                (void)ds::quantize_q8_k(view, 0, 0);
            } catch (const std::invalid_argument &error) {
                rejected = std::strcmp(error.what(),
                                       "deepseek_pto_q8_k_non_finite") == 0;
            }
            CHECK(rejected);
            input[lane] = original;
        }
    }

    std::printf("deepseek_v4_flash_q8k_pto=pass\n");
    return 0;
}
