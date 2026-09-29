// BERT encoder (HF BertModel, post-LayerNorm) as a ggml graph; weights named as in PyTorch.
#pragma once

#include "ggml_model.h"

#include <string>
#include <unordered_map>
#include <vector>

namespace rm::booknlp {

// WordPiece over pre-split words (BertTokenizer, do_basic_tokenize=False, do_lower_case=False,
// "[CAP]" added as a special token).
class WordPiece {
public:
    explicit WordPiece(const std::vector<std::string> & vocab);
    // tokens of one pre-tokenized word string (may contain "[CAP] " and spaces)
    std::vector<std::string> tokenize(const std::string & text) const;
    int id(const std::string & token) const;

private:
    std::unordered_map<std::string, int> ids_;
    int unk_ = 100;
};

class Bert {
public:
    Bert(const GgufModel & m);
    // Hidden states of the last `last_k` layers for one sequence, each [T][H] row-major, index 0 = last layer.
    std::vector<std::vector<float>> encode(const std::vector<int> & ids, int last_k) const;
    int n_embd() const { return n_embd_; }
    const WordPiece & vocab() const { return wp_; }

private:
    const GgufModel & m_;
    int n_layer_, n_embd_, n_head_;
    float eps_;
    WordPiece wp_;
};

}  // namespace rm::booknlp
