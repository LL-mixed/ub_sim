#include "qwen3_pto_range.hpp"

#include <cmath>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>

namespace qp = lingqu::qwen3_pto;
namespace ub = pto::cpu::ub_gm;

static const ub::RunContext *active_context;
static const ub::RunContext *current_context() { return active_context; }

struct Memory {
    struct Backing { void *pointer; uint64_t bytes; };
    std::vector<Backing> backings;
    std::vector<ub::Binding> bindings;
    ub::AccessOps ops{read, write, fence};
    ub::RunContext context{91, nullptr, 0, &ops, this};
    uint64_t reads = 0, writes = 0, read_bytes = 0, write_bytes = 0;
    bool remote;

    explicit Memory(bool remote) : remote(remote) {
        bindings.reserve(256);
        active_context = &context;
        ub::installRunContextProvider(current_context);
    }
    ~Memory() {
        ub::installRunContextProvider(nullptr);
        active_context = nullptr;
    }
    qp::View view(void *pointer, uint32_t rows, uint32_t cols,
                  qp::DType dtype = qp::DType::F32, uint32_t row_stride = 0,
                  uint32_t access = uint32_t(ub::Access::ReadWrite)) {
        if (!row_stride) row_stride = cols;
        const uint64_t bytes = (uint64_t(rows - 1) * row_stride + cols) *
                               (dtype == qp::DType::F32 ? 4 : 2);
        uintptr_t address = reinterpret_cast<uintptr_t>(pointer);
        if (remote) {
            const uint64_t id = bindings.size() + 1;
            if (id > 256 || bytes > 0x100000) throw std::runtime_error("test_binding_size");
            address = ub::kApertureBase + id * 0x100000;
            backings.push_back({pointer, bytes});
            bindings.push_back({91, id, address, bytes, 0x100000000ULL + id * 0x100000,
                                bytes, access, 0, id});
            context.bindings = bindings.data();
            context.binding_count = bindings.size();
        }
        return {address, rows, cols, row_stride, 1, dtype};
    }
    static int transfer(void *opaque, uint64_t request, uint64_t id, uint64_t addr,
                        void *buffer, uint64_t size, bool store) {
        auto &m = *static_cast<Memory *>(opaque);
        if (request != 91 || !id || id > m.bindings.size()) return -1;
        const auto &b = m.bindings[id - 1];
        const auto &p = m.backings[id - 1];
        if (addr < b.ub_gm_base || size > p.bytes || addr - b.ub_gm_base > p.bytes - size)
            return -1;
        auto *target = static_cast<uint8_t *>(p.pointer) + addr - b.ub_gm_base;
        if (store) {
            ++m.writes;
            m.write_bytes += size;
            std::memcpy(target, buffer, size);
        } else {
            ++m.reads;
            m.read_bytes += size;
            std::memcpy(buffer, target, size);
        }
        return 0;
    }
    static int read(void *c, uint64_t r, uint64_t b, uint64_t a, void *d, uint64_t n) {
        return transfer(c, r, b, a, d, n, false);
    }
    static int write(void *c, uint64_t r, uint64_t b, uint64_t a, const void *s, uint64_t n) {
        return transfer(c, r, b, a, const_cast<void *>(s), n, true);
    }
    static int fence(void *, uint64_t, uint64_t, uint64_t, uint64_t, uint32_t) { return 0; }
};

static std::vector<float> values(size_t count, int salt = 0) {
    std::vector<float> result(count);
    for (size_t i = 0; i < count; ++i) result[i] = float(int((i * 17 + salt) % 97) - 48) / 19;
    return result;
}

static void equal(float actual, double expected, const char *operation, double tolerance = 3e-5) {
    if (!std::isfinite(actual) || std::abs(actual - expected) > tolerance * (1 + std::abs(expected))) {
        throw std::runtime_error(std::string(operation) + ": actual=" + std::to_string(actual) +
                                 " expected=" + std::to_string(expected));
    }
}

