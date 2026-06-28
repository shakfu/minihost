// canvas.cpp -- see canvas.h for design.

#include "canvas.h"

#include "minihost_audiofile.h"
#include "node_registry.h"

#include <algorithm>
#include <cmath>
#include <unordered_set>

namespace minihost_desktop {

namespace {

juce::Colour kindColour(const juce::String& kind)
{
    if (const auto* entry = findKind(kind)) return entry->colour;
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

    // Walk every kind in canonical registry order; each entry's
    // canvas_info supplies the node label + port counts.
    for (const auto& entry : nodeRegistry())
    {
        const int c = entry.count(*doc_);
        for (int i = 0; i < c; ++i)
        {
            const auto info = entry.canvas_info(*doc_, i);
            NodeLayout n;
            n.id    = entry.id_at(*doc_, i);
            n.kind  = entry.kind_string;
            n.label = info.label;
            n.num_input_ports  = info.num_input_ports;
            n.num_output_ports = info.num_output_ports;
            n.bounds = juce::Rectangle<float>(0, 0, kNodeWidth, kNodeHeight);
            nodes_.push_back(std::move(n));
        }
    }

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
                         (int) i, e.kind };
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
        const bool is_sel  = ((int) i == selected_edge_);
        const bool is_midi = (e.kind == project::EdgeKind::Midi);
        const auto base_col = is_midi ? juce::Colour(0xffb98be0)   // lilac
                                      : juce::Colour(0xffb0b0b0);
        g.setColour(is_sel ? juce::Colours::yellow : base_col);
        juce::PathStrokeType stroke(is_sel ? 2.5f : 1.6f);
        if (is_midi)
        {
            // Dashed stroke to read as MIDI even on grayscale displays.
            const float dashes[] = { 6.0f, 4.0f };
            stroke.createDashedStroke(path, path, dashes, 2);
        }
        g.strokePath(path, stroke);
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

        // Meter overlay: vertical per-channel level bars across the
        // bottom of the node. Always draws an empty frame so the
        // user can identify the node as a meter even when no audio
        // is flowing (or before Live starts). Inside the frame,
        // bars fill bottom-up proportional to the lock-free peak
        // atomic. Atomic stores happen on the audio thread; this
        // read uses relaxed ordering.
        if (n.kind == "meter" && doc_ != nullptr)
        {
            // Find this meter in the doc; cache its channel count
            // so the empty frame uses the configured channel count
            // even when Live isn't running.
            int meter_idx = -1;
            for (int mi = 0; mi < (int) doc_->meters.size(); ++mi)
                if (doc_->meters[(size_t) mi].id == n.id)
                { meter_idx = mi; break; }
            const int ch = (meter_idx >= 0)
                ? doc_->meters[(size_t) meter_idx].channels : 2;

            // Carve out the bottom 55% of the node for the meter
            // frame. Use a dark base so empty cells are visible
            // even against the slate-grey meter node colour.
            auto box = n.bounds.reduced(8.0f);
            box = box.removeFromBottom(box.getHeight() * 0.55f);
            g.setColour(juce::Colour(0xff121212));
            g.fillRect(box);

            const float bar_w = box.getWidth() / std::max(1, ch);
            // Faint per-channel column separators inside the frame.
            g.setColour(juce::Colour(0xff2a2a2a));
            for (int c = 0; c <= ch; ++c)
                g.drawVerticalLine((int) (box.getX() + c * bar_w),
                                   box.getY(), box.getBottom());

            // Filled bars from current peaks. Skipped silently when
            // Live isn't running (live_project_ is null).
            if (live_project_ != nullptr
                && meter_idx >= 0
                && meter_idx < (int) live_project_->meter_states.size())
            {
                const auto* st
                    = live_project_->meter_states[(size_t) meter_idx].get();
                for (int c = 0; c < ch; ++c)
                {
                    const float peak = st->peak[(size_t) c]
                                          .load(std::memory_order_relaxed);
                    const float vis  = std::sqrt(std::min(1.0f, peak));
                    if (vis <= 0.0f) continue;
                    const float h = box.getHeight() * vis;
                    juce::Rectangle<float> bar(
                        box.getX() + c * bar_w + 1.0f,
                        box.getBottom() - h,
                        bar_w - 2.0f,
                        h);
                    const juce::Colour col
                        = vis < 0.7f ? juce::Colour(0xff3aa84a)    // green
                        : vis < 0.9f ? juce::Colour(0xffd0c020)    // yellow
                        :              juce::Colour(0xffd03030);   // red
                    g.setColour(col);
                    g.fillRect(bar);
                }
            }

            // Frame outline so the meter region reads as a box.
            g.setColour(juce::Colour(0xff404040));
            g.drawRect(box, 1.0f);
        }

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

