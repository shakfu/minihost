// minihost_graph_v2.hpp
//
// Header-only C++ wrapper over the C `mh_graph_v2_*` API. RAII, method
// syntax, exceptions on error. No new functionality -- pure sugar.
//
// The C ABI in `minihost_graph_v2.h` remains the source of truth and
// the surface the Python wheel binds against. This header exists so
// C++ consumers (the desktop app, future C++ examples) can write
//
//     minihost::GraphV2 g(512, 48000.0);
//     auto in  = g.addInput(2);
//     auto out = g.addOutput(2);
//     g.connect(in, out);
//     g.compile();
//
// instead of threading opaque pointers and `char err[256]` buffers.

#pragma once

#include "minihost_graph_v2.h"

#include <stdexcept>
#include <string>
#include <utility>

namespace minihost {

class GraphV2 {
public:
    using NodeId = MH_NodeId;

    GraphV2(int max_block_size, double sample_rate)
    {
        char err[kErrLen] = {0};
        g_ = mh_graph_v2_create(max_block_size, sample_rate,
                                err, sizeof(err));
        if (g_ == nullptr) throwErr("mh_graph_v2_create", err);
    }

    ~GraphV2() { if (g_) mh_graph_v2_close(g_); }

    GraphV2(const GraphV2&) = delete;
    GraphV2& operator=(const GraphV2&) = delete;

    GraphV2(GraphV2&& other) noexcept : g_(other.g_) { other.g_ = nullptr; }
    GraphV2& operator=(GraphV2&& other) noexcept
    {
        if (this != &other)
        {
            if (g_) mh_graph_v2_close(g_);
            g_ = other.g_;
            other.g_ = nullptr;
        }
        return *this;
    }

    // Direct access for cases that need to reach the C ABI (e.g. an
    // extension we haven't wrapped yet). Use sparingly.
    MH_GraphV2* handle() const noexcept { return g_; }

    NodeId addPlugin(MH_Plugin* p)
    {
        char err[kErrLen] = {0};
        const NodeId id = mh_graph_v2_add_plugin(g_, p, err, sizeof(err));
        if (id < 0) throwErr("add_plugin", err);
        return id;
    }

    NodeId addInput(int channels)
    {
        char err[kErrLen] = {0};
        const NodeId id = mh_graph_v2_add_input(g_, channels,
                                                err, sizeof(err));
        if (id < 0) throwErr("add_input", err);
        return id;
    }

    NodeId addOutput(int channels)
    {
        char err[kErrLen] = {0};
        const NodeId id = mh_graph_v2_add_output(g_, channels,
                                                 err, sizeof(err));
        if (id < 0) throwErr("add_output", err);
        return id;
    }

    NodeId addMix(int num_inputs, int channels)
    {
        char err[kErrLen] = {0};
        const NodeId id = mh_graph_v2_add_mix(g_, num_inputs, channels,
                                              err, sizeof(err));
        if (id < 0) throwErr("add_mix", err);
        return id;
    }

    // Single-port connect (the common case: plugin/output/mix-output
    // ports default to 0). For mix-input ports >0, pass dst_port.
    void connect(NodeId src, NodeId dst, int dst_port = 0)
    {
        char err[kErrLen] = {0};
        if (!mh_graph_v2_connect(g_, src, 0, dst, dst_port,
                                 err, sizeof(err)))
            throwErr("connect", err);
    }

    void setMixGain(NodeId mix_node, int input_index, float gain)
    {
        if (!mh_graph_v2_set_mix_gain(g_, mix_node, input_index, gain))
            throw std::runtime_error("set_mix_gain failed (bad node/index)");
    }

    void compile()
    {
        char err[kErrLen] = {0};
        if (!mh_graph_v2_compile(g_, err, sizeof(err)))
            throwErr("compile", err);
    }

    // Renders one block. The buffer layout matches the C API exactly;
    // callers typically pack pointer tables once and reuse them.
    void renderBlock(const float* const* const* input_buffers,
                     int num_input_nodes,
                     float* const* const* output_buffers,
                     int num_output_nodes,
                     int nframes)
    {
        if (!mh_graph_v2_render_block(g_,
                                      input_buffers,  num_input_nodes,
                                      output_buffers, num_output_nodes,
                                      nframes))
            throw std::runtime_error("render_block failed");
    }

    int numNodes()        const noexcept
    { return mh_graph_v2_num_nodes(g_); }
    int numInputNodes()   const noexcept
    { return mh_graph_v2_num_input_nodes(g_); }
    int numOutputNodes()  const noexcept
    { return mh_graph_v2_num_output_nodes(g_); }
    bool isCompiled()     const noexcept
    { return mh_graph_v2_is_compiled(g_) != 0; }

private:
    static constexpr size_t kErrLen = 256;

    [[noreturn]] static void throwErr(const char* op, const char* err)
    {
        std::string msg = "minihost::GraphV2::";
        msg += op;
        if (err && *err) { msg += ": "; msg += err; }
        throw std::runtime_error(std::move(msg));
    }

    MH_GraphV2* g_ = nullptr;
};

} // namespace minihost