static void elementwise_tests(Memory &m) {
    auto a = values(2 * 4200), b = values(a.size(), 23);
    std::vector<float> out(a.size());
    auto av = m.view(a.data(), 2, 4200), bv = m.view(b.data(), 2, 4200);
    auto ov = m.view(out.data(), 2, 4200);
    qp::residual(av, bv, ov);
    for (size_t i = 0; i < a.size(); ++i) equal(out[i], double(a[i]) + b[i], "residual");
    qp::swiglu(av, bv, ov);
    for (size_t i = 0; i < a.size(); ++i)
        equal(out[i], a[i] / (1 + std::exp(-double(a[i]))) * b[i], "swiglu");

    auto x = values(2 * 128), weight = values(128, 7);
    std::vector<float> normalized(x.size());
    qp::rms_norm(m.view(x.data(), 2, 128), m.view(weight.data(), 1, 128),
                 m.view(normalized.data(), 2, 128));
    for (size_t r = 0; r < 2; ++r) {
        double sum = 0;
        for (size_t c = 0; c < 128; ++c) sum += double(x[r * 128 + c]) * x[r * 128 + c];
        for (size_t c = 0; c < 128; ++c)
            equal(normalized[r * 128 + c], x[r * 128 + c] * weight[c] /
                  std::sqrt(sum / 128 + 1e-6), "rms_norm");
    }
}

static void copy_and_linear_tests(Memory &m) {
    std::vector<half> source(5 * 19);
    for (size_t i = 0; i < source.size(); ++i) source[i] = half(float(int(i % 23) - 11) / 8);
    std::vector<float> selected(3 * 19);
    qp::copy_rows(m.view(source.data(), 5, 19, qp::DType::F16), m.view(selected.data(), 3, 19), 2);
    for (size_t i = 0; i < selected.size(); ++i) equal(selected[i], float(source[2 * 19 + i]), "embedding");
    std::vector<half> roundtrip(selected.size());
    qp::copy_rows(m.view(selected.data(), 3, 19), m.view(roundtrip.data(), 3, 19, qp::DType::F16));
    for (size_t i = 0; i < selected.size(); ++i) equal(float(roundtrip[i]), selected[i], "kv_copy_f16");
    auto weight = values(67 * 19, 9);
    std::vector<float> output(3 * 67);
    qp::linear(m.view(selected.data(), 3, 19), m.view(weight.data(), 67, 19),
               m.view(output.data(), 3, 67));
    for (size_t r = 0; r < 3; ++r) {
        for (size_t n = 0; n < 67; ++n) {
            double sum = 0;
            for (size_t k = 0; k < 19; ++k) sum += double(selected[r * 19 + k]) * weight[n * 19 + k];
            equal(output[r * 67 + n], sum, "linear_tail");
        }
    }
}

static void rope_test(Memory &m) {
    auto x = values(2 * 128);
    std::vector<float> factors(4 * 64), output(x.size());
    for (size_t r = 0; r < 2; ++r) {
        for (size_t c = 0; c < 64; ++c) {
            const double angle = double(r + 3) / std::pow(1000000.0, double(c) / 64);
            factors[(2 * r) * 64 + c] = std::cos(angle);
            factors[(2 * r + 1) * 64 + c] = std::sin(angle);
        }
    }
    qp::rope(m.view(x.data(), 2, 128), m.view(factors.data(), 4, 64), m.view(output.data(), 2, 128));
    for (size_t r = 0; r < 2; ++r) {
        for (size_t c = 0; c < 64; ++c) {
            const double lo = x[r * 128 + c], hi = x[r * 128 + 64 + c];
            const double cs = factors[2 * r * 64 + c], sn = factors[(2 * r + 1) * 64 + c];
            equal(output[r * 128 + c], lo * cs - hi * sn, "rope_lo");
            equal(output[r * 128 + 64 + c], hi * cs + lo * sn, "rope_hi");
        }
    }
}

static void attention_test(Memory &m, uint32_t past, uint32_t queries) {
    constexpr uint32_t dim = 8, qstride = 24, kvstride = 16;
    const uint32_t tokens = past + queries;
    auto q = values(queries * qstride, 3), k = values(tokens * kvstride, 7);
    auto v = values(tokens * kvstride, 13);
    std::vector<float> out(q.size(), -999);
    const float scale = 1.0f / std::sqrt(float(dim));
    qp::attention(m.view(q.data() + dim, queries, dim, qp::DType::F32, qstride),
                  m.view(k.data() + dim, tokens, dim, qp::DType::F32, kvstride),
                  m.view(v.data() + dim, tokens, dim, qp::DType::F32, kvstride),
                  m.view(out.data() + dim, queries, dim, qp::DType::F32, qstride), past, scale);
    for (uint32_t r = 0; r < queries; ++r) {
        const uint32_t used = past + r + 1;
        std::vector<double> scores(used);
        for (uint32_t t = 0; t < used; ++t) {
            for (uint32_t c = 0; c < dim; ++c)
                scores[t] += double(q[r * qstride + dim + c]) * k[t * kvstride + dim + c];
            scores[t] *= scale;
        }
        const double maximum = *std::max_element(scores.begin(), scores.end());
        double denominator = 0;
        for (auto &s : scores) { s = std::exp(s - maximum); denominator += s; }
        for (uint32_t c = 0; c < dim; ++c) {
            double expected = 0;
            for (uint32_t t = 0; t < used; ++t)
                expected += scores[t] / denominator * v[t * kvstride + dim + c];
            equal(out[r * qstride + dim + c], expected, "causal_attention");
            equal(out[r * qstride + c], -999, "head_prefix_guard");
            equal(out[r * qstride + 2 * dim + c], -999, "head_suffix_guard");
        }
    }
}

