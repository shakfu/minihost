// canvas.cpp -- see canvas.h for design.

#include "canvas.h"

#include "minihost_audiofile.h"

#include <algorithm>
#include <unordered_set>

namespace minihost_desktop {

namespace {

juce::Colour kindColour(const juce::String& kind)
{
    if (kind == "input")  return juce::Colour(0xff3a6f99);
    if (kind == "output") return juce::Colour(0xff8e4a3a);
    if (kind == "mix")    return juce::Colour(0xff5c7a3a);
    /* plugin */          return juce::Colour(0xff404040);
}

} // namespace

CanvasComponent::CanvasComponent()
{
    setOpaque(true);
    setWantsKeyboardFocus(true);
}

void CanvasComponent::setDocument(project::ProjectDocument* doc)
{
    doc_ = doc;
    drag_kind_      = DragKind::None;
    dragging_node_  = -1;
    selected_node_  = -1;
    selected_edge_  = -1;
    rebuildLayout();
    repaint();
}

void CanvasComponent::rebuildLayout()
{
    nodes_.clear();
    edges_.clear();
    if (doc_ == nullptr) return;

    auto addNode = [&](const juce::String& id, const juce::String& kind,
                       const juce::String& label,
                       int n_in, int n_out) {
        NodeLayout n;
        n.id    = id;
        n.kind  = kind;
        n.label = label;
        n.num_input_ports  = n_in;
        n.num_output_ports = n_out;
        n.bounds = juce::Rectangle<float>(0, 0, kNodeWidth, kNodeHeight);
        nodes_.push_back(std::move(n));
    };

    for (const auto& n : doc_->inputs)
        addNode(n.id, "input",
                n.id + "\n(input, " + juce::String(n.channels) + "ch)",
                /*in*/0, /*out*/1);
    for (const auto& n : doc_->plugins)
        addNode(n.id, "plugin",
                n.id + "\n" + n.path.getFileNameWithoutExtension(),
                /*in*/1, /*out*/1);
    for (const auto& n : doc_->mixes)
        addNode(n.id, "mix",
                n.id + "\n(mix " + juce::String(n.num_inputs)
                  + " x " + juce::String(n.channels) + "ch)",
                /*in*/n.num_inputs, /*out*/1);
    for (const auto& n : doc_->outputs)
        addNode(n.id, "output",
                n.id + "\n(output, " + juce::String(n.channels) + "ch)",
                /*in*/1, /*out*/0);

    std::unordered_map<std::string, int> id_to_idx;
    for (size_t i = 0; i < nodes_.size(); ++i)
        id_to_idx[nodes_[i].id.toStdString()] = (int) i;

    for (size_t i = 0; i < doc_->edges.size(); ++i)
    {
        const auto& e = doc_->edges[i];
        auto src_it = id_to_idx.find(e.src.toStdString());
        auto dst_it = id_to_idx.find(e.dst.toStdString());
        if (src_it == id_to_idx.end() || dst_it == id_to_idx.end())
            continue;
        EdgeLayout edge{ src_it->second, dst_it->second, e.dst_port,
                         (int) i };
        edges_.push_back(edge);
    }

    autoPositionNodes();
}

void CanvasComponent::autoPositionNodes()
{
    const int N = (int) nodes_.size();
    std::vector<int> column(N, 0);
    std::vector<std::vector<int>> predecessors(N);
    for (const auto& e : edges_)
        predecessors[(size_t) e.dst_node_index].push_back(e.src_node_index);

    bool changed = true;
    int  rounds  = 0;
    while (changed && rounds++ < N + 1)
    {
        changed = false;
        for (int i = 0; i < N; ++i)
        {
            int needed = 0;
            for (int p : predecessors[(size_t) i])
                needed = std::max(needed, column[(size_t) p] + 1);
            if (column[(size_t) i] != needed)
            {
                column[(size_t) i] = needed;
                changed = true;
            }
        }
    }

    std::unordered_map<int, int> next_row_for_column;
    for (int i = 0; i < N; ++i)
    {
        const auto& n = nodes_[(size_t) i];
        if (doc_ != nullptr)
        {
            auto it = doc_->layout.find(n.id.toStdString());
            if (it != doc_->layout.end())
            {
                nodes_[(size_t) i].bounds.setPosition(
                    it->second.x, it->second.y);
                continue;
            }
        }
        const int col = column[(size_t) i];
        const int row = next_row_for_column[col]++;
        nodes_[(size_t) i].bounds.setPosition(
            kCanvasPadding + col * kColumnWidth,
            kCanvasPadding + row * kRowHeight);
    }
}

void CanvasComponent::resized() {}

juce::Point<float> CanvasComponent::outputPortPos(const NodeLayout& n) const
{
    return { n.bounds.getRight(), n.bounds.getCentreY() };
}

juce::Point<float> CanvasComponent::inputPortPos(const NodeLayout& n,
                                                 int port) const
{
    const int N = std::max(1, n.num_input_ports);
    const float t = (N == 1)
                  ? 0.5f
                  : (float)(port + 1) / (float)(N + 1);
    return { n.bounds.getX(),
             n.bounds.getY() + n.bounds.getHeight() * t };
}

void CanvasComponent::edgeCubic(juce::Point<float> p0, juce::Point<float> p1,
                                juce::Point<float>& c0, juce::Point<float>& c1) const
{
    const float dx = std::max(40.0f, std::abs(p1.x - p0.x) * 0.4f);
    c0 = { p0.x + dx, p0.y };
    c1 = { p1.x - dx, p1.y };
}

void CanvasComponent::paint(juce::Graphics& g)
{
    g.fillAll(juce::Colour(0xff1a1a1a));

    if (doc_ == nullptr)
    {
        g.setColour(juce::Colours::grey);
        g.setFont(juce::FontOptions(14.0f));
        g.drawText("No project loaded.",
                   getLocalBounds(), juce::Justification::centred, false);
        return;
    }

    // Edges (under nodes).
    for (size_t i = 0; i < edges_.size(); ++i)
    {
        const auto& e = edges_[i];
        const auto& src = nodes_[(size_t) e.src_node_index];
        const auto& dst = nodes_[(size_t) e.dst_node_index];
        const auto p0 = outputPortPos(src);
        const auto p1 = inputPortPos(dst, e.dst_port);
        juce::Point<float> c0, c1;
        edgeCubic(p0, p1, c0, c1);
        juce::Path path;
        path.startNewSubPath(p0);
        path.cubicTo(c0, c1, p1);
        const bool is_sel = ((int) i == selected_edge_);
        g.setColour(is_sel ? juce::Colours::yellow
                           : juce::Colour(0xffb0b0b0));
        g.strokePath(path, juce::PathStrokeType(is_sel ? 2.5f : 1.6f));
    }

    // In-progress connect drag.
    if (drag_kind_ == DragKind::ConnectingFromPort
        && connect_from_node_ >= 0)
    {
        const auto p0 = outputPortPos(nodes_[(size_t) connect_from_node_]);
        const auto p1 = connect_to_pos_;
        juce::Point<float> c0, c1;
        edgeCubic(p0, p1, c0, c1);
        juce::Path path;
        path.startNewSubPath(p0);
        path.cubicTo(c0, c1, p1);
        g.setColour(juce::Colour(0xffd0d000));
        const float dashes[2] = { 6.0f, 4.0f };
        juce::PathStrokeType stroke(1.6f);
        juce::Path dashed;
        stroke.createDashedStroke(dashed, path, dashes, 2);
        g.fillPath(dashed);
    }

    // Nodes.
    g.setFont(juce::FontOptions(12.0f));
    for (int i = 0; i < (int) nodes_.size(); ++i)
    {
        const auto& n = nodes_[(size_t) i];
        g.setColour(kindColour(n.kind));
        g.fillRoundedRectangle(n.bounds, 6.0f);

        g.setColour(i == selected_node_
                    ? juce::Colours::yellow
                    : juce::Colour(0xff707070));
        g.drawRoundedRectangle(n.bounds, 6.0f,
                               i == selected_node_ ? 2.0f : 1.0f);

        g.setColour(juce::Colours::white);
        g.drawFittedText(n.label,
                         n.bounds.toNearestInt().reduced(8),
                         juce::Justification::centred, 3);

        g.setColour(juce::Colour(0xffd0d0d0));
        for (int port = 0; port < n.num_input_ports; ++port)
        {
            const auto p = inputPortPos(n, port);
            g.fillEllipse(p.x - kPortRadius, p.y - kPortRadius,
                          kPortRadius * 2, kPortRadius * 2);
        }
        if (n.num_output_ports > 0)
        {
            const auto p = outputPortPos(n);
            g.fillEllipse(p.x - kPortRadius, p.y - kPortRadius,
                          kPortRadius * 2, kPortRadius * 2);
        }
    }
}

int CanvasComponent::hitTestNode(juce::Point<float> p) const
{
    for (int i = (int) nodes_.size() - 1; i >= 0; --i)
        if (nodes_[(size_t) i].bounds.contains(p))
            return i;
    return -1;
}

int CanvasComponent::hitTestOutputPort(juce::Point<float> p) const
{
    for (int i = (int) nodes_.size() - 1; i >= 0; --i)
    {
        const auto& n = nodes_[(size_t) i];
        if (n.num_output_ports <= 0) continue;
        const auto pp = outputPortPos(n);
        if (p.getDistanceFrom(pp) <= kPortHitRadius) return i;
    }
    return -1;
}

std::pair<int, int> CanvasComponent::hitTestInputPort(juce::Point<float> p) const
{
    for (int i = (int) nodes_.size() - 1; i >= 0; --i)
    {
        const auto& n = nodes_[(size_t) i];
        for (int port = 0; port < n.num_input_ports; ++port)
        {
            const auto pp = inputPortPos(n, port);
            if (p.getDistanceFrom(pp) <= kPortHitRadius)
                return { i, port };
        }
    }
    return { -1, -1 };
}

int CanvasComponent::hitTestEdge(juce::Point<float> p, float tolerance) const
{
    // Sample each edge's cubic curve and keep the minimum point-to-
    // segment distance. Cheap and good enough for click selection.
    int best = -1;
    float best_d = tolerance;
    const int kSamples = 24;
    for (size_t i = 0; i < edges_.size(); ++i)
    {
        const auto& e = edges_[i];
        const auto& src = nodes_[(size_t) e.src_node_index];
        const auto& dst = nodes_[(size_t) e.dst_node_index];
        const auto p0 = outputPortPos(src);
        const auto p1 = inputPortPos(dst, e.dst_port);
        juce::Point<float> c0, c1;
        edgeCubic(p0, p1, c0, c1);
        juce::Path path;
        path.startNewSubPath(p0);
        path.cubicTo(c0, c1, p1);

        const auto bounds = path.getBounds().expanded(8.0f);
        if (!bounds.contains(p)) continue;

        juce::Point<float> prev = p0;
        for (int s = 1; s <= kSamples; ++s)
        {
            const float t = (float) s / (float) kSamples;
            juce::Path::Iterator it(path);
            (void) it;  // unused -- we use the bezier formula directly
            const float u = 1.0f - t;
            const juce::Point<float> q
                = p0 * (u * u * u)
                + c0 * (3.0f * u * u * t)
                + c1 * (3.0f * u * t * t)
                + p1 * (t * t * t);
            const auto a = prev, b = q;
            // point-segment distance
            const auto ab = b - a;
            const float len2 = ab.x * ab.x + ab.y * ab.y;
            float u2 = 0.0f;
            if (len2 > 1e-6f)
                u2 = juce::jlimit(0.0f, 1.0f,
                    ((p.x - a.x) * ab.x + (p.y - a.y) * ab.y) / len2);
            const juce::Point<float> proj{ a.x + ab.x * u2,
                                           a.y + ab.y * u2 };
            const float d = p.getDistanceFrom(proj);
            if (d < best_d) { best_d = d; best = (int) i; }
            prev = q;
        }
    }
    return best;
}

void CanvasComponent::mouseDown(const juce::MouseEvent& e)
{
    grabKeyboardFocus();
    const auto p = e.position;

    if (e.mods.isPopupMenu())
    {
        // Right-click on a node shows the per-node menu; on empty
        // space, the add-node menu.
        const int hit = hitTestNode(p);
        if (hit >= 0)
        {
            selected_node_ = hit;
            selected_edge_ = -1;
            repaint();
            juce::PopupMenu mn;
            enum { kEditProps = 1, kDelete };
            mn.addItem(kEditProps, "Properties...");
            mn.addItem(kDelete,    "Delete");
            juce::PopupMenu::Options opts;
            opts = opts.withTargetScreenArea(
                { e.getScreenPosition(), e.getScreenPosition() });
            const int node = hit;
            mn.showMenuAsync(opts, [this, node](int chosen) {
                if (chosen == kEditProps) showNodePropertiesDialog(node);
                else if (chosen == kDelete)
                {
                    selected_node_ = node;
                    deleteSelected();
                }
            });
            return;
        }
        showContextMenu(e.getScreenPosition());
        return;
    }

    // Output port first -- start a connect drag.
    if (doc_ != nullptr)
    {
        int out_hit = hitTestOutputPort(p);
        if (out_hit >= 0)
        {
            drag_kind_         = DragKind::ConnectingFromPort;
            connect_from_node_ = out_hit;
            connect_to_pos_    = p;
            selected_node_     = -1;
            selected_edge_     = -1;
            repaint();
            return;
        }
    }

    // Node body -- start move.
    const int hit = hitTestNode(p);
    if (hit >= 0)
    {
        selected_node_ = hit;
        selected_edge_ = -1;
        drag_kind_     = DragKind::MovingNode;
        dragging_node_ = hit;
        drag_offset_   = p - nodes_[(size_t) hit].bounds.getPosition();
        repaint();
        return;
    }

    // Edge?
    const int edge_hit = hitTestEdge(p);
    if (edge_hit >= 0)
    {
        selected_edge_ = edge_hit;
        selected_node_ = -1;
        drag_kind_     = DragKind::None;
        repaint();
        return;
    }

    // Empty space: clear selection.
    selected_node_ = -1;
    selected_edge_ = -1;
    drag_kind_     = DragKind::None;
    repaint();
}

void CanvasComponent::mouseDrag(const juce::MouseEvent& e)
{
    if (drag_kind_ == DragKind::MovingNode && dragging_node_ >= 0)
    {
        auto& n = nodes_[(size_t) dragging_node_];
        n.bounds.setPosition(e.position - drag_offset_);
        repaint();
    }
    else if (drag_kind_ == DragKind::ConnectingFromPort)
    {
        connect_to_pos_ = e.position;
        repaint();
    }
}

void CanvasComponent::mouseUp(const juce::MouseEvent& e)
{
    if (drag_kind_ == DragKind::MovingNode && dragging_node_ >= 0
        && doc_ != nullptr)
    {
        const auto& n = nodes_[(size_t) dragging_node_];
        doc_->layout[n.id.toStdString()]
            = project::NodePosition{ n.bounds.getX(), n.bounds.getY() };
    }
    else if (drag_kind_ == DragKind::ConnectingFromPort
             && connect_from_node_ >= 0)
    {
        auto [dst_idx, dst_port] = hitTestInputPort(e.position);
        if (dst_idx >= 0 && dst_idx != connect_from_node_)
            addEdgeToDoc(connect_from_node_, dst_idx, dst_port);
    }
    drag_kind_         = DragKind::None;
    dragging_node_     = -1;
    connect_from_node_ = -1;
    repaint();
}

void CanvasComponent::mouseDoubleClick(const juce::MouseEvent& e)
{
    // Only plugin nodes have editors. Resolve which doc plugin index
    // the hit corresponds to (plugins follow inputs in nodes_).
    if (doc_ == nullptr) return;
    const int hit = hitTestNode(e.position);
    if (hit < 0) return;
    const int n_in = (int) doc_->inputs.size();
    const int n_pl = (int) doc_->plugins.size();
    if (hit < n_in || hit >= n_in + n_pl) return;  // not a plugin node
    const int plugin_index = hit - n_in;
    if (on_open_plugin_editor_) on_open_plugin_editor_(plugin_index);
}

bool CanvasComponent::keyPressed(const juce::KeyPress& key)
{
    if (key == juce::KeyPress::deleteKey
        || key == juce::KeyPress::backspaceKey)
    {
        deleteSelected();
        return true;
    }
    return false;
}

void CanvasComponent::deleteSelected()
{
    if (doc_ == nullptr) return;
    if (selected_edge_ >= 0
        && selected_edge_ < (int) edges_.size())
    {
        removeEdgeFromDoc(edges_[(size_t) selected_edge_].doc_edge_index);
        selected_edge_ = -1;
        rebuildLayout();
        repaint();
        return;
    }
    if (selected_node_ >= 0
        && selected_node_ < (int) nodes_.size())
    {
        removeNodeFromDoc(selected_node_);
        selected_node_ = -1;
        rebuildLayout();
        repaint();
        return;
    }
}

void CanvasComponent::removeNodeFromDoc(int node_index)
{
    const juce::String id = nodes_[(size_t) node_index].id;
    const std::string  sid = id.toStdString();

    auto& doc = *doc_;
    // Drop any edge referencing this node from either side.
    doc.edges.erase(
        std::remove_if(doc.edges.begin(), doc.edges.end(),
            [&](const project::EdgeSpec& e) {
                return e.src == id || e.dst == id;
            }),
        doc.edges.end());

    auto eraseById = [&](auto& vec) {
        vec.erase(std::remove_if(vec.begin(), vec.end(),
                    [&](const auto& n) { return n.id == id; }),
                  vec.end());
    };
    eraseById(doc.inputs);
    eraseById(doc.outputs);
    eraseById(doc.plugins);
    eraseById(doc.mixes);
    doc.layout.erase(sid);
}

void CanvasComponent::removeEdgeFromDoc(int doc_edge_index)
{
    if (doc_ == nullptr) return;
    if (doc_edge_index < 0
        || doc_edge_index >= (int) doc_->edges.size()) return;
    doc_->edges.erase(doc_->edges.begin() + doc_edge_index);
}

void CanvasComponent::addEdgeToDoc(int src_node_index, int dst_node_index,
                                   int dst_port)
{
    if (doc_ == nullptr) return;
    const auto& src_n = nodes_[(size_t) src_node_index];
    const auto& dst_n = nodes_[(size_t) dst_node_index];

    // Connect-time channel validation. For non-plugin kinds the
    // channel count is on the doc spec; for plugin nodes use the
    // cached probe (-1 means "unknown", skip validation).
    auto channels_for = [&](int node_index, bool is_output_port) -> int {
        const int n_in   = (int) doc_->inputs.size();
        const int n_plug = (int) doc_->plugins.size();
        const int n_mix  = (int) doc_->mixes.size();
        if (node_index < n_in)
            return doc_->inputs[(size_t) node_index].channels;
        const int after_in = node_index - n_in;
        if (after_in < n_plug)
        {
            const auto& spec = doc_->plugins[(size_t) after_in];
            return is_output_port ? spec.probed_out_channels
                                  : spec.probed_in_channels;
        }
        const int after_plug = after_in - n_plug;
        if (after_plug < n_mix)
            return doc_->mixes[(size_t) after_plug].channels;
        const int after_mix = after_plug - n_mix;
        if (after_mix >= 0 && after_mix < (int) doc_->outputs.size())
            return doc_->outputs[(size_t) after_mix].channels;
        return -1;
    };

    const int src_ch = channels_for(src_node_index, /*is_output_port=*/true);
    const int dst_ch = channels_for(dst_node_index, /*is_output_port=*/false);
    if (src_ch >= 0 && dst_ch >= 0 && src_ch != dst_ch)
    {
        juce::AlertWindow::showMessageBoxAsync(
            juce::AlertWindow::WarningIcon,
            "Channel mismatch",
            "Cannot connect "
                + src_n.id + " (" + juce::String(src_ch) + "ch out)"
                + " -> " + dst_n.id
                + " (" + juce::String(dst_ch) + "ch in).\n\n"
                "Channel counts must match. Insert a mix or use a "
                "differently-configured plugin.");
        return;
    }

    const auto src_id = src_n.id;
    const auto dst_id = dst_n.id;

    // One edge per (dst_node, dst_port): replace any existing.
    doc_->edges.erase(
        std::remove_if(doc_->edges.begin(), doc_->edges.end(),
            [&](const project::EdgeSpec& e) {
                return e.dst == dst_id && e.dst_port == dst_port;
            }),
        doc_->edges.end());

    project::EdgeSpec e;
    e.src      = src_id;
    e.dst      = dst_id;
    e.dst_port = dst_port;
    doc_->edges.push_back(std::move(e));
    rebuildLayout();
}

void CanvasComponent::showContextMenu(juce::Point<int> screen_pos)
{
    enum {
        kAddInput = 1,
        kAddOutput,
        kAddMix2,
        kAddMix3,
        kAddPlugin,
    };
    juce::PopupMenu m;
    m.addItem(kAddInput,  "Add Input...");
    m.addItem(kAddOutput, "Add Output...");
    m.addSeparator();
    m.addItem(kAddMix2,   "Add Mix (2 inputs, stereo)");
    m.addItem(kAddMix3,   "Add Mix (3 inputs, stereo)");
    m.addSeparator();
    m.addItem(kAddPlugin, "Add Plugin...");

    juce::PopupMenu::Options opts;
    opts = opts.withTargetScreenArea({ screen_pos, screen_pos });
    m.showMenuAsync(opts,
        [this](int chosen) {
            switch (chosen)
            {
            case kAddInput:  addInputNode();   break;
            case kAddOutput: addOutputNode();  break;
            case kAddMix2:   addMixNode(2, 2); break;
            case kAddMix3:   addMixNode(3, 2); break;
            case kAddPlugin: addPluginNode(); break;
            default: break;
            }
        });
}

void CanvasComponent::showNodePropertiesDialog(int node_index)
{
    if (doc_ == nullptr) return;
    if (node_index < 0 || node_index >= (int) nodes_.size()) return;

    // Map the canvas node index to the appropriate doc spec.
    const int n_in   = (int) doc_->inputs.size();
    const int n_plug = (int) doc_->plugins.size();
    const int n_mix  = (int) doc_->mixes.size();

    auto* aw = new juce::AlertWindow(
        "Node properties",
        "",  // populated below per-kind
        juce::AlertWindow::QuestionIcon);

    enum Kind { K_INPUT, K_PLUGIN, K_MIX, K_OUTPUT };
    Kind kind;
    int  spec_index;
    if      (node_index < n_in)                          { kind = K_INPUT;  spec_index = node_index; }
    else if (node_index < n_in + n_plug)                 { kind = K_PLUGIN; spec_index = node_index - n_in; }
    else if (node_index < n_in + n_plug + n_mix)         { kind = K_MIX;    spec_index = node_index - n_in - n_plug; }
    else                                                  { kind = K_OUTPUT; spec_index = node_index - n_in - n_plug - n_mix; }

    if      (kind == K_INPUT)  aw->setMessage("Input node");
    else if (kind == K_PLUGIN) aw->setMessage("Plugin node (path read-only here; delete + re-add to change)");
    else if (kind == K_MIX)    aw->setMessage("Mix node");
    else                       aw->setMessage("Output node");

    // Common: id
    juce::String current_id;
    if      (kind == K_INPUT)  current_id = doc_->inputs[(size_t)  spec_index].id;
    else if (kind == K_PLUGIN) current_id = doc_->plugins[(size_t) spec_index].id;
    else if (kind == K_MIX)    current_id = doc_->mixes[(size_t)   spec_index].id;
    else                       current_id = doc_->outputs[(size_t) spec_index].id;
    aw->addTextEditor("id", current_id, "id");

    // Per-kind fields
    if (kind == K_INPUT)
    {
        const auto& s = doc_->inputs[(size_t) spec_index];
        aw->addTextEditor("channels", juce::String(s.channels), "channels");
        aw->addTextEditor("source",   s.source.getFullPathName(), "source path");
    }
    else if (kind == K_OUTPUT)
    {
        const auto& s = doc_->outputs[(size_t) spec_index];
        aw->addTextEditor("channels",  juce::String(s.channels),  "channels");
        aw->addTextEditor("bit_depth", juce::String(s.bit_depth), "bit depth (16/24/32)");
        aw->addTextEditor("sink",      s.sink.getFullPathName(),  "sink path");
    }
    else if (kind == K_MIX)
    {
        const auto& s = doc_->mixes[(size_t) spec_index];
        aw->addTextEditor("num_inputs", juce::String(s.num_inputs), "num_inputs");
        aw->addTextEditor("channels",   juce::String(s.channels),   "channels");
        juce::String gain_csv;
        for (size_t i = 0; i < s.gains.size(); ++i)
        {
            if (i) gain_csv += ",";
            gain_csv += juce::String(s.gains[i]);
        }
        aw->addTextEditor("gains", gain_csv, "gains (comma-separated)");
    }
    else // K_PLUGIN
    {
        const auto& s = doc_->plugins[(size_t) spec_index];
        aw->addTextEditor("path",
                          s.path.getFullPathName(),
                          "plugin path");
        aw->getTextEditor("path")->setReadOnly(true);
        aw->addTextEditor("state_b64_size",
                          juce::String((int) s.state_b64.length()),
                          "state_b64 length (read-only)");
        aw->getTextEditor("state_b64_size")->setReadOnly(true);
        aw->addTextEditor("receives_midi",
                          s.receives_midi ? "1" : "0",
                          "receives MIDI input (1/0)");
    }

    aw->addButton("OK",     1, juce::KeyPress(juce::KeyPress::returnKey));
    aw->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));

    aw->enterModalState(true,
        juce::ModalCallbackFunction::create(
            [this, aw, kind, spec_index](int result) {
                if (result != 1) { delete aw; return; }
                if (doc_ == nullptr) { delete aw; return; }

                const auto new_id =
                    aw->getTextEditorContents("id").trim();
                if (new_id.isEmpty()) { delete aw; return; }

                auto rename_in_edges = [&](const juce::String& old_id,
                                           const juce::String& nid) {
                    for (auto& e : doc_->edges)
                    {
                        if (e.src == old_id) e.src = nid;
                        if (e.dst == old_id) e.dst = nid;
                    }
                    auto it = doc_->layout.find(old_id.toStdString());
                    if (it != doc_->layout.end())
                    {
                        doc_->layout[nid.toStdString()] = it->second;
                        doc_->layout.erase(it);
                    }
                };

                if (kind == K_INPUT)
                {
                    auto& s = doc_->inputs[(size_t) spec_index];
                    if (s.id != new_id) { rename_in_edges(s.id, new_id); s.id = new_id; }
                    s.channels = aw->getTextEditorContents("channels").getIntValue();
                    s.source   = juce::File(
                        aw->getTextEditorContents("source"));
                }
                else if (kind == K_OUTPUT)
                {
                    auto& s = doc_->outputs[(size_t) spec_index];
                    if (s.id != new_id) { rename_in_edges(s.id, new_id); s.id = new_id; }
                    s.channels  = aw->getTextEditorContents("channels").getIntValue();
                    s.bit_depth = aw->getTextEditorContents("bit_depth").getIntValue();
                    s.sink      = juce::File(
                        aw->getTextEditorContents("sink"));
                }
                else if (kind == K_MIX)
                {
                    auto& s = doc_->mixes[(size_t) spec_index];
                    if (s.id != new_id) { rename_in_edges(s.id, new_id); s.id = new_id; }
                    const int new_n = std::max(1,
                        aw->getTextEditorContents("num_inputs").getIntValue());
                    s.channels = std::max(1,
                        aw->getTextEditorContents("channels").getIntValue());
                    // Parse gains CSV; pad/truncate to new_n with 1.0.
                    juce::StringArray gain_strs;
                    gain_strs.addTokens(aw->getTextEditorContents("gains"),
                                        ",", "");
                    std::vector<float> new_gains((size_t) new_n, 1.0f);
                    for (int i = 0;
                         i < std::min(new_n, gain_strs.size()); ++i)
                        new_gains[(size_t) i]
                            = gain_strs[i].trim().getFloatValue();
                    s.gains = std::move(new_gains);
                    // Drop any edges referencing ports >= new_n.
                    if (new_n < s.num_inputs)
                    {
                        const juce::String mix_id = s.id;
                        doc_->edges.erase(
                            std::remove_if(
                                doc_->edges.begin(), doc_->edges.end(),
                                [&](const project::EdgeSpec& e) {
                                    return e.dst == mix_id
                                        && e.dst_port >= new_n;
                                }),
                            doc_->edges.end());
                    }
                    s.num_inputs = new_n;
                }
                else // K_PLUGIN
                {
                    auto& s = doc_->plugins[(size_t) spec_index];
                    if (s.id != new_id) { rename_in_edges(s.id, new_id); s.id = new_id; }
                    s.receives_midi
                        = aw->getTextEditorContents("receives_midi")
                              .getIntValue() != 0;
                    // path/state_b64 are read-only in this dialog
                }
                rebuildLayout();
                repaint();
                delete aw;
            }),
        /*deleteWhenDismissed=*/false);
}

