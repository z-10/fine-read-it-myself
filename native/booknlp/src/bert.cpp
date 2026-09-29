#include "bert.h"

#include "tokenizer.h"

#include <cmath>
#include <stdexcept>

namespace rm::booknlp {

WordPiece::WordPiece(const std::vector<std::string> & vocab) {
    for (size_t i = 0; i < vocab.size(); ++i) ids_.emplace(vocab[i], static_cast<int>(i));
    unk_ = id("[UNK]");
}

int WordPiece::id(const std::string & token) const {
    auto it = ids_.find(token);
    if (it == ids_.end()) throw std::runtime_error("token not in vocab: " + token);
    return it->second;
}

std::vector<std::string> WordPiece::tokenize(const std::string & text) const {
    std::vector<std::string> out;
    // PreTrainedTokenizer.tokenize: split off the added special token "[CAP]", wordpiece the rest
    std::vector<std::string> pieces;
    size_t a = 0;
    for (size_t b; (b = text.find("[CAP]", a)) != std::string::npos; a = b + 5) {
        pieces.push_back(text.substr(a, b - a));
        pieces.push_back("[CAP]");
    }
    pieces.push_back(text.substr(a));
    for (const auto & piece : pieces) {
        if (piece == "[CAP]") {
            out.push_back(piece);
            continue;
        }
        // whitespace_tokenize: str.split()
        const std::u32string u = utf8_to_u32(piece);
        size_t i = 0;
        while (i < u.size()) {
            while (i < u.size() && py_isspace(u[i])) ++i;
            size_t j = i;
            while (j < u.size() && !py_isspace(u[j])) ++j;
            if (j > i) {
                const std::u32string word = u.substr(i, j - i);
                if (word.size() > 100) {
                    out.push_back("[UNK]");
                } else {
                    std::vector<std::string> sub;
                    bool bad = false;
                    for (size_t start = 0; start < word.size();) {
                        size_t end = word.size();
                        std::string cur;
                        for (; start < end; --end) {
                            std::string s = u32_to_utf8(word.substr(start, end - start));
                            if (start > 0) s = "##" + s;
                            if (ids_.count(s)) {
                                cur = s;
                                break;
                            }
                        }
                        if (cur.empty()) {
                            bad = true;
                            break;
                        }
                        sub.push_back(cur);
                        start = end;
                    }
                    if (bad) out.push_back("[UNK]");
                    else out.insert(out.end(), sub.begin(), sub.end());
                }
            }
            i = j;
        }
    }
    return out;
}

Bert::Bert(const GgufModel & m)
    : m_(m),
      n_layer_(static_cast<int>(m.u32("bnlp.bert.n_layer"))),
      n_embd_(static_cast<int>(m.u32("bnlp.bert.n_embd"))),
      n_head_(static_cast<int>(m.u32("bnlp.bert.n_head"))),
      eps_(m.f32("bnlp.bert.norm_eps")),
      wp_(m.strings("bnlp.vocab")) {}

std::vector<std::vector<float>> Bert::encode(const std::vector<int> & ids, int last_k) const {
    const int T = static_cast<int>(ids.size());
    const int H = n_embd_, nh = n_head_, d = H / nh;
    Graph g(m_.backend());
    ggml_context * ctx = g.ctx();
    auto W = [&](const std::string & n) { return m_.tensor(n); };
    auto mm = [&](ggml_tensor * a, ggml_tensor * b) {
        ggml_tensor * r = ggml_mul_mat(ctx, a, b);
        ggml_mul_mat_set_prec(r, GGML_PREC_F32);
        return r;
    };
    auto linear = [&](ggml_tensor * x, const std::string & p) {
        return ggml_add(ctx, mm(W(p + ".weight"), x), W(p + ".bias"));
    };
    auto layer_norm = [&](ggml_tensor * x, const std::string & p) {
        return ggml_add(ctx, ggml_mul(ctx, ggml_norm(ctx, x, eps_), W(p + ".weight")), W(p + ".bias"));
    };

    ggml_tensor * inp_ids = g.input(GGML_TYPE_I32, T);
    ggml_tensor * inp_pos = g.input(GGML_TYPE_I32, T);
    ggml_tensor * inp_type = g.input(GGML_TYPE_I32, T);
    ggml_tensor * x = ggml_get_rows(ctx, W("bert.embeddings.word_embeddings.weight"), inp_ids);
    x = ggml_add(ctx, x, ggml_get_rows(ctx, W("bert.embeddings.position_embeddings.weight"), inp_pos));
    x = ggml_add(ctx, x, ggml_get_rows(ctx, W("bert.embeddings.token_type_embeddings.weight"), inp_type));
    x = layer_norm(x, "bert.embeddings.LayerNorm");

    std::vector<ggml_tensor *> layers;
    const float scale = 1.0f / std::sqrt(static_cast<float>(d));
    for (int il = 0; il < n_layer_; ++il) {
        const std::string p = "bert.encoder.layer." + std::to_string(il);
        ggml_tensor * q = ggml_reshape_3d(ctx, linear(x, p + ".attention.self.query"), d, nh, T);
        ggml_tensor * k = ggml_reshape_3d(ctx, linear(x, p + ".attention.self.key"), d, nh, T);
        ggml_tensor * v = ggml_reshape_3d(ctx, linear(x, p + ".attention.self.value"), d, nh, T);
        q = ggml_permute(ctx, q, 0, 2, 1, 3);                            // [d, T, nh]
        k = ggml_permute(ctx, k, 0, 2, 1, 3);                            // [d, T, nh]
        ggml_tensor * vt = ggml_cont(ctx, ggml_permute(ctx, v, 1, 2, 0, 3));  // [T, d, nh]
        ggml_tensor * kq = ggml_soft_max_ext(ctx, mm(k, q), nullptr, scale, 0.0f);  // [Tk, Tq, nh]
        ggml_tensor * kqv = mm(vt, kq);                    // [d, Tq, nh]
        ggml_tensor * att = ggml_cont_2d(ctx, ggml_permute(ctx, kqv, 0, 2, 1, 3), H, T);
        att = linear(att, p + ".attention.output.dense");
        x = layer_norm(ggml_add(ctx, att, x), p + ".attention.output.LayerNorm");
        ggml_tensor * h = ggml_gelu_erf(ctx, linear(x, p + ".intermediate.dense"));
        h = linear(h, p + ".output.dense");
        x = layer_norm(ggml_add(ctx, h, x), p + ".output.LayerNorm");
        layers.push_back(x);
    }
    std::vector<ggml_tensor *> outs;
    for (int i = 0; i < last_k; ++i) {
        outs.push_back(layers[n_layer_ - 1 - i]);
        g.output(outs.back());
    }
    g.allocate();
    std::vector<int> pos(T), type(T, 0);
    for (int i = 0; i < T; ++i) pos[i] = i;
    Graph::set(inp_ids, ids.data());
    Graph::set(inp_pos, pos.data());
    Graph::set(inp_type, type.data());
    g.run();
    std::vector<std::vector<float>> res;
    for (ggml_tensor * t : outs) res.push_back(Graph::get(t));
    return res;
}

}  // namespace rm::booknlp
