#include "ggml_model.h"

#include "ggml-alloc.h"
#include "ggml-cpu.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <thread>

namespace rm::booknlp {

static std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

Backend::Backend(const std::string & name, int device) {
    const std::string want = lower(name);
    if (want != "cpu") {
        int seen = 0;
        for (size_t i = 0; i < ggml_backend_dev_count(); ++i) {
            ggml_backend_dev_t dev = ggml_backend_dev_get(i);
            if (ggml_backend_dev_type(dev) != GGML_BACKEND_DEVICE_TYPE_GPU) continue;
            const std::string reg = lower(ggml_backend_reg_name(ggml_backend_dev_backend_reg(dev)));
            if (!want.empty() && reg.find(want) == std::string::npos) continue;
            if (seen++ != device) continue;
            backend_ = ggml_backend_dev_init(dev, nullptr);
            break;
        }
        if (!backend_ && !want.empty()) throw std::runtime_error("no " + name + " device " + std::to_string(device));
    }
    if (!backend_) {
        backend_ = ggml_backend_init_by_type(GGML_BACKEND_DEVICE_TYPE_CPU, nullptr);
        ggml_backend_cpu_set_n_threads(backend_, std::max(1, static_cast<int>(std::thread::hardware_concurrency())));
    }
    if (!backend_) throw std::runtime_error("cannot initialise a ggml backend");
}

Backend::~Backend() {
    if (backend_) ggml_backend_free(backend_);
}

std::string Backend::name() const { return ggml_backend_name(backend_); }

GgufModel::GgufModel(const std::string & path, const Backend & backend) : backend_(backend.get()) {
    gguf_init_params params{true, &ctx_};
    gguf_ = gguf_init_from_file(path.c_str(), params);
    if (!gguf_) throw std::runtime_error("cannot read GGUF " + path);
    buffer_ = ggml_backend_alloc_ctx_tensors(ctx_, backend_);
    if (!buffer_) throw std::runtime_error("cannot allocate weights for " + path);
    ggml_backend_buffer_set_usage(buffer_, GGML_BACKEND_BUFFER_USAGE_WEIGHTS);

    std::ifstream f(path, std::ios::binary);
    const size_t data = gguf_get_data_offset(gguf_);
    std::vector<char> buf;
    for (int64_t i = 0; i < gguf_get_n_tensors(gguf_); ++i) {
        const char * name = gguf_get_tensor_name(gguf_, i);
        ggml_tensor * t = ggml_get_tensor(ctx_, name);
        buf.resize(ggml_nbytes(t));
        f.seekg(static_cast<std::streamoff>(data + gguf_get_tensor_offset(gguf_, i)));
        f.read(buf.data(), static_cast<std::streamsize>(buf.size()));
        if (!f) throw std::runtime_error(std::string("short read for tensor ") + name);
        ggml_backend_tensor_set(t, buf.data(), 0, buf.size());
        tensors_[name] = t;
    }
}

GgufModel::~GgufModel() {
    if (buffer_) ggml_backend_buffer_free(buffer_);
    if (ctx_) ggml_free(ctx_);
    if (gguf_) gguf_free(gguf_);
}

ggml_tensor * GgufModel::maybe(const std::string & name) const {
    auto it = tensors_.find(name);
    return it == tensors_.end() ? nullptr : it->second;
}

ggml_tensor * GgufModel::tensor(const std::string & name) const {
    ggml_tensor * t = maybe(name);
    if (!t) throw std::runtime_error("missing tensor " + name);
    return t;
}

std::vector<float> GgufModel::read(const std::string & name) const {
    ggml_tensor * t = tensor(name);
    const int64_t n = ggml_nelements(t);
    std::vector<float> out(static_cast<size_t>(n));
    if (t->type == GGML_TYPE_F32) {
        ggml_backend_tensor_get(t, out.data(), 0, ggml_nbytes(t));
    } else if (t->type == GGML_TYPE_F16) {
        std::vector<ggml_fp16_t> h(static_cast<size_t>(n));
        ggml_backend_tensor_get(t, h.data(), 0, ggml_nbytes(t));
        for (int64_t i = 0; i < n; ++i) out[i] = ggml_fp16_to_fp32(h[i]);
    } else {
        throw std::runtime_error("unsupported host read type for " + name);
    }
    return out;
}

int64_t GgufModel::key_id(const std::string & key) const {
    const int64_t id = gguf_find_key(gguf_, key.c_str());
    if (id < 0) throw std::runtime_error("missing GGUF key " + key);
    return id;
}

uint32_t GgufModel::u32(const std::string & key) const { return gguf_get_val_u32(gguf_, key_id(key)); }
float GgufModel::f32(const std::string & key) const { return gguf_get_val_f32(gguf_, key_id(key)); }
std::string GgufModel::str(const std::string & key) const { return gguf_get_val_str(gguf_, key_id(key)); }

std::vector<std::string> GgufModel::strings(const std::string & key) const {
    const int64_t id = key_id(key);
    std::vector<std::string> out(gguf_get_arr_n(gguf_, id));
    for (size_t i = 0; i < out.size(); ++i) out[i] = gguf_get_arr_str(gguf_, id, i);
    return out;
}

Graph::Graph(ggml_backend_t backend, size_t max_nodes) : backend_(backend) {
    ggml_init_params p{ggml_tensor_overhead() * max_nodes + ggml_graph_overhead_custom(max_nodes, false), nullptr, true};
    ctx_ = ggml_init(p);
    graph_ = ggml_new_graph_custom(ctx_, max_nodes, false);
}

Graph::~Graph() {
    if (alloc_) ggml_gallocr_free(alloc_);
    if (ctx_) ggml_free(ctx_);
}

ggml_tensor * Graph::input(ggml_type type, int64_t ne0, int64_t ne1, int64_t ne2, int64_t ne3) {
    ggml_tensor * t = ggml_new_tensor_4d(ctx_, type, ne0, ne1, ne2, ne3);
    ggml_set_input(t);
    return t;
}

void Graph::output(ggml_tensor * t) {
    ggml_set_output(t);
    outputs_.push_back(t);
}

void Graph::allocate() {
    for (ggml_tensor * t : outputs_) ggml_build_forward_expand(graph_, t);
    alloc_ = ggml_gallocr_new(ggml_backend_get_default_buffer_type(backend_));
    if (!ggml_gallocr_alloc_graph(alloc_, graph_)) throw std::runtime_error("graph allocation failed");
}

void Graph::run() {
    if (ggml_backend_graph_compute(backend_, graph_) != GGML_STATUS_SUCCESS)
        throw std::runtime_error("graph compute failed");
}

std::vector<float> Graph::get(ggml_tensor * t) {
    std::vector<float> out(static_cast<size_t>(ggml_nelements(t)));
    ggml_backend_tensor_get(t, out.data(), 0, ggml_nbytes(t));
    return out;
}

}  // namespace rm::booknlp
