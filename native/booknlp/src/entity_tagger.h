// ModernBookNLP entity tagger (LitBankEntityTagger, entities only): BERT-6L, last 4 layers averaged per
// word, three stacked BiLSTM + CRF layers for nested entities. Batching and padding follow
// layered_reader.get_batches exactly, because the unpacked BiLSTMs see the padding.
#pragma once

#include "bert.h"
#include "frontend.h"

#include <string>
#include <vector>

namespace rm::booknlp {

struct Entity {
    int start, end;          // token ids, inclusive
    std::string cat;         // e.g. "PROP_PER"
    std::string text;
    bool operator<(const Entity & o) const {
        if (start != o.start) return start < o.start;
        if (end != o.end) return end < o.end;
        if (cat != o.cat) return cat < o.cat;
        return text < o.text;
    }
};

struct Lstm {                // one PyTorch nn.LSTM layer, bidirectional, hidden H
    int in = 0, H = 0;
    std::vector<float> w_ih[2], w_hh[2], b[2];   // [dir]: w_ih [4H, in], w_hh [4H, H], b = b_ih + b_hh
};

class EntityTagger {
public:
    explicit EntityTagger(const GgufModel & m);
    std::vector<Entity> tag(const std::vector<Token> & tokens) const;

private:
    struct Chunk {
        std::vector<std::vector<std::string>> words;   // wordpieces per word, incl. [CLS] / [SEP]
        std::vector<int> toks;                         // token indices of the real words
    };
    // [B][nl][out] = LSTM over padded inputs [B][nl][in]; input projection of layer 1 on the backend
    std::vector<std::vector<float>> run_lstm(const Lstm & l, const std::string & prefix,
                                             const std::vector<std::vector<float>> & x, int nl, bool on_backend) const;
    std::vector<int> viterbi(const std::vector<float> & logits, int nl, int len) const;
    void fix(std::vector<int> & seq) const;

    const GgufModel & m_;
    Bert bert_;
    Lstm lstm_[3];
    std::vector<float> h2t_w_[3], h2t_b_[3], trans_;
    std::vector<std::string> rev_;     // tag index -> name ("O" for start/stop)
    int n_tags_ = 0;                   // incl. start/stop
    int tag_O_ = 0;
    std::vector<std::string> labels_;  // per tag: label part after "B-"/"I-", "" for O
    std::vector<char> kind_;           // 'B', 'I' or 'O'
    int tag_id(const std::string & name) const;
};

}  // namespace rm::booknlp