    // Sweep every kind's spec vector for an entry matching this id;
    // at most one will hit since ids are unique across the doc.
    for (const auto& entry : nodeRegistry())
        entry.erase_by_id(doc, id);
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

    // If either endpoint is a MIDI node, this is a MIDI edge. Audio
    // channel validation is skipped; the graph compiler enforces
    // MIDI capability at load time. MIDI processors and merges are
    // both source AND dst capable.
    // MIDI source/sink classification comes straight from the
    // registry; adding a new MIDI-capable kind needs nothing here.
    const auto* src_entry = findKind(src_n.kind);
    const auto* dst_entry = findKind(dst_n.kind);
    const bool src_is_midi = src_entry && src_entry->is_midi_source;
    const bool dst_is_midi = dst_entry && dst_entry->is_midi_sink;
    if (src_is_midi || dst_is_midi)
    {
        if (!src_is_midi && src_n.kind != "plugin")
        {
            juce::AlertWindow::showMessageBoxAsync(
                juce::AlertWindow::WarningIcon,
                "MIDI edge requires a MIDI source",
                "Connect from a MIDI source (input, clock, processor, "
                "merge, or MIDI-producing plugin) into a MIDI sink "
                "(output, processor, merge, or MIDI-accepting plugin).");
            return;
        }
        // For midi_merge, pick the lowest unconnected dst_port; for
        // other dst kinds use port 0 (single MIDI input port).
        int chosen_port = 0;
        if (dst_n.kind == "midi_merge")
        {
            // Find the merge spec to know num_inputs.
            int num_inputs = 1;
            for (const auto& mm : doc_->midi_merges)
                if (mm.id == dst_n.id) { num_inputs = mm.num_inputs; break; }
            chosen_port = -1;
            for (int p = 0; p < num_inputs; ++p)
            {
                bool used = false;
                for (const auto& e : doc_->edges)
                    if (e.kind == project::EdgeKind::Midi
                        && e.dst == dst_n.id && e.dst_port == p)
                    { used = true; break; }
                if (!used) { chosen_port = p; break; }
            }
            if (chosen_port < 0)
            {
                juce::AlertWindow::showMessageBoxAsync(
                    juce::AlertWindow::WarningIcon,
                    "Merge full",
                    "All input ports on this midi_merge are already "
                    "connected. Edit the merge to increase num_inputs.");
                return;
            }
        }
        else
        {
            // Single-port consumer: replace any existing edge at port 0.
            const auto dst_id = dst_n.id;
            doc_->edges.erase(
                std::remove_if(doc_->edges.begin(), doc_->edges.end(),
                    [&](const project::EdgeSpec& e) {
                        return e.dst == dst_id
                            && e.kind == project::EdgeKind::Midi
                            && e.dst_port == 0;
                    }),
                doc_->edges.end());
        }
        project::EdgeSpec e;
        e.src      = src_n.id;
        e.dst      = dst_n.id;
        e.dst_port = chosen_port;
        e.kind     = project::EdgeKind::Midi;
        doc_->edges.push_back(std::move(e));
        rebuildLayout();
        return;
    }