void CanvasComponent::addInputNode()
{
    chooser_ = std::make_unique<juce::FileChooser>(
        "Choose an input WAV/FLAC source",
        juce::File(),
        "*.wav;*.flac;*.mp3;*.ogg");
    const int flags = juce::FileBrowserComponent::openMode
                    | juce::FileBrowserComponent::canSelectFiles;
    chooser_->launchAsync(flags,
        [this](const juce::FileChooser& fc) {
            const auto file = fc.getResult();
            if (file == juce::File() || doc_ == nullptr) return;

            // Probe the file to take its actual channel count + sr.
            char err[256] = {0};
            MH_AudioFileInfo info{};
            if (!mh_audio_get_file_info(file.getFullPathName().toRawUTF8(),
                                        &info, err, sizeof(err)))
            {
                juce::AlertWindow::showMessageBoxAsync(
                    juce::AlertWindow::WarningIcon,
                    "Failed to read audio file",
                    juce::String(err));
                return;
            }
            if ((int) info.sample_rate != doc_->sample_rate)
            {
                juce::AlertWindow::showMessageBoxAsync(
                    juce::AlertWindow::WarningIcon,
                    "Sample rate mismatch",
                    "File: " + juce::String((int) info.sample_rate)
                  + " Hz, project: " + juce::String(doc_->sample_rate)
                  + " Hz. The render will fail until rates match.");
            }
            project::InputNodeSpec n;
            n.id       = generateUniqueId("in");
            n.channels = (int) info.channels;
            n.source   = file;
            doc_->inputs.push_back(std::move(n));
            rebuildLayout();
            repaint();
        });
}

