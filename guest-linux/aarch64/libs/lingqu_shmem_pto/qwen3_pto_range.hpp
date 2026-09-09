#pragma once
#include "qwen3_pto_layer.hpp"
#include "qwen3_pto_trace.hpp"
#include <cinttypes>
#include <cstdio>

namespace lingqu::qwen3_pto {

struct RangeGeometry {
    uint32_t first, end, layers, past, tokens;
    LayerGeometry layer;
    uint32_t vocab;
    void validate() const {
        layer.validate();
        require(first < end && end <= layers && layers <= 128 && tokens &&
                uint64_t(past) + tokens <= kRowCapacity && vocab && vocab <= 16777216);
    }
};

class PackedViews {
    View backing_;
    uint64_t offset_ = 0;
public:
    explicit PackedViews(View backing) : backing_(backing) {
        backing.validate();
        require(backing.rows == 1 && backing.col_stride == 1 && backing.dtype == DType::F32);
    }
    View take(uint32_t rows, uint32_t cols) {
        const uint64_t count = uint64_t(rows) * cols;
        require(rows && cols && offset_ <= backing_.cols && count <= backing_.cols - offset_);
        View result{backing_.address + offset_ * 4, rows, cols, cols, 1, DType::F32};
        offset_ += count;
        return result;
    }
    void skip(uint32_t words) { (void)take(1, words); }
    void finish() const { require(offset_ == backing_.cols); }
};

inline void decoder_range(const RangeGeometry &g, const View (&args)[7]) {
    operator_counts = {};
    g.validate();
    const auto &d = g.layer;
    const uint32_t qw = d.query_heads * d.head_dim, kw = d.kv_heads * d.head_dim;
    const uint32_t count = g.end - g.first, total = g.past + g.tokens;
    const uint64_t previous_words = uint64_t(count) * (10 + 2ULL * g.past * kw);
    const uint64_t next_words = uint64_t(count) * (10 + 2ULL * total * kw);
    require(previous_words <= UINT32_MAX && next_words <= UINT32_MAX);
    for (const auto &view : args) view.validate();
    geometry(args[0], g.first ? g.tokens : 1, g.first ? d.hidden : 1);
    geometry(args[1], 1, g.past ? uint32_t(previous_words) : 1);
    geometry(args[2], 1, g.first ? 1 : g.tokens);
    geometry(args[3], g.tokens, d.hidden);
    geometry(args[4], 1, uint32_t(next_words));
    geometry(args[5], 1, g.end == g.layers ? g.vocab : 1);
    require(args[1].dtype == DType::F32 && args[2].dtype == DType::F32 &&
            args[4].dtype == DType::F32 && args[5].dtype == DType::F32);

    PackedViews constants(args[6]), previous(args[1]), next(args[4]);
    View table{}, norm{}, head{};
    if (g.first == 0) table = constants.take(g.vocab, d.hidden);
    if (g.end == g.layers) {
        norm = constants.take(1, d.hidden);
        head = constants.take(g.vocab, d.hidden);
    }
    std::vector<LayerWeights> weights;
    std::vector<View> old_key, old_value, new_key, new_value;
    for (uint32_t i = 0; i < count; ++i) {
        weights.push_back({constants.take(1, d.hidden), constants.take(qw, d.hidden),
            constants.take(kw, d.hidden), constants.take(kw, d.hidden),
            constants.take(1, d.head_dim), constants.take(1, d.head_dim),
            constants.take(d.hidden, qw), constants.take(1, d.hidden),
            constants.take(d.intermediate, d.hidden), constants.take(d.intermediate, d.hidden),
            constants.take(d.hidden, d.intermediate)});
        if (g.past) {
            previous.skip(10);
            old_key.push_back(previous.take(g.past, kw));
            old_value.push_back(previous.take(g.past, kw));
        }
        next.skip(10);
        new_key.push_back(next.take(total, kw));
        new_value.push_back(next.take(total, kw));
    }
    const auto factors = constants.take(g.tokens * 2, d.head_dim / 2);
    constants.finish();
    next.finish();
    if (g.past) previous.finish();

    RangeTrace trace(g.first, g.end, g.past, g.tokens, d.hidden, kw, g.vocab);

    LocalMatrix a(g.tokens, d.hidden), b(g.tokens, d.hidden);
    View current = args[0];
    if (g.first == 0) {
        Row ids(g.tokens);
        load(ids, args[2], 0);
        std::vector<uint32_t> selected;
        for (uint32_t i = 0; i < g.tokens; ++i) {
            const float id = ids.GetValue(i); // Control index, not numerical model math.
            require(std::isfinite(id) && id >= 0 && id < float(g.vocab) && float(uint32_t(id)) == id);
            selected.push_back(uint32_t(id));
        }
        embedding(table, selected, a.view());
        current = a.view();
    }
    for (uint32_t i = 0; i < count; ++i) {
        View destination = current.address == a.view().address ? b.view() : a.view();
        decoder_layer(d, weights[i], current, factors,
            g.past ? &old_key[i] : nullptr, g.past ? &old_value[i] : nullptr,
            new_key[i], new_value[i], destination, g.past);
        current = destination;
        trace.matrix(1, g.first + i, current);
        trace.matrix(2, g.first + i, new_key[i]);
        trace.matrix(3, g.first + i, new_value[i]);
    }
    copy_rows(current, args[3]);
    if (g.end == g.layers)
        terminal_logits(slice(current, g.tokens - 1, 0, 1, d.hidden), norm, head, args[5],
                        trace.enabled() ? &RangeTrace::logits : nullptr, &trace);
    trace.finish();
    const auto &c = operator_counts;
    std::fprintf(stderr, "QWEN3_PTO_OPERATOR_COUNTS first=%u end=%u tokens=%u past=%u"
        " copy=%" PRIu64 " rms_norm=%" PRIu64 " linear=%" PRIu64 " rope=%" PRIu64
        " attention=%" PRIu64 " residual=%" PRIu64 " swiglu=%" PRIu64
        " softmax=%" PRIu64 " embedding=%" PRIu64 " decoder_layer=%" PRIu64
        " terminal_logits=%" PRIu64 " kernel_complete=1\n",
        g.first, g.end, g.tokens, g.past, c.copy, c.rms_norm, c.linear, c.rope,
        c.attention, c.residual, c.swiglu, c.softmax, c.embedding, c.decoder_layer,
        c.terminal_logits);
}

} // namespace lingqu::qwen3_pto