    // Connect-time channel validation. For non-plugin kinds the
    // channel count is on the doc spec; for plugin nodes use the
    // cached probe (-1 means "unknown", skip validation). Order must
    // match rebuildLayout's section order.
    // Channel count for the canvas node at `idx`. The registry's
    // channels_for hook is asymmetric (pick_channel / merge_channels
    // return different values for src vs dst), so we pass through
    // is_output_port.
    auto channels_for = [&](int idx, bool is_output_port) -> int {
        const auto lk = mapCanvasIndex(*doc_, idx);
        return lk.entry ? lk.entry->channels_for(*doc_, lk.spec_index,
                                                 is_output_port)
                        : -1;
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
    // Hard-coded item ids for entries that need special handling
    // (file choosers, multi-node convenience helpers). Everything
    // else is registry-driven: any NodeKindEntry with a non-empty
    // menu_label gets a menu item that calls entry.menu_add(*doc_).
    enum {
        kAddInput  = 1,
        kAddOutput,
        kAddPlugin,
        kAddSplitStereo,
        kRegistryBase = 1000,
    };

    juce::PopupMenu m;
    m.addItem(kAddInput,  "Add Input...");
    m.addItem(kAddOutput, "Add Output...");
    m.addItem(kAddPlugin, "Add Plugin...");
    m.addSeparator();
    m.addItem(kAddSplitStereo,
              "Add Channel Split (stereo -> 2 mono)");
    m.addSeparator();
    for (int i = 0; i < (int) nodeRegistry().size(); ++i)
    {
        const auto& entry = nodeRegistry()[(size_t) i];
        if (entry.menu_label.isNotEmpty() && entry.menu_add)
            m.addItem(kRegistryBase + i, entry.menu_label);
    }

    juce::PopupMenu::Options opts;
    opts = opts.withTargetScreenArea({ screen_pos, screen_pos });
    m.showMenuAsync(opts,
        [this](int chosen) {
            if (chosen == kAddInput)        { addInputNode();          return; }
            if (chosen == kAddOutput)       { addOutputNode();         return; }
            if (chosen == kAddPlugin)       { addPluginNode();         return; }
            if (chosen == kAddSplitStereo)  { addChannelSplitStereo(); return; }
            const int reg_idx = chosen - kRegistryBase;
            if (reg_idx >= 0 && reg_idx < (int) nodeRegistry().size())
            {
                const auto& entry = nodeRegistry()[(size_t) reg_idx];
                if (entry.menu_add && doc_ != nullptr)
                {
                    entry.menu_add(*doc_);
                    rebuildLayout();
                    repaint();
                }
            }
        });
}

void CanvasComponent::setLiveProject(project::LoadedProject* lp)
{
    live_project_ = lp;
    if (lp != nullptr && doc_ != nullptr && !doc_->meters.empty())
        startTimerHz(30);
    else
        stopTimer();
    repaint();
}

void CanvasComponent::showNodePropertiesDialog(int node_index)
{
    if (doc_ == nullptr) return;
    if (node_index < 0 || node_index >= (int) nodes_.size()) return;

    // Registry-driven: the entry's dialog_emit / dialog_apply hooks
    // know which fields to show + how to write them back.
    const auto lookup = mapCanvasIndex(*doc_, node_index);
    if (lookup.entry == nullptr) return;
    const auto& entry = *lookup.entry;
    const int   spec_index = lookup.spec_index;

    auto* aw = new juce::AlertWindow(
        "Node properties",
        entry.dialog_title(*doc_, spec_index),
        juce::AlertWindow::QuestionIcon);

    // Every kind shares an editable id field.
    aw->addTextEditor("id", entry.id_at(*doc_, spec_index), "id");
    // Per-kind fields.
    entry.dialog_emit(*doc_, spec_index, *aw);

    aw->addButton("OK",     1, juce::KeyPress(juce::KeyPress::returnKey));
    aw->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));

    aw->enterModalState(true,
        juce::ModalCallbackFunction::create(
            [this, aw, &entry, spec_index](int result) {
                if (result != 1) { delete aw; return; }
                if (doc_ == nullptr) { delete aw; return; }

                const auto new_id = aw->getTextEditorContents("id").trim();
                if (new_id.isEmpty()) { delete aw; return; }

                NodeKindEntry::RenameFn rename =
                    [this](const juce::String& old_id,
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

                entry.dialog_apply(*doc_, spec_index, *aw, new_id, rename);
                rebuildLayout();
                repaint();
                delete aw;
            }),
        /*deleteWhenDismissed=*/false);
}

void CanvasComponent::addChannelSplitStereo()
{
    if (doc_ == nullptr) return;
    // "Channel split" is two pick_channel nodes (one per channel).
    // No grouped doc-level entity -- they're independent specs.
    auto unique_id = [&](const juce::String& prefix) -> juce::String {
        int n = 1;
        juce::String id;
        do { id = prefix + juce::String(n++); }
        while (std::any_of(doc_->pick_channels.begin(),
                           doc_->pick_channels.end(),
                           [&](const project::PickChannelNodeSpec& s) {
                               return s.id == id;
                           }));
        return id;
    };
    for (int c = 0; c < 2; ++c)
    {
        project::PickChannelNodeSpec s;
        s.id = unique_id(c == 0 ? "L" : "R");
        s.in_channels   = 2;
        s.channel_index = c;
        doc_->pick_channels.push_back(std::move(s));
    }
    rebuildLayout();
    repaint();
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
            // Default to "fx" prefix; we'll switch to "synth" below
            // once the probe tells us whether it's an instrument.
            juce::String id_prefix = "fx";
            if (probe != nullptr)
            {
                MH_Info info{};
                if (mh_get_info(probe, &info))
                {
                    p.probed_in_channels  = info.num_input_ch;
                    p.probed_out_channels = info.num_output_ch;
                    p.accepts_midi        = info.accepts_midi != 0;
                    p.produces_midi       = info.produces_midi != 0;
                    // Heuristic: an instrument has no audio input
                    // (or only MIDI-driven output). Naming reflects
                    // that so the canvas reads as "synth_1" instead
                    // of "fx_1".
                    if (info.num_input_ch == 0 || info.is_midi_effect)
                        id_prefix = "synth";
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
            p.id = generateUniqueId(id_prefix);
            doc_->plugins.push_back(std::move(p));
            rebuildLayout();
            repaint();
        });
}

} // namespace minihost_desktop
