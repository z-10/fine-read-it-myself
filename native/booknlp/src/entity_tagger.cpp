#include "entity_tagger.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <numeric>
#include <set>
#include <thread>
#include <stdexcept>
#include <tuple>

namespace rm::booknlp {
namespace {

constexpr int kMaxSentence = 500;
constexpr int kBert = 768 * 4;

float sigmoid(float x) { return 1.0f / (1.0f + std::exp(-x)); }

std::vector<float> host(const GgufModel & m, const std::string & n) { return m.read(n); }

}  // namespace

int EntityTagger::tag_id(const std::string & name) const {
    for (int i = 0; i < static_cast<int>(rev_.size()) - 2; ++i)
        if (rev_[i] == name) return i;
    throw std::runtime_error("unknown tag " + name);
}

EntityTagger::EntityTagger(const GgufModel & m) : m_(m), bert_(m) {
    rev_ = m.strings("bnlp.tagset");
    rev_.push_back("O");
    rev_.push_back("O");
    n_tags_ = static_cast<int>(rev_.size());
    for (const auto & t : rev_) {
        kind_.push_back(t[0]);
        labels_.push_back(t.size() > 2 && t[1] == '-' ? t.substr(2) : "");
    }
    tag_O_ = tag_id("O");
    for (int k = 0; k < 3; ++k) {
        const std::string p = "lstm" + std::to_string(k + 1);
        Lstm & l = lstm_[k];
        for (int d = 0; d < 2; ++d) {
            const std::string s = d ? "_reverse" : "";
            l.w_ih[d] = host(m, p + ".weight_ih_l0" + s);
            l.w_hh[d] = host(m, p + ".weight_hh_l0" + s);
            auto bi = host(m, p + ".bias_ih_l0" + s), bh = host(m, p + ".bias_hh_l0" + s);
            l.b[d].resize(bi.size());
            for (size_t i = 0; i < bi.size(); ++i) l.b[d][i] = bi[i] + bh[i];
        }
        l.H = static_cast<int>(l.b[0].size() / 4);
        l.in = static_cast<int>(l.w_ih[0].size() / l.b[0].size());
        h2t_w_[k] = host(m, "hidden2tag" + std::to_string(k + 1) + ".weight");
        h2t_b_[k] = host(m, "hidden2tag" + std::to_string(k + 1) + ".bias");
    }
    trans_ = host(m, "crf.transitions");
}

std::vector<std::vector<float>> EntityTagger::run_lstm(const Lstm & l, const std::string & prefix,
                                                       const std::vector<std::vector<float>> & x, int nl,
                                                       bool on_backend) const {
    const int B = static_cast<int>(x.size()), H = l.H, G = 4 * H;
    // input projections [dir][b][t][4H]
    std::vector<std::vector<float>> proj[2];
    if (on_backend) {
        Graph g(m_.backend());
        ggml_tensor * X = g.input(GGML_TYPE_F32, l.in, static_cast<int64_t>(B) * nl);
        ggml_tensor * out[2];
        for (int d = 0; d < 2; ++d) {
            const std::string s = d ? "_reverse" : "";
            ggml_tensor * y = ggml_mul_mat(g.ctx(), m_.tensor(prefix + ".weight_ih_l0" + s), X);
            ggml_mul_mat_set_prec(y, GGML_PREC_F32);
            y = ggml_add(g.ctx(), y, m_.tensor(prefix + ".bias_ih_l0" + s));
            out[d] = ggml_add(g.ctx(), y, m_.tensor(prefix + ".bias_hh_l0" + s));
            g.output(out[d]);
        }
        g.allocate();
        std::vector<float> flat(static_cast<size_t>(B) * nl * l.in);
        for (int b = 0; b < B; ++b) std::copy(x[b].begin(), x[b].end(), flat.begin() + static_cast<size_t>(b) * nl * l.in);
        Graph::set(X, flat.data());
        g.run();
        for (int d = 0; d < 2; ++d) {
            const auto r = Graph::get(out[d]);
            proj[d].resize(B);
            for (int b = 0; b < B; ++b)
                proj[d][b].assign(r.begin() + static_cast<size_t>(b) * nl * G, r.begin() + static_cast<size_t>(b + 1) * nl * G);
        }
    } else {
        for (int d = 0; d < 2; ++d) {
            proj[d].assign(B, std::vector<float>(static_cast<size_t>(nl) * G));
            for (int b = 0; b < B; ++b)
                for (int t = 0; t < nl; ++t) {
                    const float * xi = &x[b][static_cast<size_t>(t) * l.in];
                    float * o = &proj[d][b][static_cast<size_t>(t) * G];
                    for (int r = 0; r < G; ++r) {
                        const float * w = &l.w_ih[d][static_cast<size_t>(r) * l.in];
                        float acc = 0.f;
                        for (int c = 0; c < l.in; ++c) acc += w[c] * xi[c];
                        o[r] = acc + l.b[d][r];
                    }
                }
        }
    }
    std::vector<std::vector<float>> y(B, std::vector<float>(static_cast<size_t>(nl) * 2 * H));
    auto recur = [&](int b, int d) {
        std::vector<float> h(H, 0.f), c(H, 0.f), gates(G);
        for (int s = 0; s < nl; ++s) {
            const int t = d ? nl - 1 - s : s;
            const float * p = &proj[d][b][static_cast<size_t>(t) * G];
            for (int r = 0; r < G; ++r) {
                const float * w = &l.w_hh[d][static_cast<size_t>(r) * H];
                float acc = p[r];
                for (int k = 0; k < H; ++k) acc += w[k] * h[k];
                gates[r] = acc;
            }
            for (int k = 0; k < H; ++k) {   // PyTorch gate order: i, f, g, o
                const float i = sigmoid(gates[k]), f = sigmoid(gates[H + k]);
                const float gg = std::tanh(gates[2 * H + k]), o = sigmoid(gates[3 * H + k]);
                c[k] = f * c[k] + i * gg;
                h[k] = o * std::tanh(c[k]);
            }
            std::copy(h.begin(), h.end(), y[b].begin() + static_cast<size_t>(t) * 2 * H + d * H);
        }
    };
    std::vector<std::thread> pool;
    for (int b = 0; b < B; ++b)
        for (int d = 0; d < 2; ++d) pool.emplace_back(recur, b, d);
    for (auto & th : pool) th.join();
    return y;
}

// crf.CRF.viterbi_decode for one sequence padded to nl; returns nl tags
std::vector<int> EntityTagger::viterbi(const std::vector<float> & logits, int nl, int len) const {
    const int n = n_tags_, start = n - 2, stop = n - 1;
    std::vector<float> vit(n, -10000.f), nxt(n);
    vit[start] = 0.f;
    std::vector<std::vector<int>> ptr(nl, std::vector<int>(n));
    int c_len = len;
    for (int t = 0; t < nl; ++t) {
        for (int j = 0; j < n; ++j) {
            int best = 0;
            float bv = vit[0] + trans_[static_cast<size_t>(j) * n];
            for (int i = 1; i < n; ++i) {
                const float v = vit[i] + trans_[static_cast<size_t>(j) * n + i];
                if (v > bv) { bv = v; best = i; }
            }
            nxt[j] = bv + logits[static_cast<size_t>(t) * n + j];
            ptr[t][j] = best;
        }
        if (c_len > 0) vit = nxt;
        if (c_len == 1)
            for (int j = 0; j < n; ++j) vit[j] += trans_[static_cast<size_t>(stop) * n + j];
        --c_len;
    }
    int idx = static_cast<int>(std::max_element(vit.begin(), vit.end()) - vit.begin());
    std::vector<int> path(nl);
    path[nl - 1] = idx;
    for (int t = nl - 1; t > 0; --t) {
        idx = ptr[t][idx];
        path[t - 1] = idx;
    }
    return path;
}

void EntityTagger::fix(std::vector<int> & seq) const {
    for (size_t idx = 0; idx < seq.size(); ++idx) {
        if (kind_[seq[idx]] != 'I') continue;
        const std::string & label = labels_[seq[idx]];
        bool flag = false;
        for (int i = static_cast<int>(idx) - 1; i >= 0; --i) {
            const char k = kind_[seq[i]];
            if (k == 'B' && labels_[seq[i]] == label) { flag = true; break; }
            if (k == 'O') break;
            if (labels_[seq[i]] != label) break;
        }
        if (!flag) seq[idx] = tag_id("B-" + label);
    }
}

std::vector<Entity> EntityTagger::tag(const std::vector<Token> & tokens) const {
    const WordPiece & wp = bert_.vocab();
    // --- sentences (entity_tagger.tag): split at sentence change or 500 wordpieces
    std::vector<std::vector<std::vector<std::string>>> sents(1);
    std::vector<std::vector<int>> o_sents(1);
    int last_sid = -1, length = 0;
    for (size_t i = 0; i < tokens.size(); ++i) {
        const Token & tok = tokens[i];
        const std::string wptok = first_is_upper(tok.text) ? "[CAP] " + py_lower(tok.text) : tok.text;
        auto toks = wp.tokenize(wptok);
        if (last_sid != -1 && (tok.sentence_id != last_sid || length + static_cast<int>(toks.size()) > kMaxSentence)) {
            sents.emplace_back();
            o_sents.emplace_back();
            length = 0;
        }
        sents.back().push_back(toks);
        o_sents.back().push_back(static_cast<int>(i));
        last_sid = tok.sentence_id;
        length += static_cast<int>(toks.size());
    }
    // --- pack sentences into chunks of < 500 wordpieces
    std::vector<Chunk> chunks;
    Chunk cur;
    cur.words.push_back({"[CLS]"});
    int cur_length = 0;
    for (size_t s = 0; s < sents.size(); ++s) {
        int sent_len = 0;
        for (const auto & w : sents[s]) sent_len += static_cast<int>(w.size());
        if (sent_len + cur_length >= kMaxSentence) {
            cur.words.push_back({"[SEP]"});
            chunks.push_back(std::move(cur));
            cur = Chunk();
            cur.words.push_back({"[CLS]"});
            cur_length = 0;
        }
        cur_length += sent_len;
        cur.words.insert(cur.words.end(), sents[s].begin(), sents[s].end());
        cur.toks.insert(cur.toks.end(), o_sents[s].begin(), o_sents[s].end());
    }
    if (cur.words.size() > 1) {
        cur.words.push_back({"[SEP]"});
        chunks.push_back(std::move(cur));
    }
    // --- get_batches: order by wordpiece count, batches of 32 then 12 / 6
    std::vector<std::vector<int>> ids(chunks.size());
    for (size_t c = 0; c < chunks.size(); ++c)
        for (const auto & w : chunks[c].words)
            for (const auto & p : w) ids[c].push_back(wp.id(p));
    std::vector<size_t> order(chunks.size());
    std::iota(order.begin(), order.end(), 0);
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return ids[a].size() < ids[b].size(); });

    std::vector<Entity> entities;
    size_t current_batch = 32;
    for (size_t i = 0; i < order.size();) {
        const std::vector<size_t> batch(order.begin() + i, order.begin() + std::min(order.size(), i + current_batch));
        size_t max_len = 0, max_label = 0;
        for (size_t c : batch) {
            max_len = std::max(max_len, ids[c].size());
            max_label = std::max(max_label, chunks[c].words.size());
        }
        const int nl = static_cast<int>(max_label) - 1;
        const int B = static_cast<int>(batch.size());
        // reduced = transforms @ cat(last 4 layers), without the [CLS] row, zero-padded to nl rows
        std::vector<std::vector<float>> reduced(B, std::vector<float>(static_cast<size_t>(nl) * kBert, 0.f));
        for (int b = 0; b < B; ++b) {
            const Chunk & ch = chunks[batch[b]];
            const auto layers = bert_.encode(ids[batch[b]], 4);
            size_t pos = 0;
            for (size_t w = 0; w < ch.words.size(); ++w) {
                const size_t n = ch.words[w].size();
                if (w > 0) {
                    float * row = &reduced[b][(w - 1) * kBert];
                    const float scale = 1.0f / static_cast<float>(n);
                    for (size_t p = pos; p < pos + n; ++p)
                        for (int L = 0; L < 4; ++L) {
                            const float * src = &layers[L][p * 768];
                            for (int k = 0; k < 768; ++k) row[L * 768 + k] += src[k] * scale;
                        }
                }
                pos += n;
            }
        }
        auto tag_space = [&](const std::vector<float> & out, int k) {
            std::vector<float> ts(static_cast<size_t>(nl) * n_tags_);
            const int in = 2 * lstm_[k].H;
            for (int t = 0; t < nl; ++t)
                for (int j = 0; j < n_tags_; ++j) {
                    float acc = h2t_b_[k][j];
                    const float * w = &h2t_w_[k][static_cast<size_t>(j) * in];
                    const float * x = &out[static_cast<size_t>(t) * in];
                    for (int c = 0; c < in; ++c) acc += w[c] * x[c];
                    ts[static_cast<size_t>(t) * n_tags_ + j] = acc;
                }
            return ts;
        };
        // layer transformation: merge tokens of the same entity (mean), remember merged positions
        struct Layer {
            std::vector<std::vector<int>> tags, missing;
            std::vector<int> lens;
            std::vector<std::vector<float>> next;   // merged inputs for the next layer
        };
        auto transform = [&](const std::vector<std::vector<float>> & out, const std::vector<int> & lens, int k) {
            Layer L;
            const int D = 2 * lstm_[k].H;
            for (int b = 0; b < B; ++b) {
                auto tags = viterbi(tag_space(out[b], k), nl, lens[b]);
                fix(tags);
                std::vector<std::vector<int>> groups;
                std::vector<int> missing;
                for (int t = 0; t < nl; ++t) {
                    if (kind_[tags[t]] != 'I') groups.push_back({t});
                    else groups.back().push_back(t);
                    if (t > 0 && kind_[tags[t]] == 'I') missing.push_back(t);
                }
                std::vector<float> nx(static_cast<size_t>(nl) * D, 0.f);
                for (size_t gi = 0; gi < groups.size(); ++gi) {
                    const float scale = 1.0f / static_cast<float>(groups[gi].size());
                    for (int t : groups[gi])
                        for (int c = 0; c < D; ++c) nx[gi * D + c] += out[b][static_cast<size_t>(t) * D + c] * scale;
                }
                L.tags.push_back(tags);
                L.missing.push_back(missing);
                L.lens.push_back(static_cast<int>(groups.size()));
                L.next.push_back(std::move(nx));
            }
            return L;
        };
        std::vector<int> lens0(B);
        for (int b = 0; b < B; ++b) lens0[b] = static_cast<int>(chunks[batch[b]].words.size()) - 2;
        const auto out1 = run_lstm(lstm_[0], "lstm1", reduced, nl, true);
        const Layer l1 = transform(out1, lens0, 0);
        const auto out2 = run_lstm(lstm_[1], "lstm2", l1.next, nl, false);
        const Layer l2 = transform(out2, l1.lens, 1);
        const auto out3 = run_lstm(lstm_[2], "lstm3", l2.next, nl, false);
        std::vector<std::vector<int>> tags1 = l1.tags, tags2 = l2.tags, tags3(B);
        for (int b = 0; b < B; ++b) {
            tags3[b] = viterbi(tag_space(out3[b], 2), nl, l2.lens[b]);
            fix(tags3[b]);
        }
        auto reinsert = [&](std::vector<int> & tags, const std::vector<int> & missing) {
            for (int m : missing) {
                const int prev = tags[m - 1];
                const std::string & lab = labels_[prev];
                tags.insert(tags.begin() + m, lab.empty() ? tag_O_ : tag_id("I-" + lab));
            }
        };
        for (int b = 0; b < B; ++b) {
            reinsert(tags3[b], l2.missing[b]);
            reinsert(tags3[b], l1.missing[b]);
            reinsert(tags2[b], l1.missing[b]);
            tags2[b].resize(tags1[b].size());
            tags3[b].resize(tags1[b].size());
        }
        // get_spans over the three layers, union per chunk
        for (int b = 0; b < B; ++b) {
            const Chunk & ch = chunks[batch[b]];
            const int length = static_cast<int>(ch.words.size()) - 2;
            std::set<std::tuple<std::string, int, int>> keys;
            for (const auto * tags : {&tags1[b], &tags2[b], &tags3[b]}) {
                const int n = std::min(length, static_cast<int>(tags->size()));
                for (int idx = 0; idx < n; ++idx) {
                    const int tg = (*tags)[idx];
                    if (kind_[tg] != 'B') continue;
                    int j = idx + 1;
                    while (j < n) {
                        const int tn = (*tags)[j];
                        if (kind_[tn] == 'B' || kind_[tn] == 'O') break;
                        if (labels_[tn] != labels_[tg]) break;
                        ++j;
                    }
                    keys.emplace(labels_[tg], idx, j);
                }
            }
            for (const auto & [label, s, e] : keys) {
                Entity ent{tokens[ch.toks[s]].token_id, tokens[ch.toks[e - 1]].token_id, label, ""};
                for (int t = s; t < e; ++t) ent.text += (t > s ? " " : "") + tokens[ch.toks[t]].text;
                entities.push_back(ent);
            }
        }
        i += current_batch;
        if (max_len > 100) current_batch = 12;
        if (max_len > 200) current_batch = 6;
    }
    std::sort(entities.begin(), entities.end());
    return entities;
}

}  // namespace rm::booknlp
