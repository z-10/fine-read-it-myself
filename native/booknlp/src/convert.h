// Builds bnlp-entities.gguf, bnlp-coref.gguf and bnlp-quote.gguf from the models as their publishers
// distribute them (downloaded on first run; the converted files are never redistributed):
//
//   <src>/entities.model              people.ischool.berkeley.edu/~dbamman/booknlp_models/entities_google_bert_uncased_L-6_H-768_A-12-v1.0.model
//   <src>/coref.model                 .../coref_google_bert_uncased_L-12_H-768_A-12-v1.0.model
//   <src>/modernqa.safetensors        huggingface.co/gasmichel/ModernBookNLP  ModernBERT_T2000.safetensors
//   <src>/bert6-vocab.txt, bert6-config.json     huggingface.co/google/bert_uncased_L-6_H-768_A-12
//   <src>/bert12-vocab.txt, bert12-config.json   huggingface.co/google/bert_uncased_L-12_H-768_A-12
//   <src>/modernbert-tokenizer.json, modernbert-config.json   huggingface.co/answerdotai/ModernBERT-large
//   <src>/aliases.txt, gender_terms.txt, entity_cat.tagset   github.com/booknlp/booknlp  booknlp/english/data
//
// Same output as tools/convert_booknlp.py (PyTorch checkpoints are read directly: zip + pickle).
#pragma once

#include <filesystem>
#include <functional>
#include <string>

namespace rm::booknlp {

void convert_models(const std::filesystem::path & src, const std::filesystem::path & out, bool f16,
                    const std::function<void(const std::string &)> & log = {});

}  // namespace rm::booknlp