void CanvasComponent::addOutputNode()
{
    chooser_ = std::make_unique<juce::FileChooser>(
        "Choose an output WAV/FLAC file",
        juce::File(),
        "*.wav;*.flac");
    const int flags = juce::FileBrowserComponent::saveMode
                    | juce::FileBrowserComponent::canSelectFiles
                    | juce::FileBrowserComponent::warnAboutOverwriting;
    chooser_->launchAsync(flags,
        [this](const juce::FileChooser& fc) {
            const auto file = fc.getResult();
            if (file == juce::File() || doc_ == nullptr) return;

            project::OutputNodeSpec n;
            n.id        = generateUniqueId("out");
            n.channels  = 2;     // sensible default; user can edit JSON
            n.bit_depth = 24;
            n.sink      = file;
            doc_->outputs.push_back(std::move(n));
            rebuildLayout();
            repaint();
        });
}

juce::String CanvasComponent::generateUniqueId(const juce::String& prefix) const
{
    if (doc_ == nullptr) return prefix + "_1";
    std::unordered_set<std::string> used;
    for (const auto& n : doc_->inputs)  used.insert(n.id.toStdString());
    for (const auto& n : doc_->outputs) used.insert(n.id.toStdString());
    for (const auto& n : doc_->plugins) used.insert(n.id.toStdString());
    for (const auto& n : doc_->mixes)   used.insert(n.id.toStdString());
    for (int i = 1; i < 1000; ++i)
    {
        const auto candidate
            = (prefix + "_" + juce::String(i)).toStdString();
        if (used.find(candidate) == used.end())
            return juce::String(candidate);
    }
    return prefix + "_x";
}