static std::vector<float> oracle_norm(const std::vector<float> &x,
                                      const std::vector<float> &w, uint32_t cols) {
    std::vector<float> out(x.size());
    for (size_t r = 0; r < x.size() / cols; ++r) {
        double sum = 0;
        for (uint32_t c = 0; c < cols; ++c) sum += double(x[r * cols + c]) * x[r * cols + c];
        for (uint32_t c = 0; c < cols; ++c)
            out[r * cols + c] = x[r * cols + c] * w[c] / std::sqrt(sum / cols + 1e-6);
    }
    return out;
}

static std::vector<float> oracle_linear(const std::vector<float> &x,
                                        const std::vector<float> &w,
                                        uint32_t input, uint32_t output) {
    std::vector<float> out(x.size() / input * output);
    for (size_t r = 0; r < x.size() / input; ++r) {
        for (uint32_t n = 0; n < output; ++n) {
            double sum = 0;
            for (uint32_t k = 0; k < input; ++k) sum += double(x[r * input + k]) * w[n * input + k];
            out[r * output + n] = sum;
        }
    }
    return out;
}

static void layer_test(Memory &m, uint32_t past, uint32_t tokens) {
    const qp::LayerGeometry g{16, 24, 4, 2, 8, float(1 / std::sqrt(8.0))};
    const uint32_t qw = 32, kw = 16, total = past + tokens;
    const std::pair<uint32_t, uint32_t> shapes[] = {
        {1,16}, {32,16}, {16,16}, {16,16}, {1,8}, {1,8},
        {16,32}, {1,16}, {24,16}, {24,16}, {16,24}};
    std::vector<std::vector<float>> weights;
    std::vector<qp::View> views;
    weights.reserve(11);
    for (size_t i = 0; i < 11; ++i) {
        weights.push_back(values(shapes[i].first * shapes[i].second, int(i * 3)));
        for (float &x : weights.back()) x = shapes[i].first == 1 ? 1 + x / 10 : x / 20;
        views.push_back(m.view(weights.back().data(), shapes[i].first, shapes[i].second));
    }
    const qp::LayerWeights w{views[0], views[1], views[2], views[3], views[4], views[5],
                            views[6], views[7], views[8], views[9], views[10]};
    auto hidden = values(tokens * g.hidden, 3);
    auto old_k = values(past * kw, 17), old_v = values(past * kw, 31);
    std::vector<float> next_k(total * kw, -999), next_v(total * kw, -999);
    std::vector<float> output(hidden.size(), -999), factors(tokens * 8);
    for (uint32_t t = 0; t < tokens; ++t) {
        for (uint32_t c = 0; c < 4; ++c) {
            const double angle = double(past + t) / std::pow(1000000.0, double(c) / 4);
            factors[t * 8 + c] = std::cos(angle);
            factors[t * 8 + 4 + c] = std::sin(angle);
        }
    }
    qp::View previous_k{}, previous_v{};
    if (past) {
        previous_k = m.view(old_k.data(), past, kw);
        previous_v = m.view(old_v.data(), past, kw);
    }
    qp::decoder_layer(g, w, m.view(hidden.data(), tokens, g.hidden),
        m.view(factors.data(), tokens * 2, 4), past ? &previous_k : nullptr,
        past ? &previous_v : nullptr, m.view(next_k.data(), total, kw),
        m.view(next_v.data(), total, kw), m.view(output.data(), tokens, g.hidden), past);

    auto norm = oracle_norm(hidden, weights[0], 16);
    auto q = oracle_norm(oracle_linear(norm, weights[1], 16, qw), weights[4], 8);
    auto k = oracle_norm(oracle_linear(norm, weights[2], 16, kw), weights[5], 8);
    auto v = oracle_linear(norm, weights[3], 16, kw);
    for (auto item : {std::pair{&q, 4u}, std::pair{&k, 2u}}) {
        for (uint32_t t = 0; t < tokens; ++t) {
            for (uint32_t h = 0; h < item.second; ++h) {
                const size_t base = (t * item.second + h) * 8;
                for (uint32_t c = 0; c < 4; ++c) {
                    const double lo = (*item.first)[base + c], hi = (*item.first)[base + 4 + c];
                    const double cs = factors[t * 8 + c], sn = factors[t * 8 + 4 + c];
                    (*item.first)[base + c] = lo * cs - hi * sn;
                    (*item.first)[base + 4 + c] = hi * cs + lo * sn;
                }
            }
        }
    }
    old_k.insert(old_k.end(), k.begin(), k.end());
    old_v.insert(old_v.end(), v.begin(), v.end());
    for (size_t i = 0; i < next_k.size(); ++i) {
        equal(next_k[i], old_k[i], "layer_key_append_reuse");
        equal(next_v[i], old_v[i], "layer_value_append_reuse");
    }
    std::vector<float> context(tokens * qw);
    for (uint32_t t = 0; t < tokens; ++t) {
        for (uint32_t h = 0; h < 4; ++h) {
            std::vector<double> scores(past + t + 1);
            for (uint32_t p = 0; p < scores.size(); ++p) {
                for (uint32_t c = 0; c < 8; ++c)
                    scores[p] += double(q[t * qw + h * 8 + c]) * old_k[p * kw + (h / 2) * 8 + c];
                scores[p] *= g.attention_scale;
            }
            const double max_score = *std::max_element(scores.begin(), scores.end());
            double sum = 0;
            for (double &x : scores) { x = std::exp(x - max_score); sum += x; }
            for (uint32_t c = 0; c < 8; ++c) {
                double value = 0;
                for (uint32_t p = 0; p < scores.size(); ++p)
                    value += scores[p] / sum * old_v[p * kw + (h / 2) * 8 + c];
                context[t * qw + h * 8 + c] = value;
            }
        }
    }
    auto residual = oracle_linear(context, weights[6], qw, 16);
    for (size_t i = 0; i < residual.size(); ++i) residual[i] += hidden[i];
    auto post = oracle_norm(residual, weights[7], 16);
    auto gate = oracle_linear(post, weights[8], 16, 24);
    auto up = oracle_linear(post, weights[9], 16, 24);
    for (size_t i = 0; i < gate.size(); ++i) gate[i] = gate[i] / (1 + std::exp(-double(gate[i]))) * up[i];
    auto down = oracle_linear(gate, weights[10], 24, 16);
    for (size_t i = 0; i < output.size(); ++i)
        equal(output[i], double(residual[i]) + down[i], "complete_decoder_layer");
}

