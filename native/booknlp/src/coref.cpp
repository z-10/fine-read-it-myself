#include "coref.h"

#include "frontend.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <numeric>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace rm::booknlp {
namespace {

constexpr int kMaxSentence = 500, kMaxPrevious = 20, kTop = 10;
constexpr int kNone = INT_MIN;

int width_bucket(int d) { return std::abs(d) < 10 ? std::abs(d) + 1 : 11; }
int distance_bucket(int d) { return d < 30 ? d + 10 : (d < 40 ? 41 : 42); }

// y[r] = sum_k W[r, col0 + k] * x[k] for k < n, W row-major with `cols` columns
void matvec(const std::vector<float> & W, int rows, int cols, int col0, const float * x, int n, float * y) {
    for (int r = 0; r < rows; ++r) {
        const float * w = &W[static_cast<size_t>(r) * cols + col0];
        float acc = 0.f;
        for (int k = 0; k < n; ++k) acc += w[k] * x[k];
        y[r] = acc;
    }
}

struct Ment {
    int chunk, ws, we;           // word positions in the chunk (we from the end token's own mapping)
    bool in_quote;
    int quote_mention = kNone;   // entity index of the attributed speaker of the enclosing quote
    std::string proper, text_lower;
    int single_word;
};

}  // namespace

Coref::Coref(const GgufModel & m) : m_(m), bert_(m) {
    dist_emb_ = m.read("distance_embeddings.weight");
    speaker_emb_ = m.read("speaker_embeddings.weight");
    nested_emb_ = m.read("nested_embeddings.weight");
    width_emb_ = m.read("width_embeddings.weight");
    quote_emb_ = m.read("quote_embeddings.weight");
    att2_b_ = m.read("attention2.bias")[0];
    u1_w_ = m.read("unary1.weight");
    u1_b_ = m.read("unary1.bias");
    u2_w_ = m.read("unary2.weight");
    u2_b_ = m.read("unary2.bias");
    u3_w_ = m.read("unary3.weight");
    u3_b_ = m.read("unary3.bias")[0];
    m1_rest_ = m.read("mention_mention1.weight_rest");
    m1_b_ = m.read("mention_mention1.bias");
    m2_w_ = m.read("mention_mention2.weight");
    m2_b_ = m.read("mention_mention2.bias");
    m3_w_ = m.read("mention_mention3.weight");
    m3_b_ = m.read("mention_mention3.bias")[0];
}

