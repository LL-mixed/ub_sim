#pragma once

#include "qwen3_pto_ops.hpp"

namespace lingqu::qwen3_pto {

struct LayerGeometry {
    uint32_t hidden, intermediate, query_heads, kv_heads, head_dim;
    float attention_scale;

    void validate() const {
        require(hidden && hidden <= kRowCapacity && intermediate &&
                intermediate <= kRowCapacity && query_heads && kv_heads &&
                query_heads % kv_heads == 0 && head_dim && head_dim % 2 == 0 &&
                uint64_t(query_heads) * head_dim <= kRowCapacity &&
                std::isfinite(attention_scale) && attention_scale > 0);
    }
};

struct LayerWeights {
    View input_norm, query, key, value, query_norm, key_norm;
    View output, post_norm, gate, up, down;
};

inline void geometry(const View &view, uint32_t rows, uint32_t cols) {
    view.validate();
    require(view.rows == rows && view.cols == cols);
}

inline View slice(const View &view, uint32_t row, uint32_t col,
                  uint32_t rows, uint32_t cols) {
    view.validate();
    require(rows && cols && row < view.rows && col < view.cols &&
            rows <= view.rows - row && cols <= view.cols - col);
    return {view.at(row, col), rows, cols, view.row_stride, view.col_stride, view.dtype};
}

class LocalMatrix {
    std::vector<float> data_;
    uint32_t rows_, cols_;
public:
    LocalMatrix(uint32_t rows, uint32_t cols)
        : data_(size_t(rows) * cols), rows_(rows), cols_(cols) {}
    View view() {
        return {reinterpret_cast<uintptr_t>(data_.data()), rows_, cols_, cols_, 1, DType::F32};
    }
    View heads(uint32_t head_dim) {
        require(head_dim && cols_ % head_dim == 0 &&
                uint64_t(rows_) * (cols_ / head_dim) <= UINT32_MAX);
        return {reinterpret_cast<uintptr_t>(data_.data()), rows_ * (cols_ / head_dim),
                head_dim, head_dim, 1, DType::F32};
    }
};

// Token IDs are control metadata; the selected weight rows stay in GM and
// are loaded by PTO. Validate the entire selection before producing output.
inline void embedding(const View &table, const std::vector<uint32_t> &tokens,
                      const View &hidden) {
    ++operator_counts.embedding;
    table.validate();
    geometry(hidden, uint32_t(tokens.size()), table.cols);
    require(!tokens.empty() && tokens.size() <= kRowCapacity);
    for (uint32_t token : tokens) require(token < table.rows);
    for (uint32_t r = 0; r < tokens.size(); ++r)
        copy_rows(table, slice(hidden, r, 0, 1, hidden.cols), tokens[r]);
}

inline void terminal_logits(const View &hidden, const View &norm_weight,
                            const View &head_weight, const View &logits,
                            LinearObserver observe = nullptr, void *context = nullptr) {
    ++operator_counts.terminal_logits;
    hidden.validate();
    require(hidden.cols <= kRowCapacity);
    geometry(norm_weight, 1, hidden.cols);
    geometry(head_weight, logits.cols, hidden.cols);
    geometry(logits, hidden.rows, head_weight.rows);
    LocalMatrix normalized(hidden.rows, hidden.cols);
    rms_norm(hidden, norm_weight, normalized.view());
    linear(normalized.view(), head_weight, logits, observe, context);
}

// All numerical operations, including cache copying, use the common PTO
// implementation. No model/reference callback participates in this graph.
// Empty prefill caches are represented by null pointers, not zero-size views.
inline void decoder_layer(const LayerGeometry &g, const LayerWeights &w,
                          const View &hidden, const View &factors,
                          const View *previous_key, const View *previous_value,
                          const View &next_key, const View &next_value,
                          const View &output, uint32_t past_tokens) {
    ++operator_counts.decoder_layer;
    g.validate();
    const uint32_t tokens = hidden.rows;
    require(tokens && uint64_t(tokens) + past_tokens <= kRowCapacity);
    const uint32_t qwidth = g.query_heads * g.head_dim;
    const uint32_t kvwidth = g.kv_heads * g.head_dim;
    geometry(hidden, tokens, g.hidden);
    geometry(output, tokens, g.hidden);
    geometry(factors, tokens * 2, g.head_dim / 2);
    geometry(next_key, past_tokens + tokens, kvwidth);
    geometry(next_value, past_tokens + tokens, kvwidth);
    require(next_key.dtype == DType::F32 && next_value.dtype == DType::F32);
    if (past_tokens) {
        require(previous_key && previous_value);
        geometry(*previous_key, past_tokens, kvwidth);
        geometry(*previous_value, past_tokens, kvwidth);
        require(previous_key->dtype == DType::F32 && previous_value->dtype == DType::F32);
    } else {
        require(!previous_key && !previous_value);
    }
    geometry(w.input_norm, 1, g.hidden);
    geometry(w.query, qwidth, g.hidden);
    geometry(w.key, kvwidth, g.hidden);
    geometry(w.value, kvwidth, g.hidden);
    geometry(w.query_norm, 1, g.head_dim);
    geometry(w.key_norm, 1, g.head_dim);
    geometry(w.output, g.hidden, qwidth);
    geometry(w.post_norm, 1, g.hidden);
    geometry(w.gate, g.intermediate, g.hidden);
    geometry(w.up, g.intermediate, g.hidden);
    geometry(w.down, g.hidden, g.intermediate);

    LocalMatrix normalized(tokens, g.hidden), q(tokens, qwidth), k(tokens, kvwidth);
    LocalMatrix qnorm(tokens, qwidth), knorm(tokens, kvwidth), rotated_q(tokens, qwidth);
    LocalMatrix rotated_k(tokens, kvwidth), context(tokens, qwidth);
    LocalMatrix projected(tokens, g.hidden), residual_hidden(tokens, g.hidden);
    LocalMatrix post(tokens, g.hidden), gate(tokens, g.intermediate);
    LocalMatrix up(tokens, g.intermediate), activated(tokens, g.intermediate);
    LocalMatrix down(tokens, g.hidden);

    if (past_tokens) {
        copy_rows(*previous_key, slice(next_key, 0, 0, past_tokens, kvwidth));
        copy_rows(*previous_value, slice(next_value, 0, 0, past_tokens, kvwidth));
    }
    rms_norm(hidden, w.input_norm, normalized.view());
    linear(normalized.view(), w.query, q.view());
    linear(normalized.view(), w.key, k.view());
    linear(normalized.view(), w.value,
           slice(next_value, past_tokens, 0, tokens, kvwidth));
    rms_norm(q.heads(g.head_dim), w.query_norm, qnorm.heads(g.head_dim));
    rms_norm(k.heads(g.head_dim), w.key_norm, knorm.heads(g.head_dim));
    for (uint32_t t = 0; t < tokens; ++t) {
        const auto position_factors = slice(factors, t * 2, 0, 2, g.head_dim / 2);
        rope(slice(qnorm.heads(g.head_dim), t * g.query_heads, 0, g.query_heads, g.head_dim),
             position_factors,
             slice(rotated_q.heads(g.head_dim), t * g.query_heads, 0, g.query_heads, g.head_dim));
        rope(slice(knorm.heads(g.head_dim), t * g.kv_heads, 0, g.kv_heads, g.head_dim),
             position_factors,
             slice(rotated_k.heads(g.head_dim), t * g.kv_heads, 0, g.kv_heads, g.head_dim));
    }
    copy_rows(rotated_k.view(), slice(next_key, past_tokens, 0, tokens, kvwidth));
    for (uint32_t h = 0; h < g.query_heads; ++h) {
        const uint32_t kv = h / (g.query_heads / g.kv_heads);
        attention(slice(rotated_q.view(), 0, h * g.head_dim, tokens, g.head_dim),
                  slice(next_key, 0, kv * g.head_dim, next_key.rows, g.head_dim),
                  slice(next_value, 0, kv * g.head_dim, next_value.rows, g.head_dim),
                  slice(context.view(), 0, h * g.head_dim, tokens, g.head_dim),
                  past_tokens, g.attention_scale);
    }
    linear(context.view(), w.output, projected.view());
    residual(hidden, projected.view(), residual_hidden.view());
    rms_norm(residual_hidden.view(), w.post_norm, post.view());
    linear(post.view(), w.gate, gate.view());
    linear(post.view(), w.up, up.view());
    swiglu(gate.view(), up.view(), activated.view());
    linear(activated.view(), w.down, down.view());
    residual(residual_hidden.view(), down.view(), output);
}

} // namespace lingqu::qwen3_pto