static void embedding_logits_test(Memory &m) {
    // Noncontiguous token selection; tied embedding and LM-head weights.
    auto table = values(67 * 16, 4), norm = values(16, 5);
    const std::vector<uint32_t> tokens{66, 0, 31};
    std::vector<float> hidden(3 * 16), logits(3 * 67), expected_hidden;
    auto tv = m.view(table.data(), 67, 16);
    auto hv = m.view(hidden.data(), 3, 16);
    qp::embedding(tv, tokens, hv);
    for (uint32_t token : tokens)
        expected_hidden.insert(expected_hidden.end(), table.begin() + token * 16,
                               table.begin() + (token + 1) * 16);
    for (size_t i = 0; i < hidden.size(); ++i) equal(hidden[i], expected_hidden[i], "embedding_tokens");
    qp::terminal_logits(hv, m.view(norm.data(), 1, 16), tv, m.view(logits.data(), 3, 67));
    auto expected = oracle_linear(oracle_norm(expected_hidden, norm, 16), table, 16, 67);
    for (size_t i = 0; i < logits.size(); ++i) equal(logits[i], expected[i], "terminal_logits");
    const auto original = hidden;
    bool rejected = false;
    try { qp::embedding(tv, {0, 67, 1}, hv); }
    catch (const std::invalid_argument &) { rejected = true; }
    if (!rejected || hidden != original) throw std::runtime_error("embedding_partial_write_on_bad_token");
}