void CanvasComponent::addMixNode(int num_inputs, int channels)
{
    if (doc_ == nullptr) return;
    project::MixNodeSpec m;
    m.id         = generateUniqueId("mix");
    m.num_inputs = num_inputs;
    m.channels   = channels;
    m.gains.assign((size_t) num_inputs, 1.0f);
    doc_->mixes.push_back(std::move(m));
    rebuildLayout();
    repaint();
}

void CanvasComponent::addPluginNode()
{
    chooser_ = std::make_unique<juce::FileChooser>(
        "Choose a plugin",
        juce::File("/Library/Audio/Plug-Ins"),
        "*.vst3;*.component;*.lv2");
    const int flags = juce::FileBrowserComponent::openMode
                    | juce::FileBrowserComponent::canSelectFiles
                    | juce::FileBrowserComponent::canSelectDirectories;
    chooser_->launchAsync(flags,
        [this](const juce::FileChooser& fc) {
            const auto file = fc.getResult();
            if (file == juce::File() || doc_ == nullptr) return;
            project::PluginNodeSpec p;
            p.id   = generateUniqueId("fx");
            p.path = file;
            // Probe channel counts so canvas edges can validate. Use
            // the project's sample_rate / block_size so prepareToPlay
            // matches what loadProject will request later.
            char err[256] = {0};
            MH_Plugin* probe = mh_open(file.getFullPathName().toRawUTF8(),
                                       (double) doc_->sample_rate,
                                       doc_->block_size,
                                       /*req_in=*/0, /*req_out=*/0,
                                       err, sizeof(err));
            if (probe != nullptr)
            {
                MH_Info info{};
                if (mh_get_info(probe, &info))
                {
                    p.probed_in_channels  = info.num_input_ch;
                    p.probed_out_channels = info.num_output_ch;
                }
                mh_close(probe);
            }
            else
            {
                juce::AlertWindow::showMessageBoxAsync(
                    juce::AlertWindow::WarningIcon,
                    "Could not probe plugin",
                    juce::String("mh_open failed: ")
                        + juce::String(static_cast<const char*>(err))
                        + "\n\nAdding the node anyway; channel "
                          "validation at connect time will be disabled "
                          "until the next load.");
            }
            doc_->plugins.push_back(std::move(p));
            rebuildLayout();
            repaint();
        });
}

} // namespace minihost_desktop
