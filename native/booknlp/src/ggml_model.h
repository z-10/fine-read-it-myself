// GGUF weights on a ggml backend (Vulkan / Metal / CPU) + helpers to run small graphs.
#pragma once

#include "ggml-alloc.h"
#include "ggml-backend.h"
#include "ggml.h"
#include "gguf.h"

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace rm::booknlp {

// One compute backend shared by the three models. name: "vulkan", "metal", "cpu" or "" (best available).
class Backend {
public:
    explicit Backend(const std::string & name = "", int device = 0);
    ~Backend();
    ggml_backend_t get() const { return backend_; }
    std::string name() const;

private:
    ggml_backend_t backend_ = nullptr;
};

class GgufModel {
public:
    GgufModel(const std::string & path, const Backend & backend);
    ~GgufModel();

    ggml_tensor * tensor(const std::string & name) const;            // throws if missing
    ggml_tensor * maybe(const std::string & name) const;             // nullptr if missing
    std::vector<float> read(const std::string & name) const;         // tensor as f32 (host copy)

    uint32_t u32(const std::string & key) const;
    float f32(const std::string & key) const;
    std::string str(const std::string & key) const;
    std::vector<std::string> strings(const std::string & key) const;

    ggml_backend_t backend() const { return backend_; }

private:
    int64_t key_id(const std::string & key) const;
    gguf_context * gguf_ = nullptr;
    ggml_context * ctx_ = nullptr;
    ggml_backend_buffer_t buffer_ = nullptr;
    ggml_backend_t backend_ = nullptr;
    std::unordered_map<std::string, ggml_tensor *> tensors_;
};

// A graph built on the fly: allocate a context, build, compute on the backend, read outputs.
class Graph {
public:
    explicit Graph(ggml_backend_t backend, size_t max_nodes = 8192);
    ~Graph();
    ggml_context * ctx() const { return ctx_; }
    ggml_tensor * input(ggml_type type, int64_t ne0, int64_t ne1 = 1, int64_t ne2 = 1, int64_t ne3 = 1);
    void output(ggml_tensor * t);            // mark as output (kept, readable after run)
    void allocate();                         // after building: allocate, then set() the inputs
    void run();
    static void set(ggml_tensor * t, const void * data) { ggml_backend_tensor_set(t, data, 0, ggml_nbytes(t)); }
    static std::vector<float> get(ggml_tensor * t);

private:
    ggml_backend_t backend_;
    ggml_context * ctx_ = nullptr;
    ggml_cgraph * graph_ = nullptr;
    ggml_gallocr_t alloc_ = nullptr;
    std::vector<ggml_tensor *> outputs_;
};

}  // namespace rm::booknlp
