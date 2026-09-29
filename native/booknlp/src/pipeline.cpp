#include "pipeline.h"

#include "coref.h"
#include "quotes.h"

#include <chrono>

namespace rm::booknlp {

struct Pipeline::Impl {
    GgufModel ent_model, quote_model, coref_model;
    EntityTagger tagger;
    QuoteAttribution attrib;
    Coref coref;
    NameCoref names;
    GenderPriors priors;
    Impl(const std::string & dir, const Backend & be)
        : ent_model(dir + "/bnlp-entities.gguf", be),
          quote_model(dir + "/bnlp-quote.gguf", be),
          coref_model(dir + "/bnlp-coref.gguf", be),
          tagger(ent_model),
          attrib(quote_model),
          coref(coref_model),
          names(ent_model.str("bnlp.aliases")),
          priors(ent_model.str("bnlp.gender_terms")) {}
};

Pipeline::Pipeline(const std::string & models_dir, const Backend & backend)
    : impl_(std::make_unique<Impl>(models_dir, backend)) {}

Pipeline::~Pipeline() = default;

Result Pipeline::run(const std::string & chapter_text) const {
    Result r;
    auto t = std::chrono::steady_clock::now();
    auto lap = [&](const char * name) {
        const auto now = std::chrono::steady_clock::now();
        r.seconds[name] = std::chrono::duration<double>(now - t).count();
        t = now;
    };
    r.tokens = make_tokens(chapter_text);
    lap("tokens");
    r.entities = impl_->tagger.tag(r.tokens);
    lap("entities");
    r.quotes = tag_quotes(r.tokens);
    r.attributions = impl_->attrib.tag(r.quotes, r.entities, r.tokens);
    lap("attribution");

    std::vector<int> in_quotes;
    for (const auto & e : r.entities) in_quotes.push_back(r.tokens[e.start].in_quote || r.tokens[e.end].in_quote ? 1 : 0);
    r.refs = impl_->names.cluster_narrator(r.entities, in_quotes);
    r.refs = impl_->names.cluster_identical_propers(r.entities, r.refs);
    r.refs = impl_->names.cluster_only_nouns(r.entities, r.refs, r.tokens);
    r.genders_em = infer_genders(impl_->priors, r.tokens, r.entities, r.refs);
    lap("names_gender");

    r.assignments = impl_->coref.tag(r.tokens, r.entities, r.refs, r.genders_em, r.attributions, r.quotes);
    r.genders = r.genders_em;
    update_gender_from_coref(r.genders, r.entities, r.assignments);
    lap("coref");

    // get_syntax(): characters with more than one PER mention, most frequent first
    OrderedMap<int, int> counts;
    OrderedMap<int, OrderedMap<std::string, int>> proper;
    for (size_t i = 0; i < r.entities.size(); ++i) {
        const auto & e = r.entities[i];
        const size_t us = e.cat.find('_');
        if (e.cat.substr(us + 1) != "PER") continue;
        const int c = r.assignments[i];
        counts[c] += 1;
        auto & p = proper[c];
        if (e.cat.substr(0, us) == "PROP") p[e.text] += 1;
    }
    for (const auto & [c, n] : most_common(counts)) {
        if (n <= 1) continue;
        Character ch{c, n, most_common(proper.at(c)), nullptr};
        auto g = r.genders.find(c);
        if (g != r.genders.end()) ch.g = &g->second;
        r.characters.push_back(std::move(ch));
    }
    // booknlp_runner.py: speaker name = most frequent proper name; female if argmax is she/her
    std::map<int, std::string> char_name;
    OrderedMap<std::string, std::string> genders_by_name;
    for (const auto & ch : r.characters) {
        if (ch.proper.empty()) continue;
        char_name[ch.id] = ch.proper.front().first;
        genders_by_name[ch.proper.front().first] = ch.g && ch.g->argmax == 1 ? "female" : "male";
    }
    for (const auto & [n, g] : genders_by_name) r.speaker_genders.emplace_back(n, g);
    for (size_t q = 0; q < r.quotes.size(); ++q) {
        std::string text;
        for (int k = r.quotes[q].first; k <= r.quotes[q].second; ++k) text += (text.empty() ? "" : " ") + r.tokens[k].text;
        std::string who = "Unknown";
        if (q < r.attributions.size() && r.attributions[q] >= 0) {
            auto it = char_name.find(r.assignments[r.attributions[q]]);
            if (it != char_name.end()) who = it->second;
        }
        r.speaker_quotes.emplace_back(text, who);
    }
    return r;
}

}  // namespace rm::booknlp