static void range_test(Memory &m, uint32_t first, uint32_t past) {
    // Zero projection weights make each decoder a residual identity. Use an
    // independent head so accidentally reusing the embedding table fails.
    qp::RangeGeometry g{first, 2, 2, past, 2, {16, 24, 4, 2, 8, float(1 / std::sqrt(8.0))}, 67};
    auto table = values(67 * 16, 8), head = values(67 * 16, 23);
    std::vector<float> norm(16, 1), constants;
    if (!first) constants.insert(constants.end(), table.begin(), table.end());
    constants.insert(constants.end(), norm.begin(), norm.end());
    constants.insert(constants.end(), head.begin(), head.end());
    const uint32_t count = 2 - first, kw = 16, total = past + 2;
    const uint32_t weight_words = 16 + 32 * 16 + 2 * 16 * 16 + 2 * 8 +
                                  16 * 32 + 16 + 3 * 24 * 16;
    constants.resize(constants.size() + count * weight_words, 0);
    for (uint32_t t = 0; t < 2; ++t) {
        constants.insert(constants.end(), 4, 1); // cos
        constants.insert(constants.end(), 4, 0); // sin
    }
    auto input = values(first ? 32 : 1, 7);
    std::vector<float> ids{66, 31}, output(32), logits(67);
    std::vector<float> previous(past ? count * (10 + 2 * past * kw) : 1, 0.25f);
    std::vector<float> next(count * (10 + 2 * total * kw), -77);
    qp::View args[7] = {
        m.view(input.data(), first ? 2 : 1, first ? 16 : 1),
        m.view(previous.data(), 1, previous.size()),
        m.view(ids.data(), 1, first ? 1 : 2),
        m.view(output.data(), 2, 16), m.view(next.data(), 1, next.size()),
        m.view(logits.data(), 1, 67), m.view(constants.data(), 1, constants.size()),
    };
    qp::decoder_range(g, args);
    const auto c = qp::operator_counts;
    if (c.embedding != (first == 0) || c.decoder_layer != count || c.terminal_logits != 1 ||
        c.copy != count * (past ? 3 : 1) + (first ? 0 : 2) + 1 ||
        c.rms_norm != count * 4 + 1 || c.linear != count * (7 + 2 * 4 * 2) + 1 ||
        c.rope != count * 4 || c.attention != count * 4 || c.residual != count * 2 ||
        c.swiglu != count || c.softmax != count * 4 * 2)
        throw std::runtime_error("range_operator_counts");
    std::thread isolated([] { qp::operator_counts.decoder_layer = 999; });
    isolated.join();
    if (qp::operator_counts.decoder_layer != count)
        throw std::runtime_error("operator_counts_not_thread_local");
    std::vector<float> expected;
    if (first) expected = input;
    else for (uint32_t token : {66, 31})
        expected.insert(expected.end(), table.begin() + token * 16, table.begin() + (token + 1) * 16);
    for (size_t i = 0; i < output.size(); ++i) equal(output[i], expected[i], "range_hidden");
    std::vector<float> last(expected.end() - 16, expected.end());
    auto expected_logits = oracle_linear(oracle_norm(last, norm, 16), head, 16, 67);
    for (size_t i = 0; i < logits.size(); ++i) equal(logits[i], expected_logits[i], "range_independent_head");
    for (uint32_t layer = 0; layer < count; ++layer) {
        const size_t start = layer * (10 + 2 * total * kw);
        for (size_t i = 0; i < 10; ++i) equal(next[start + i], -77, "range_header_preserved");
        for (uint32_t kind = 0; kind < 2; ++kind)
            for (uint32_t row = 0; row < total; ++row)
                for (uint32_t col = 0; col < kw; ++col)
                    equal(next[start + 10 + kind * total * kw + row * kw + col],
                          row < past ? 0.25 : 0, "range_kv_append");
    }
}

