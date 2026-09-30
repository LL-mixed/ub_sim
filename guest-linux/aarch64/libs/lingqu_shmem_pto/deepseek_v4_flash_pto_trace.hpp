#pragma once

#include "deepseek_v4_flash_pto_range.hpp"

#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <vector>

namespace lingqu::deepseek_v4_flash_pto {

class TerminalLogitsTrace {
    std::string path_;
    std::vector<float> values_;

public:
    explicit TerminalLogitsTrace(const RangeGeometry &geometry) {
        const char *directory = std::getenv("SIM_W5_TEST_PTO_TRACE_DIR");
        if (!directory || !*directory || geometry.end != kLayers) return;
        path_ = std::string(directory) + "/deepseek-range-" +
            std::to_string(geometry.first) + "-" +
            std::to_string(geometry.end) + "-past-" +
            std::to_string(geometry.past) + "-tokens-" +
            std::to_string(geometry.tokens) + "-logits.f32";
        values_.resize(kVocab);
    }

    bool enabled() const { return !path_.empty(); }

    F32View view() {
        if (!enabled()) throw std::logic_error("deepseek_pto_trace_disabled");
        return {reinterpret_cast<uintptr_t>(values_.data()), 1, kVocab,
                kVocab, 1};
    }

    void finish() const {
        if (!enabled()) return;
        FILE *file = std::fopen(path_.c_str(), "wbx");
        if (!file) throw std::runtime_error("deepseek_pto_trace_open_failed");
        const bool written =
            std::fwrite(values_.data(), sizeof(float), values_.size(), file) ==
            values_.size();
        const bool closed = std::fclose(file) == 0;
        if (!written || !closed)
            throw std::runtime_error("deepseek_pto_trace_write_failed");
    }
};

} // namespace lingqu::deepseek_v4_flash_pto