std::vector<int> Coref::tag(const std::vector<Token> & tokens, const std::vector<Entity> & entities,
                            const std::vector<int> & refs, const std::map<int, GenderInfo> & ref_genders,
                            const std::vector<int> & attributions, const std::vector<std::pair<int, int>> & quotes) const {
    const WordPiece & wp = bert_.vocab();
    const int H = bert_.n_embd();
    const int N = static_cast<int>(entities.size());
    if (N == 0) return {};
    // --- convert_data: sentences of <= 500 wordpieces, packed into chunks
    std::vector<std::vector<int>> sents(1);
    std::vector<std::vector<int>> sent_len(1);
    std::vector<std::string> wptok(tokens.size());
    std::vector<std::vector<std::string>> pieces(tokens.size());
    {
        int last = -1, length = 0;
        for (size_t i = 0; i < tokens.size(); ++i) {
            wptok[i] = first_is_upper(tokens[i].text) ? "[CAP] " + py_lower(tokens[i].text) : tokens[i].text;
            pieces[i] = wp.tokenize(wptok[i]);
            const int n = static_cast<int>(pieces[i].size());
            if (last != -1 && (tokens[i].sentence_id != last || length + n > kMaxSentence)) {
                sents.emplace_back();
                length = 0;
            }
            sents.back().push_back(static_cast<int>(i));
            last = tokens[i].sentence_id;
            length += n;
        }
    }
    struct Chunk {
        std::vector<int> words;   // token index, -1 = [CLS], -2 = [SEP]
    };
    std::vector<Chunk> chunks;
    std::vector<std::pair<int, int>> mapper(tokens.size(), {0, 0});
    {
        Chunk cur;
        cur.words.push_back(-1);
        int running = 0;
        for (const auto & s : sents) {
            int length = 0;
            for (int t : s) length += static_cast<int>(pieces[t].size());
            if (length + running >= kMaxSentence) {
                cur.words.push_back(-2);
                chunks.push_back(cur);
                running = 1;
                cur = Chunk();
                cur.words.push_back(-1);
            }
            running += length;
            for (int t : s) {
                mapper[t] = {static_cast<int>(chunks.size()), static_cast<int>(cur.words.size())};
                cur.words.push_back(t);
            }
        }
        if (cur.words.size() > 1) {
            cur.words.push_back(-2);
            chunks.push_back(cur);
        }
    }
    int max_words = 1;
    for (const auto & c : chunks) max_words = std::max(max_words, static_cast<int>(c.words.size()));

    // --- mentions
    std::vector<Ment> ms(N);
    for (int i = 0; i < N; ++i) {
        const Entity & e = entities[i];
        Ment & m = ms[i];
        m.chunk = mapper[e.start].first;
        m.ws = mapper[e.start].second;
        m.we = mapper[e.end].second;
        m.in_quote = tokens[e.start].in_quote || tokens[e.end].in_quote;
        const size_t us = e.cat.find('_');
        m.proper = e.cat.substr(0, us);
        m.text_lower = py_lower(e.text);
        m.single_word = e.text.find(' ') == std::string::npos;
        if (m.in_quote)
            for (size_t q = 0; q < quotes.size(); ++q)
                if (e.start >= quotes[q].first && e.start <= quotes[q].second)
                    m.quote_mention = q < attributions.size() && attributions[q] >= 0 ? attributions[q] : kNone;
    }

    // --- word representations: mean of wordpieces of BERT's last layer, [chunk * max_words + w][H]
    std::vector<float> embeds(static_cast<size_t>(chunks.size()) * max_words * H, 0.f);
    std::vector<int> real_rows;   // flattened rows that hold a word
    for (size_t c = 0; c < chunks.size(); ++c) {
        std::vector<int> ids;
        std::vector<int> counts;
        for (int t : chunks[c].words) {
            std::vector<std::string> p;
            if (t == -1) p = {"[CLS]"};
            else if (t == -2) p = {"[SEP]"};
            else p = pieces[t];
            for (const auto & s : p) ids.push_back(wp.id(s));
            counts.push_back(static_cast<int>(p.size()));
        }
        const auto last = bert_.encode(ids, 1)[0];
        size_t pos = 0;
        for (size_t w = 0; w < counts.size(); ++w) {
            const size_t row = c * max_words + w;
            float * dst = &embeds[row * H];
            const float scale = counts[w] ? 1.0f / counts[w] : 0.f;
            for (int p = 0; p < counts[w]; ++p)
                for (int k = 0; k < H; ++k) dst[k] += last[(pos + p) * H + k] * scale;
            pos += counts[w];
            real_rows.push_back(static_cast<int>(row));
        }
    }
    // attention weights exp(attention2(tanh(attention1(e)))) per word; padded rows are never used
    std::vector<float> att(embeds.size() / H, 0.f);
    {
        Graph g(m_.backend());
        ggml_context * ctx = g.ctx();
        const int W = static_cast<int>(real_rows.size());
        ggml_tensor * X = g.input(GGML_TYPE_F32, H, W);
        ggml_tensor * h = ggml_mul_mat(ctx, m_.tensor("attention1.weight"), X);
        ggml_mul_mat_set_prec(h, GGML_PREC_F32);
        h = ggml_tanh(ctx, ggml_add(ctx, h, m_.tensor("attention1.bias")));
        ggml_tensor * a = ggml_mul_mat(ctx, m_.tensor("attention2.weight"), h);
        ggml_mul_mat_set_prec(a, GGML_PREC_F32);
        g.output(a);
        g.allocate();
        std::vector<float> x(static_cast<size_t>(W) * H);
        for (int w = 0; w < W; ++w) std::copy_n(&embeds[static_cast<size_t>(real_rows[w]) * H], H, &x[static_cast<size_t>(w) * H]);
        Graph::set(X, x.data());
        g.run();
        const auto r = Graph::get(a);
        for (int w = 0; w < W; ++w) att[real_rows[w]] = std::exp(r[w] + att2_b_);
    }
    // span representations [start ; end ; attended ; width ; quote]
    const int R = 3 * H + 40;
    std::vector<float> reps(static_cast<size_t>(N) * R, 0.f);
    const size_t n_rows = att.size();
    for (int i = 0; i < N; ++i) {
        const Ment & m = ms[i];
        float * r = &reps[static_cast<size_t>(i) * R];
        const size_t s = std::min(n_rows - 1, static_cast<size_t>(m.chunk) * max_words + m.ws);
        const size_t e = std::min(n_rows - 1, static_cast<size_t>(m.chunk) * max_words + m.we);
        std::copy_n(&embeds[s * H], H, r);
        std::copy_n(&embeds[e * H], H, r + H);
        const int w0 = m.ws, w1 = std::min(m.we, max_words - 1);
        float denom = 0.f;
        for (int w = 0; w < max_words; ++w) {
            const bool in = w >= w0 && w <= w1;
            denom += 1e-8f + (in ? att[static_cast<size_t>(m.chunk) * max_words + w] : 0.f);
        }
        for (int w = w0; w <= w1; ++w) {
            const size_t row = static_cast<size_t>(m.chunk) * max_words + w;
            const float v = att[row] / denom;
            for (int k = 0; k < H; ++k) r[2 * H + k] += v * embeds[row * H + k];
        }
        std::copy_n(&width_emb_[static_cast<size_t>(width_bucket(m.we - m.ws)) * 20], 20, r + 3 * H);
        std::copy_n(&quote_emb_[static_cast<size_t>(m.in_quote ? 1 : 0) * 20], 20, r + 3 * H + 20);
    }

    // --- candidate antecedents per pronoun (independent of the assignments)
    struct Cand {
        int j, dist, nest1, nest2;
    };
    std::vector<std::vector<Cand>> cands(N);
    std::vector<int> pair_i, pair_j;
    for (int i = 1; i < N; ++i) {
        if (refs[i] != -1 || ms[i].proper != "PRON") continue;
        const int first = std::max(0, i - kMaxPrevious);
        std::vector<int> idx, dist;
        if (!ms[i].in_quote) {
            for (int k = first; k < i; ++k)
                if (!ms[k].in_quote) idx.push_back(k);
            for (size_t k = 0; k < idx.size(); ++k) dist.push_back(static_cast<int>(idx.size() - 1 - k));
        } else {
            for (int k = first; k < i; ++k) idx.push_back(k);
            for (size_t k = 0; k < idx.size(); ++k) dist.push_back(static_cast<int>(idx.size() - 1 - k));
            int kk = 1;
            for (int k = i + 1; k < std::min(i + 1 + kTop, N); ++k)
                if (!ms[k].in_quote) {
                    idx.push_back(k);
                    dist.push_back(-kk++);
                }
        }
        const size_t from = idx.size() > kMaxPrevious ? idx.size() - kMaxPrevious : 0;
        for (size_t k = from; k < idx.size(); ++k) {
            const int j = idx[k];
            const Entity & a = entities[i], & b = entities[j];
            cands[i].push_back({j, distance_bucket(dist[k]),
                                a.start >= b.start && a.end < b.end ? 1 : 0, b.start >= a.start && b.end < a.end ? 1 : 0});
            pair_i.push_back(i);
            pair_j.push_back(j);
        }
    }
    // --- backend: unary scores and the assignment-independent part of mention_mention1 for all pairs
    const int P = static_cast<int>(pair_i.size());
    const int D1 = static_cast<int>(m1_b_.size());
    std::vector<float> unary, pre;
    {
        Graph g(m_.backend());
        ggml_context * ctx = g.ctx();
        auto mm = [&](ggml_tensor * a, ggml_tensor * b) {
            ggml_tensor * r = ggml_mul_mat(ctx, a, b);
            ggml_mul_mat_set_prec(r, GGML_PREC_F32);
            return r;
        };
        auto W = [&](const std::string & n) { return m_.tensor(n); };
        ggml_tensor * SR = g.input(GGML_TYPE_F32, R, N);
        ggml_tensor * u = ggml_tanh(ctx, ggml_add(ctx, mm(W("unary1.weight"), SR), W("unary1.bias")));
        u = ggml_tanh(ctx, ggml_add(ctx, mm(W("unary2.weight"), u), W("unary2.bias")));
        u = mm(W("unary3.weight"), u);
        g.output(u);
        ggml_tensor *I = nullptr, *J = nullptr, *pr = nullptr;
        if (P > 0) {
            I = g.input(GGML_TYPE_I32, P);
            J = g.input(GGML_TYPE_I32, P);
            ggml_tensor * a = ggml_get_rows(ctx, SR, I);
            ggml_tensor * b = ggml_get_rows(ctx, SR, J);
            pr = ggml_add(ctx, ggml_add(ctx, mm(W("mention_mention1.weight_a"), a), mm(W("mention_mention1.weight_b"), b)),
                          mm(W("mention_mention1.weight_ab"), ggml_mul(ctx, a, b)));
            g.output(pr);
        }
        g.allocate();
        Graph::set(SR, reps.data());
        if (P > 0) {
            Graph::set(I, pair_i.data());
            Graph::set(J, pair_j.data());
        }
        g.run();
        unary = Graph::get(u);
        for (auto & x : unary) x += u3_b_;
        if (P > 0) pre = Graph::get(pr);
    }
    // small embedding terms of mention_mention1: columns [distance 20 | nested1 20 | nested2 20 | speaker 20]
    auto rest_term = [&](int block, const std::vector<float> & table, int row) {
        std::vector<float> y(D1);
        matvec(m1_rest_, D1, 80, block * 20, &table[static_cast<size_t>(row) * 20], 20, y.data());
        return y;
    };
    std::vector<std::vector<float>> rd(43), rn1(2), rn2(2), rs(3);
    for (int b = 0; b < 43; ++b) rd[b] = rest_term(0, dist_emb_, b);
    for (int v = 0; v < 2; ++v) {
        rn1[v] = rest_term(1, nested_emb_, v);
        rn2[v] = rest_term(2, nested_emb_, v);
    }
    for (int s = 0; s < 3; ++s) rs[s] = rest_term(3, speaker_emb_, s);

    // conflicting pronouns per gender category
    const auto & cats = gender_categories();
    std::vector<std::unordered_set<std::string>> conflicting(cats.size());
    for (size_t c = 0; c < cats.size(); ++c)
        for (size_t a = 0; a < cats.size(); ++a)
            if (a != c)
                for (const auto & w : cats[a])
                    if (std::find(cats[c].begin(), cats[c].end(), w) == cats[c].end()) conflicting[c].insert(w);
    auto compatible = [&](int eid, const Ment & m) {
        if (m.single_word) {
            auto it = ref_genders.find(eid);
            if (it != ref_genders.end() && it->second.argmax >= 0 && conflicting[it->second.argmax].count(m.text_lower))
                return false;
        }
        return true;
    };

    // --- forward(): greedy assignment, mentions outside quotes first
    int curid = -1;
    for (int r : refs) curid = std::max(curid, r);
    ++curid;
    std::vector<int> as(N, kNone);
    std::vector<int> pair_base(N, 0);
    {
        int p = 0;
        for (int i = 0; i < N; ++i) {
            pair_base[i] = p;
            p += static_cast<int>(cands[i].size());
        }
    }
    std::vector<float> h1(D1), h2(m2_b_.size());
    for (bool in_quote_pass : {false, true}) {
        for (int i = 0; i < N; ++i) {
            const Ment & m = ms[i];
            if (m.in_quote != in_quote_pass) continue;
            if (refs[i] != -1) {
                as[i] = refs[i];
                continue;
            }
            if (m.proper != "PRON" || i == 0 || cands[i].empty()) {
                as[i] = curid++;
                continue;
            }
            if (m.quote_mention != kNone && as[m.quote_mention] != kNone && m.in_quote &&
                (m.text_lower == "i" || m.text_lower == "me" || m.text_lower == "my" || m.text_lower == "myself")) {
                as[i] = as[m.quote_mention];
                continue;
            }
            const auto & cs = cands[i];
            std::vector<float> preds(cs.size());
            for (size_t k = 0; k < cs.size(); ++k) {
                int speaker = 2;
                if (m.in_quote && m.quote_mention != kNone) speaker = as[cs[k].j] == as[m.quote_mention] ? 1 : 0;
                const float * pk = &pre[static_cast<size_t>(pair_base[i] + k) * D1];
                for (int r = 0; r < D1; ++r)
                    h1[r] = std::tanh(pk[r] + m1_b_[r] + rd[cs[k].dist][r] + rn1[cs[k].nest1][r] + rn2[cs[k].nest2][r] +
                                      rs[speaker][r]);
                for (size_t r = 0; r < h2.size(); ++r) {
                    float acc = m2_b_[r];
                    for (int c = 0; c < D1; ++c) acc += m2_w_[r * D1 + c] * h1[c];
                    h2[r] = std::tanh(acc);
                }
                float s = m3_b_;
                for (size_t c = 0; c < h2.size(); ++c) s += m3_w_[c] * h2[c];
                preds[k] = s + unary[i] + unary[cs[k].j];
            }
            std::vector<size_t> order(cs.size());
            std::iota(order.begin(), order.end(), 0);
            std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return preds[a] > preds[b]; });
            int assignment = kNone;
            for (size_t k : order) {
                if (preds[k] > 0) {
                    const int cand = as[cs[k].j];
                    if (compatible(cand, m)) {
                        assignment = cand;
                        break;
                    }
                } else {
                    assignment = curid++;
                    break;
                }
            }
            if (assignment == kNone) assignment = curid++;
            as[i] = assignment;
        }
    }
    // NameCoref.cluster_noms: identical nominal mentions share the first one's id
    std::unordered_map<std::string, int> names;
    std::unordered_map<int, int> mapper_ids;
    for (int i = 0; i < N; ++i) {
        if (ms[i].proper != "NOM") continue;
        auto it = names.find(ms[i].text_lower);
        if (it == names.end()) names.emplace(ms[i].text_lower, as[i]);
        else mapper_ids[as[i]] = it->second;
    }
    for (int & a : as) {
        auto it = mapper_ids.find(a);
        if (it != mapper_ids.end()) a = it->second;
    }
    return as;
}

}  // namespace rm::booknlp
