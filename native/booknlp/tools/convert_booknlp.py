"""Convert the ModernBookNLP models to GGUF for the C++ port.

Run with the ModernBookNLP Python env (torch, safetensors) and llama.cpp's gguf-py:
  python convert_booknlp.py --out <dir> [--f16]

Inputs (downloaded by ModernBookNLP / transformers on first use):
  ~/booknlp_models/entities_google_bert_uncased_L-6_H-768_A-12-v1.0.model   entity tagger (BERT-6L + BiLSTM/CRF)
  ~/booknlp_models/coref_google_bert_uncased_L-12_H-768_A-12-v1.0.model     coreference (BERT-12L + scorers)
  ~/booknlp_models/ModernBERT_T2000.safetensors                            quote attribution (ModernBERT-large + MLP)
  HF cache: google/bert_uncased_L-{6,12}_H-768_A-12 vocab.txt, answerdotai/ModernBERT-large tokenizer.json

Outputs: bnlp-entities.gguf, bnlp-coref.gguf, bnlp-quote.gguf. Tensor names are the PyTorch names;
the quote model's BatchNorm (eval mode) is folded into the preceding Linear, whose weight is split into
proj.0.weight_q / proj.0.weight_m (the quote and mention halves of its input).
"""
import argparse
import glob
import json
import os
import sys
from pathlib import Path

import numpy as np
import torch
from safetensors import safe_open

sys.path.insert(0, r"E:\read-myself\llama.cpp\gguf-py")
import gguf  # noqa: E402

MODELS = Path.home() / "booknlp_models"
HF = Path.home() / ".cache" / "huggingface" / "hub"
BOOKNLP_DATA = Path(r"E:\read-myself\ModernBookNLP_QA\ModernBookNLP\booknlp\english\data")
SKIP = ("position_ids", "pooler.", "supersense", "wn_embedding", "num_batches_tracked")


def hf_file(repo: str, name: str) -> Path:
    hits = glob.glob(str(HF / f"models--{repo.replace('/', '--')}" / "snapshots" / "*" / name))
    if not hits:
        raise FileNotFoundError(f"{repo}/{name} not in the HF cache")
    return Path(hits[0])


def add_tensors(w: gguf.GGUFWriter, tensors: dict, f16: bool):
    for name, t in tensors.items():
        if any(s in name for s in SKIP):
            continue
        a = t.detach().cpu().float().numpy() if isinstance(t, torch.Tensor) else np.asarray(t, np.float32)
        # 2D matrices go to f16 on request; vectors, embeddings of small heads and CRF stay f32
        if f16 and a.ndim == 2 and a.shape[0] >= 256 and a.shape[1] >= 256:
            a = a.astype(np.float16)
        w.add_tensor(name, np.ascontiguousarray(a))


def bert_vocab(repo: str) -> list[str]:
    vocab = hf_file(repo, "vocab.txt").read_text(encoding="utf-8").split("\n")
    if vocab and vocab[-1] == "":
        vocab.pop()
    return vocab + ["[CAP]"]   # ModernBookNLP adds [CAP] as a special token (id 30522)


def bert_meta(w: gguf.GGUFWriter, sd: dict, repo: str, n_layer: int):
    cfg = json.loads(hf_file(repo, "config.json").read_text(encoding="utf-8"))
    w.add_uint32("bnlp.bert.n_layer", n_layer)
    w.add_uint32("bnlp.bert.n_embd", cfg["hidden_size"])
    w.add_uint32("bnlp.bert.n_head", cfg["num_attention_heads"])
    w.add_uint32("bnlp.bert.n_ff", cfg["intermediate_size"])
    w.add_float32("bnlp.bert.norm_eps", cfg.get("layer_norm_eps", 1e-12))
    w.add_uint32("bnlp.bert.n_ctx", cfg["max_position_embeddings"])
    vocab = bert_vocab(repo)
    assert len(vocab) == sd["bert.embeddings.word_embeddings.weight"].shape[0], len(vocab)
    w.add_array("bnlp.vocab", vocab)


def convert_entities(out: Path, f16: bool):
    name = "entities_google_bert_uncased_L-6_H-768_A-12-v1.0.model"
    sd = torch.load(MODELS / name, map_location="cpu", weights_only=False)
    w = gguf.GGUFWriter(str(out / "bnlp-entities.gguf"), "bnlp-entities")
    w.add_string("bnlp.source", name)
    bert_meta(w, sd, "google/bert_uncased_L-6_H-768_A-12", 6)
    tags = {}
    for line in (BOOKNLP_DATA / "entity_cat.tagset").read_text(encoding="utf-8").splitlines():
        if line.strip():
            t, i = line.split("\t")
            tags[int(i)] = t
    w.add_array("bnlp.tagset", [tags[i] for i in range(len(tags))])
    # rule data used by name coreference and gender inference
    w.add_string("bnlp.aliases", (BOOKNLP_DATA / "aliases.txt").read_text(encoding="utf-8"))
    w.add_string("bnlp.gender_terms", (BOOKNLP_DATA / "gutenberg_prop_gender_terms.txt").read_text(encoding="utf-8"))
    add_tensors(w, sd, f16)
    w.write_header_to_file(); w.write_kv_data_to_file(); w.write_tensors_to_file(); w.close()


