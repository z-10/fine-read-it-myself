// Byte-level BPE (HF tokenizers, GPT-2 pre-tokenizer) for ModernBERT, with character offsets.
// Added tokens other than [CLS]/[SEP] and the NFC normalizer are not applied (the context text
// is rebuilt from single-space-joined tokens, so neither occurs in practice).
#pragma once

#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace rm::booknlp {

struct BpeEncoding {
    std::vector<int> ids;                          // incl. [CLS] ... [SEP]
    std::vector<std::pair<int, int>> offsets;      // codepoint [start, end), (0, 0) for specials
    std::vector<char> special;
};

class Bpe {
public:
    explicit Bpe(const std::string & tokenizer_json);
    BpeEncoding encode(const std::string & utf8_text) const;

private:
    std::vector<std::string> bpe(const std::string & word) const;
    std::unordered_map<std::string, int> vocab_;
    std::unordered_map<std::string, int> ranks_;   // "left right" -> merge rank
    int cls_ = 0, sep_ = 0;
};

}  // namespace rm::booknlp
