#include "quote_attrib.h"

#include "tokenizer.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <optional>

namespace rm::booknlp {
namespace {

constexpr int kN = 2000, kS = 512;

struct Context {
    int start, end;
    std::vector<std::pair<int, int>> spans;   // per token in [start, end]: codepoint [a, b) in text
    BpeEncoding enc;
};

struct Instance {
    int idx;      // quote / entity index
    int ctx;
    int st, et;   // subword span, inclusive
};

std::vector<int> containing_or_overlapping(const std::vector<Context> & cs, int s, int e, bool & clipped) {
    std::vector<int> out;
    for (size_t c = 0; c < cs.size(); ++c)
        if (s >= cs[c].start && e <= cs[c].end) out.push_back(static_cast<int>(c));
    clipped = out.empty();
    if (clipped)
        for (size_t c = 0; c < cs.size(); ++c)
            if (s <= cs[c].end && e >= cs[c].start) out.push_back(static_cast<int>(c));
    return out;
}

std::optional<std::pair<int, int>> resolve_span(const Context & c, int s, int e, bool clipped) {
    const int ts = std::max(s, c.start), te = std::min(e, c.end);
    const int a = c.spans[ts - c.start].first, b = c.spans[te - c.start].second;
    int st = -1, et = -1;
    for (size_t p = 0; p < c.enc.ids.size(); ++p) {
        if (c.enc.special[p]) continue;
        if (c.enc.offsets[p].first < b && c.enc.offsets[p].second > a) {
            if (st < 0) st = static_cast<int>(p);
            et = static_cast<int>(p);
        }
    }
    if (st < 0) return std::nullopt;
    if (clipped) {
        st = std::max(st, 0);
        et = std::min(et, static_cast<int>(c.enc.ids.size()) - 1);
    }
    return std::make_pair(st, et);
}

}  // namespace

QuoteAttribution::QuoteAttribution(const GgufModel & m) : m_(m), bert_(m) {
    b3_ = m.read("proj.3.bias")[0];
}

std::vector<int> QuoteAttribution::tag(const std::vector<std::pair<int, int>> & quotes,
                                       const std::vector<Entity> & entities, const std::vector<Token> & tokens) const {
    const int total = static_cast<int>(tokens.size());
    // --- build_context_nodes
    std::vector<Context> contexts;
    int start = 0, par_id = 0;
    while (start < total) {
        int end = std::min(start + kN - 1, total - 1);
        while (end < total - 1 && tokens[end].sentence_id == tokens[end + 1].sentence_id) ++end;
        Context c{start, end, {}, {}};
        std::string text;
        int off = 0;
        for (int t = start; t <= end; ++t) {
            if (tokens[t].paragraph_id != par_id) {
                text += "\n\n";
                off += 2;
                par_id = tokens[t].paragraph_id;
            }
            const int len = static_cast<int>(utf8_to_u32(tokens[t].text).size());
            c.spans.emplace_back(off, off + len);
            text += tokens[t].text;
            text += ' ';
            off += len + 1;
        }
        c.enc = bert_.tokenizer().encode(text);
        contexts.push_back(std::move(c));
        if (end == total - 1) break;
        start += kN - kS;
    }
    // --- quote and PER mention instances per context
    std::vector<std::vector<Instance>> q_inst(contexts.size()), m_inst(contexts.size());
    for (size_t i = 0; i < quotes.size(); ++i) {
        bool clipped;
        for (int c : containing_or_overlapping(contexts, quotes[i].first, quotes[i].second, clipped))
            if (auto sp = resolve_span(contexts[c], quotes[i].first, quotes[i].second, clipped))
                q_inst[c].push_back({static_cast<int>(i), c, sp->first, sp->second});
    }
    for (size_t i = 0; i < entities.size(); ++i) {
        const std::string & cat = entities[i].cat;
        const size_t us = cat.find('_');
        if ((us == std::string::npos ? cat : cat.substr(us + 1)) != "PER") continue;
        bool clipped;
        for (int c : containing_or_overlapping(contexts, entities[i].start, entities[i].end, clipped))
            if (auto sp = resolve_span(contexts[c], entities[i].start, entities[i].end, clipped))
                m_inst[c].push_back({static_cast<int>(i), c, sp->first, sp->second});
    }
    // --- score
    const int H = bert_.n_embd(), F = 2 * H;
    std::map<int, std::pair<float, int>> best;   // quote -> (prob, entity)
    int max_quote = -1;
    for (size_t c = 0; c < contexts.size(); ++c) {
        if (q_inst[c].empty()) continue;
        const auto & qs = q_inst[c];
        const auto & ms = m_inst[c];
        if (ms.empty()) continue;   // no scores for this window (numel() == 0)
        for (const auto & q : qs) max_quote = std::max(max_quote, q.idx);
        const int T = static_cast<int>(contexts[c].enc.ids.size());
        const int nq = static_cast<int>(qs.size()), nm = static_cast<int>(ms.size());
        // encoder + [start ; end] span features of quotes and mentions
        std::vector<float> qf, mf;
        {
            Graph g(m_.backend());
            ModernBert::Inputs in;
            ggml_tensor * x = bert_.build(g, T, in);
            auto span_features = [&](const std::vector<Instance> & v, ggml_tensor *& st, ggml_tensor *& et) {
                st = g.input(GGML_TYPE_I32, static_cast<int64_t>(v.size()));
                et = g.input(GGML_TYPE_I32, static_cast<int64_t>(v.size()));
                ggml_tensor * f = ggml_concat(g.ctx(), ggml_get_rows(g.ctx(), x, st), ggml_get_rows(g.ctx(), x, et), 0);
                g.output(f);
                return f;
            };
            ggml_tensor *qst, *qet, *mst, *met;
            ggml_tensor * qft = span_features(qs, qst, qet);
            ggml_tensor * mft = span_features(ms, mst, met);
            g.allocate();
            bert_.set_inputs(in, contexts[c].enc.ids);
            auto set_idx = [&](const std::vector<Instance> & v, ggml_tensor * st, ggml_tensor * et) {
                std::vector<int> s(v.size()), e(v.size());
                for (size_t i = 0; i < v.size(); ++i) {   // Python indexing: -1 is the last position
                    s[i] = v[i].st < 0 ? v[i].st + T : v[i].st;
                    e[i] = v[i].et < 0 ? v[i].et + T : v[i].et;
                }
                Graph::set(st, s.data());
                Graph::set(et, e.data());
            };
            set_idx(qs, qst, qet);
            set_idx(ms, mst, met);
            g.run();
            qf = Graph::get(qft);
            mf = Graph::get(mft);
        }
        // proj(q, m) = w3 . gelu(W_q q + W_m m + b0) + b3, for every pair; quotes in chunks
        const int D = static_cast<int>(m_.tensor("proj.0.bias")->ne[0]);
        const int step = std::max(1, static_cast<int>((64ll << 20) / (static_cast<int64_t>(D) * nm)));
        std::vector<float> scores(static_cast<size_t>(nq) * nm);
        for (int q0 = 0; q0 < nq; q0 += step) {
            const int n = std::min(step, nq - q0);
            Graph g(m_.backend());
            ggml_context * ctx = g.ctx();
            ggml_tensor * Q = g.input(GGML_TYPE_F32, F, n);
            ggml_tensor * M = g.input(GGML_TYPE_F32, F, nm);
            ggml_tensor * A = ggml_mul_mat(ctx, m_.tensor("proj.0.weight_q"), Q);
            ggml_mul_mat_set_prec(A, GGML_PREC_F32);
            A = ggml_add(ctx, A, m_.tensor("proj.0.bias"));
            ggml_tensor * B = ggml_mul_mat(ctx, m_.tensor("proj.0.weight_m"), M);
            ggml_mul_mat_set_prec(B, GGML_PREC_F32);
            ggml_tensor * shape = ggml_new_tensor_3d(ctx, GGML_TYPE_F32, D, n, nm);
            ggml_tensor * S = ggml_add(ctx, ggml_repeat(ctx, ggml_reshape_3d(ctx, A, D, n, 1), shape),
                                       ggml_reshape_3d(ctx, B, D, 1, nm));
            S = ggml_gelu_erf(ctx, S);
            ggml_tensor * out = ggml_mul_mat(ctx, m_.tensor("proj.3.weight"), ggml_reshape_2d(ctx, S, D, n * nm));
            ggml_mul_mat_set_prec(out, GGML_PREC_F32);
            g.output(out);
            g.allocate();
            Graph::set(Q, &qf[static_cast<size_t>(q0) * F]);
            Graph::set(M, mf.data());
            g.run();
            const auto r = Graph::get(out);   // index qi + n * mj
            for (int qi = 0; qi < n; ++qi)
                for (int mj = 0; mj < nm; ++mj)
                    scores[static_cast<size_t>(q0 + qi) * nm + mj] = r[static_cast<size_t>(mj) * n + qi] + b3_;
        }
        for (int qi = 0; qi < nq; ++qi) {
            const float * sc = &scores[static_cast<size_t>(qi) * nm];
            const size_t arg = std::max_element(sc, sc + nm) - sc;
            double sum = 0.0;
            for (int mj = 0; mj < nm; ++mj) sum += std::exp(static_cast<double>(sc[mj] - sc[arg]));
            const float prob = static_cast<float>(1.0 / sum);
            auto it = best.find(qs[qi].idx);
            if (it == best.end() || prob > it->second.first) best[qs[qi].idx] = {prob, ms[arg].idx};
        }
    }
    std::vector<int> out(static_cast<size_t>(max_quote + 1), -1);
    for (const auto & [qi, pm] : best)
        if (qi <= max_quote) out[qi] = pm.second;
    return out;
}

}  // namespace rm::booknlp