def convert_coref(out: Path, f16: bool):
    name = "coref_google_bert_uncased_L-12_H-768_A-12-v1.0.model"
    sd = torch.load(MODELS / name, map_location="cpu", weights_only=False)
    # mention_mention1 acts on [a ; b ; a*b ; distance ; nested1 ; nested2 ; speaker]: split its columns
    w1 = sd.pop("mention_mention1.weight")
    r = sd["unary1.weight"].shape[1]   # span representation size
    for i, name in enumerate(("a", "b", "ab")):
        sd[f"mention_mention1.weight_{name}"] = w1[:, i * r:(i + 1) * r].contiguous()
    sd["mention_mention1.weight_rest"] = w1[:, 3 * r:].contiguous()
    w = gguf.GGUFWriter(str(out / "bnlp-coref.gguf"), "bnlp-coref")
    w.add_string("bnlp.source", name)
    bert_meta(w, sd, "google/bert_uncased_L-12_H-768_A-12", 12)
    add_tensors(w, sd, f16)
    w.write_header_to_file(); w.write_kv_data_to_file(); w.write_tensors_to_file(); w.close()


def convert_quote(out: Path, f16: bool):
    name = "ModernBERT_T2000.safetensors"
    f = safe_open(str(MODELS / name), "pt")
    sd = {k: f.get_tensor(k) for k in f.keys()}
    # fold eval-mode BatchNorm1d (proj.1) into proj.0
    eps = 1e-5
    s = sd["proj.1.weight"] / torch.sqrt(sd["proj.1.running_var"] + eps)
    sd["proj.0.weight"] = sd["proj.0.weight"] * s[:, None]
    sd["proj.0.bias"] = (sd["proj.0.bias"] - sd["proj.1.running_mean"]) * s + sd["proj.1.bias"]
    for k in ("proj.1.weight", "proj.1.bias", "proj.1.running_mean", "proj.1.running_var", "proj.1.num_batches_tracked"):
        sd.pop(k)
    # proj.0 acts on [quote ; mention]: split its columns so the halves are applied per node
    w0 = sd.pop("proj.0.weight")
    half = w0.shape[1] // 2
    sd["proj.0.weight_q"] = w0[:, :half].contiguous()
    sd["proj.0.weight_m"] = w0[:, half:].contiguous()
    cfg = json.loads(hf_file("answerdotai/ModernBERT-large", "config.json").read_text(encoding="utf-8"))
    w = gguf.GGUFWriter(str(out / "bnlp-quote.gguf"), "bnlp-quote")
    w.add_string("bnlp.source", name)
    w.add_uint32("bnlp.mbert.n_layer", cfg["num_hidden_layers"])
    w.add_uint32("bnlp.mbert.n_embd", cfg["hidden_size"])
    w.add_uint32("bnlp.mbert.n_head", cfg["num_attention_heads"])
    w.add_uint32("bnlp.mbert.n_ff", cfg["intermediate_size"])
    w.add_float32("bnlp.mbert.norm_eps", cfg["norm_eps"])
    w.add_uint32("bnlp.mbert.global_every", cfg["global_attn_every_n_layers"])
    w.add_uint32("bnlp.mbert.local_window", cfg["local_attention"])
    w.add_float32("bnlp.mbert.global_rope_theta", cfg["global_rope_theta"])
    w.add_float32("bnlp.mbert.local_rope_theta", cfg["local_rope_theta"])
    w.add_string("bnlp.tokenizer_json", hf_file("answerdotai/ModernBERT-large", "tokenizer.json").read_text(encoding="utf-8"))
    add_tensors(w, sd, f16)
    w.write_header_to_file(); w.write_kv_data_to_file(); w.write_tensors_to_file(); w.close()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", required=True)
    ap.add_argument("--f16", action="store_true")
    ap.add_argument("--only", choices=["entities", "coref", "quote"])
    a = ap.parse_args()
    out = Path(a.out)
    out.mkdir(parents=True, exist_ok=True)
    with torch.no_grad():
        for name, fn in (("entities", convert_entities), ("coref", convert_coref), ("quote", convert_quote)):
            if a.only in (None, name):
                fn(out, a.f16)
    for p in sorted(out.glob("bnlp-*.gguf")):
        print(p.name, f"{p.stat().st_size / 2**20:.0f} MiB")


if __name__ == "__main__":
    main()
