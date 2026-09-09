#pragma once
#include "qwen3_pto_ops.hpp"
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace lingqu::qwen3_pto {

// Diagnostic output only. No snapshot is ever consumed by model computation.
class RangeTrace {
    FILE *file_ = nullptr;
    uint32_t end_;

    void word(uint32_t value) {
        const unsigned char bytes[] = {static_cast<unsigned char>(value),
            static_cast<unsigned char>(value >> 8),
            static_cast<unsigned char>(value >> 16),
            static_cast<unsigned char>(value >> 24)};
        if (std::fwrite(bytes, 1, sizeof(bytes), file_) != sizeof(bytes))
            throw std::runtime_error("qwen3_pto_trace_write_failed");
    }
    void header(uint32_t kind, uint32_t layer, uint32_t row, uint32_t col,
                uint32_t rows, uint32_t cols) {
        for (uint32_t v : {kind, layer, row, col, rows, cols}) word(v);
    }
    void tile(Row &row) {
        for (uint32_t i = 0; i < row.GetValidCol(); ++i)
            word(std::bit_cast<uint32_t>(row.GetValue(i)));
    }
public:
    RangeTrace(uint32_t first, uint32_t end, uint32_t past, uint32_t tokens,
               uint32_t hidden, uint32_t kv_width, uint32_t vocab) : end_(end) {
        const char *directory = std::getenv("SIM_QWEN3_PTO_TRACE_DIR");
        if (!directory || !*directory) return;
        const std::string path = std::string(directory) + "/range-" +
            std::to_string(first) + "-" + std::to_string(end) + "-past-" +
            std::to_string(past) + ".bin";
        file_ = std::fopen(path.c_str(), "wbx");
        if (!file_) throw std::runtime_error("qwen3_pto_trace_open_failed");
        try {
            if (std::fwrite("QPTOTR1\0", 1, 8, file_) != 8)
                throw std::runtime_error("qwen3_pto_trace_header_failed");
            for (uint32_t v : {first, end, past, tokens, hidden, kv_width, vocab}) word(v);
        } catch (...) { std::fclose(file_); file_ = nullptr; throw; }
    }
    RangeTrace(const RangeTrace &) = delete;
    RangeTrace &operator=(const RangeTrace &) = delete;
    ~RangeTrace() { if (file_) std::fclose(file_); }
    bool enabled() const { return file_ != nullptr; }

    void matrix(uint32_t kind, uint32_t layer, const View &view) {
        if (!file_) return;
        view.validate();
        header(kind, layer, 0, 0, view.rows, view.cols);
        for (uint32_t r = 0; r < view.rows; ++r) {
            for (uint32_t c = 0; c < view.cols; c += kRowCapacity) {
                Row values(std::min(kRowCapacity, view.cols - c));
                load(values, view, r, c);
                tile(values);
            }
        }
    }
    static void logits(Row &values, uint32_t row, uint32_t col, void *context) {
        auto &trace = *static_cast<RangeTrace *>(context);
        trace.header(4, trace.end_, row, col, 1, values.GetValidCol());
        trace.tile(values);
    }
    void finish() {
        if (!file_) return;
        header(0, 0, 0, 0, 0, 0);
        const bool flushed = std::fflush(file_) == 0;
        const bool closed = std::fclose(file_) == 0;
        file_ = nullptr;
        if (!flushed || !closed) throw std::runtime_error("qwen3_pto_trace_close_failed");
    }
};

} // namespace lingqu::qwen3_pto
