#include "bpe.h"

#include "unicode.h"

#include <climits>
#include <nlohmann/json.hpp>
#include <stdexcept>

namespace rm::booknlp {

Bpe::Bpe(const std::string & tokenizer_json) {
    const auto j = nlohmann::json::parse(tokenizer_json);
    for (const auto & [tok, id] : j["model"]["vocab"].items()) vocab_[tok] = id.get<int>();
    int rank = 0;
    for (const auto & m : j["model"]["merges"]) {
        if (m.is_string()) ranks_[m.get<std::string>()] = rank++;
        else ranks_[m[0].get<std::string>() + " " + m[1].get<std::string>()] = rank++;
    }
    for (const auto & a : j["added_tokens"]) {
        vocab_[a["content"].get<std::string>()] = a["id"].get<int>();
    }
    cls_ = vocab_.at("[CLS]");
    sep_ = vocab_.at("[SEP]");
}

// GPT-2 BPE on one byte-encoded word: merge the lowest-ranked adjacent pair until none applies
std::vector<std::string> Bpe::bpe(const std::string & word) const {
    std::vector<std::string> sym;
    for (size_t i = 0; i < word.size();) {
        size_t n = 1;
        const unsigned char c = static_cast<unsigned char>(word[i]);
        if (c >= 0xF0) n = 4; else if (c >= 0xE0) n = 3; else if (c >= 0xC0) n = 2;
        sym.push_back(word.substr(i, n));
        i += n;
    }
    while (sym.size() > 1) {
        int best = INT_MAX;
        std::string bl, br;
        for (size_t i = 0; i + 1 < sym.size(); ++i) {
            auto it = ranks_.find(sym[i] + " " + sym[i + 1]);
            if (it != ranks_.end() && it->second < best) {
                best = it->second;
                bl = sym[i];
                br = sym[i + 1];
            }
        }
        if (best == INT_MAX) break;
        std::vector<std::string> out;
        for (size_t i = 0; i < sym.size();) {
            if (i + 1 < sym.size() && sym[i] == bl && sym[i + 1] == br) {
                out.push_back(bl + br);
                i += 2;
            } else {
                out.push_back(sym[i++]);
            }
        }
        sym.swap(out);
    }
    return sym;
}

BpeEncoding Bpe::encode(const std::string & text) const {
    // byte offset -> codepoint index
    std::vector<int> char_of_byte(text.size() + 1);
    int ci = 0;
    for (size_t i = 0; i < text.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        if (i > 0 && (c & 0xC0) != 0x80) ++ci;
        char_of_byte[i] = ci;
    }
    char_of_byte[text.size()] = text.empty() ? 0 : ci + 1;

    BpeEncoding enc;
    enc.ids.push_back(cls_);
    enc.offsets.emplace_back(0, 0);
    enc.special.push_back(1);
    static const std::vector<std::string> kRegex = {
        "'s|'t|'re|'ve|'m|'ll|'d| ?\\p{L}+| ?\\p{N}+| ?[^\\s\\p{L}\\p{N}]+|\\s+(?!\\S)"};
    size_t pos = 0;
    for (const auto & word : unicode_regex_split(text, kRegex, false)) {
        std::string encoded;
        for (unsigned char b : word) encoded += unicode_byte_to_utf8(b);
        size_t b0 = pos;
        for (const auto & piece : bpe(encoded)) {
            size_t nbytes = 0;   // each byte-encoded codepoint stands for one input byte
            for (size_t i = 0; i < piece.size(); ++i)
                if ((static_cast<unsigned char>(piece[i]) & 0xC0) != 0x80) ++nbytes;
            auto it = vocab_.find(piece);
            if (it == vocab_.end()) throw std::runtime_error("BPE piece not in vocab");
            enc.ids.push_back(it->second);
            const size_t b1 = b0 + nbytes;
            enc.offsets.emplace_back(char_of_byte[b0], b1 > b0 ? char_of_byte[b1 - 1] + 1 : char_of_byte[b0]);
            enc.special.push_back(0);
            b0 = b1;
        }
        pos += word.size();
    }
    enc.ids.push_back(sep_);
    enc.offsets.emplace_back(0, 0);
    enc.special.push_back(1);
    return enc;
}

}  // namespace rm::booknlp
