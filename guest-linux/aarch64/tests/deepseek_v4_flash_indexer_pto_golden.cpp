#include "deepseek_v4_flash_pto_ops.hpp"

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <numeric>

namespace ds = lingqu::deepseek_v4_flash_pto;

static ds::F32View view(std::vector<float> &values) {
    const uint32_t count = static_cast<uint32_t>(values.size());
    return {reinterpret_cast<uintptr_t>(values.data()), 1, count, count, 1};
}

static void check(std::vector<float> values, uint32_t count) {
    std::vector<uint32_t> expected(values.size());
    std::iota(expected.begin(), expected.end(), 0);
    std::stable_sort(expected.begin(), expected.end(),
        [&](uint32_t left, uint32_t right) {
            return values[left] > values[right];
        });
    expected.resize(count);
    ds::operator_counts = {};
    const auto actual = ds::sparse_topk_indices(view(values), count);
    assert(actual == expected);
    const uint64_t chunks = (values.size() - 1) / ds::kVectorCapacity + 1;
    assert(ds::operator_counts.sparse_indexer_argmax == count * (chunks + 1));
    assert(ds::operator_counts.tload > 0 && ds::operator_counts.tstore > 0);
    for (uint32_t row : actual)
        assert(values[row] == -std::numeric_limits<float>::infinity());
}

int main() {
    std::vector<float> boundary(513);
    std::iota(boundary.begin(), boundary.end(), -256.0f);
    check(boundary, 512);
    check({-4.0f, -2.0f, -3.0f, -2.0f}, 4);
    check({0.0f}, 1);
    check(std::vector<float>(ds::kVectorCapacity + 1, 1.0f), 9);

    for (uint32_t size : {ds::kVectorCapacity - 1, ds::kVectorCapacity,
                          ds::kVectorCapacity + 1,
                          2 * ds::kVectorCapacity + 3}) {
        std::vector<float> scores(size);
        for (uint32_t i = 0; i < size; ++i)
            scores[i] = static_cast<float>((i * 17u) % 31u) - 15.0f;
        scores.back() = 19.0f;
        if (size > ds::kVectorCapacity) {
            scores[ds::kVectorCapacity - 1] = 18.0f;
            scores[ds::kVectorCapacity] = 18.0f;
        }
        check(scores, 17);
    }

    for (float invalid : {std::numeric_limits<float>::quiet_NaN(),
                          std::numeric_limits<float>::infinity(),
                          -std::numeric_limits<float>::infinity()}) {
        for (uint32_t position : {0u, 2u, ds::kVectorCapacity}) {
            std::vector<float> scores(ds::kVectorCapacity + 1, 0.0f);
            scores[position] = invalid;
            bool rejected = false;
            try {
                (void)ds::sparse_topk_indices(view(scores), 1);
            } catch (const std::invalid_argument &) {
                rejected = true;
            }
            assert(rejected);
        }
    }
    for (uint32_t count : {0u, 4u}) {
        std::vector<float> scores(3, 1.0f);
        bool rejected = false;
        try {
            (void)ds::sparse_topk_indices(view(scores), count);
        } catch (const std::invalid_argument &) {
            rejected = true;
        }
        assert(rejected);
    }
    std::puts("deepseek_v4_flash_indexer_pto=pass");
}
