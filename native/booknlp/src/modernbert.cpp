#include "modernbert.h"

#include <cmath>
#include <cstdlib>
#include <string>

namespace rm::booknlp {

ModernBert::ModernBert(const GgufModel & m)
    : m_(m),
      n_layer_(static_cast<int>(m.u32("bnlp.mbert.n_layer"))),
      n_embd_(static_cast<int>(m.u32("bnlp.mbert.n_embd"))),
      n_head_(static_cast<int>(m.u32("bnlp.mbert.n_head"))),
      global_every_(static_cast<int>(m.u32("bnlp.mbert.global_every"))),
      window_(static_cast<int>(m.u32("bnlp.mbert.local_window"))),
      eps_(m.f32("bnlp.mbert.norm_eps")),
      theta_global_(m.f32("bnlp.mbert.global_rope_theta")),
      theta_local_(m.f32("bnlp.mbert.local_rope_theta")),
      bpe_(m.str("bnlp.tokenizer_json")) {}

ggml_tensor * ModernBert::build(Graph & g, int T, Inputs & in) const {
    const int H = n_embd_, nh = n_head_, d = H / nh;
    ggml_context * ctx = g.ctx();
    auto W = [&](const std::string & n) { return m_.tensor(n); };
    auto mm = [&](ggml_tensor * a, ggml_tensor * b) {
        ggml_tensor * r = ggml_mul_mat(ctx, a, b);
        ggml_mul_mat_set_prec(r, GGML_PREC_F32);
        return r;
    };
    auto norm = [&](ggml_tensor * x, const std::string & w) { return ggml_mul(ctx, ggml_norm(ctx, x, eps_), W(w)); };

    in.T = T;
    ggml_tensor * inp_ids = in.ids = g.input(GGML_TYPE_I32, T);
    ggml_tensor * inp_pos = in.pos = g.input(GGML_TYPE_I32, T);
    ggml_tensor * local_mask = in.local_mask = g.input(GGML_TYPE_F16, T, T);
    ggml_tensor * x = norm(ggml_get_rows(ctx, W("bert.embeddings.tok_embeddings.weight"), inp_ids),
                           "bert.embeddings.norm.weight");
    const float scale = 1.0f / std::sqrt(static_cast<float>(d));
    for (int il = 0; il < n_layer_; ++il) {
        const std::string p = "bert.layers." + std::to_string(il);
        const bool global = il % global_every_ == 0;
        ggml_tensor * h = il == 0 ? x : norm(x, p + ".attn_norm.weight");
        ggml_tensor * qkv = mm(W(p + ".attn.Wqkv.weight"), h);                  // [3H, T]
        auto part = [&](int k) {
            return ggml_view_3d(ctx, qkv, d, nh, T, d * ggml_element_size(qkv), qkv->nb[1],
                                static_cast<size_t>(k) * H * ggml_element_size(qkv));
        };
        const float theta = global ? theta_global_ : theta_local_;
        auto rope = [&](ggml_tensor * t) {
            return ggml_rope_ext(ctx, ggml_cont(ctx, t), inp_pos, nullptr, d, GGML_ROPE_TYPE_NEOX, 0, theta, 1.0f,
                                 0.0f, 1.0f, 0.0f, 0.0f);
        };
        ggml_tensor * q = ggml_permute(ctx, rope(part(0)), 0, 2, 1, 3);       // [d, T, nh]
        ggml_tensor * k = ggml_permute(ctx, rope(part(1)), 0, 2, 1, 3);
        ggml_tensor * v = ggml_permute(ctx, ggml_cont(ctx, part(2)), 0, 2, 1, 3);
        ggml_tensor * att = ggml_flash_attn_ext(ctx, q, k, v, global ? nullptr : local_mask, scale, 0.0f, 0.0f);
        ggml_flash_attn_ext_set_prec(att, GGML_PREC_F32);
        att = mm(W(p + ".attn.Wo.weight"), ggml_reshape_2d(ctx, att, H, T));
        x = ggml_add(ctx, x, att);
        ggml_tensor * m = mm(W(p + ".mlp.Wi.weight"), norm(x, p + ".mlp_norm.weight"));   // [2F, T]
        m = mm(W(p + ".mlp.Wo.weight"), ggml_geglu_erf(ctx, m));
        x = ggml_add(ctx, x, m);
    }
    return norm(x, "bert.final_norm.weight");
}

void ModernBert::set_inputs(const Inputs & in, const std::vector<int> & ids) const {
    const int T = in.T;
    std::vector<int> pos(T);
    for (int i = 0; i < T; ++i) pos[i] = i;
    std::vector<ggml_fp16_t> mask(static_cast<size_t>(T) * T);
    const ggml_fp16_t zero = ggml_fp32_to_fp16(0.0f), ninf = ggml_fp32_to_fp16(-INFINITY);
    for (int qi = 0; qi < T; ++qi)
        for (int ki = 0; ki < T; ++ki)
            mask[static_cast<size_t>(qi) * T + ki] = std::abs(qi - ki) <= window_ / 2 ? zero : ninf;
    Graph::set(in.ids, ids.data());
    Graph::set(in.pos, pos.data());
    Graph::set(in.local_mask, mask.data());
}

}  // namespace rm::booknlp