static void negative_tests(Memory &m) {
    auto source = values(8);
    std::vector<float> output(8, -77);
    auto src = m.view(source.data(), 1, 8);
    auto dst = m.view(output.data(), 1, 8);
    auto bad = src;
    bad.cols = 7;
    bool rejected = false;
    try { qp::residual(src, bad, dst); }
    catch (const std::invalid_argument &) { rejected = true; }
    if (!rejected) throw std::runtime_error("shape_not_rejected");
    for (float x : output) equal(x, -77, "failed_shape_no_output");
    rejected = false;
    try { qp::copy_rows(src, dst, 1); }
    catch (const std::invalid_argument &) { rejected = true; }
    if (!rejected) throw std::runtime_error("embedding_bounds_not_rejected");
    if (m.remote) {
        auto read_only = m.view(output.data(), 1, 8, qp::DType::F32, 0,
                                uint32_t(ub::Access::Read));
        rejected = false;
        try { qp::copy_rows(src, read_only); }
        catch (const ub::Error &error) { rejected = error.code() == ub::ErrorCode::AccessDenied; }
        if (!rejected) throw std::runtime_error("write_access_not_rejected");
        for (float x : output) equal(x, -77, "denied_write_no_output");
    }
}

static void fp32_norm_rounding_test(Memory &m) {
    std::vector<float> input{0.3f, 0.7f, -1.1f, 2.3f, -0.01f, 3.7f, 0.9f, -2.1f};
    std::vector<float> weight(8, 1.0f), output(8);
    float sum = 0;
    for (float x : input) { volatile float product = x * x; sum += product; }
    volatile float root = std::sqrt(sum / 8.0f + 1.0e-6f);
    const float inverse = 1.0f / root;
    qp::rms_norm(m.view(input.data(), 1, 8), m.view(weight.data(), 1, 8),
                 m.view(output.data(), 1, 8));
    for (size_t i = 0; i < input.size(); ++i) {
        if (std::bit_cast<uint32_t>(output[i]) != std::bit_cast<uint32_t>(input[i] * inverse))
            throw std::runtime_error("fp32_norm_rounding_mismatch");
    }
}

static void fp32_exp_rounding_test(Memory &m) {
    constexpr uint32_t count = 4096;
    std::vector<float> input(count), output(count);
    float (*volatile exp32)(float) = std::exp;
    uint32_t mismatches = 0;
    for (int batch = 0; batch < 4; ++batch) {
        for (uint32_t i = 0; i < count; ++i)
            input[i] = float(int(i) + batch * int(count) - 8192) / 397.0f;
        qp::Row source(count), destination(count);
        qp::load(source, m.view(input.data(), 1, count), 0);
        pto::TEXP(destination, source);
        qp::store(m.view(output.data(), 1, count), 0, destination);
        for (uint32_t i = 0; i < count; ++i)
            if (std::bit_cast<uint32_t>(output[i]) != std::bit_cast<uint32_t>(exp32(input[i])))
                ++mismatches;
    }
    if (mismatches)
        throw std::runtime_error("fp32_exp_rounding_mismatches=" + std::to_string(mismatches));
}

int main(int argc, char **argv) {
    if (argc != 2 || (std::string(argv[1]) != "gm" && std::string(argv[1]) != "ub-gm")) {
        std::cerr << "usage: qwen3_pto_ops_golden gm|ub-gm\n";
        return 2;
    }
    try {
        Memory memory(std::string(argv[1]) == "ub-gm");
        fp32_exp_rounding_test(memory);
        fp32_norm_rounding_test(memory);
        elementwise_tests(memory);
        copy_and_linear_tests(memory);
        rope_test(memory);
        attention_test(memory, 0, 3);
        attention_test(memory, 2, 2);
        layer_test(memory, 0, 3);
        layer_test(memory, 3, 2);
        embedding_logits_test(memory);
        range_test(memory, 0, 0);
        range_test(memory, 1, 3);
        negative_tests(memory);
        if (memory.remote && (!memory.reads || !memory.writes))
            throw std::runtime_error("missing_ub_gm_callback_evidence");
        std::cout << "{\"status\":\"pass\",\"backend\":\"pto_cpu\",\"memory\":\"" << argv[1]
                  << "\",\"read_callbacks\":" << memory.reads
                  << ",\"write_callbacks\":" << memory.writes
                  << ",\"read_bytes\":" << memory.read_bytes
                  << ",\"write_bytes\":" << memory.write_bytes
                  << ",\"operations\":[\"rms_norm\",\"linear\",\"rope\",\"attention\","
                     "\"softmax\",\"residual\",\"swiglu\",\"embedding\",\"kv_copy\",\"cast\"],"
                     "\"decoder_layer_cases\":[\"prefill\",\"decode_with_past_kv\"],"
                     "\"model_edge_cases\":[\"embedding_tokens\",\"terminal_logits\"],"
                     "\"range_cases\":[\"full_prefill_independent_head\",\"terminal_decode_with_kv\"]}\n";
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
