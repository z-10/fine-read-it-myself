// ModernBERT encoder (HF ModernBertModel: pre-LN, GeGLU, RoPE, global attention every 3rd layer and
// +-64 token sliding-window attention otherwise), weights under "bert." as in ModernQA.
#pragma once

#include "bpe.h"
#include "ggml_model.h"

#include <vector>

namespace rm::booknlp {

class ModernBert {
public:
    struct Inputs {
        ggml_tensor * ids = nullptr, * pos = nullptr, * local_mask = nullptr;
        int T = 0;
    };
    explicit ModernBert(const GgufModel & m);
    // adds the encoder to `g`; returns last_hidden_state [H, T]
    ggml_tensor * build(Graph & g, int T, Inputs & in) const;
    // after g.allocate()
    void set_inputs(const Inputs & in, const std::vector<int> & ids) const;
    int n_embd() const { return n_embd_; }
    const Bpe & tokenizer() const { return bpe_; }

private:
    const GgufModel & m_;
    int n_layer_, n_embd_, n_head_, global_every_, window_;
    float eps_, theta_global_, theta_local_;
    Bpe bpe_;
};

}  // namespace rm::booknlp
