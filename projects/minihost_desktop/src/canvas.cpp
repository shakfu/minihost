// canvas.cpp -- see canvas.h for design.

#include "canvas.h"
#include "chain_library.h"
#include "quick_add.h"
#include "sequencer.h"

#include "minihost_audiofile.h"
#include "node_registry.h"

#include <algorithm>
#include <cmath>

namespace minihost_desktop {

namespace {

juce::Colour kindColour(const juce::String& kind)
{
    if (const auto* entry = findKind(kind)) return entry->colour;
    return juce::Colour(0xff8a9bb0);
}

const juce::Colour kCanvasBg   (0xff1a1b1e);
const juce::Colour kGridDot    (0xff2c2e33);
const juce::Colour kNodeBody   (0xff2a2c31);
const juce::Colour kWell       (0xff141518);  // meter and sequencer strips
const juce::Colour kSubtitle   (0xffaab0b8);
const juce::Colour kPort       (0xffd4d8de);
const juce::Colour kMidiColour (0xffb98be0);  // MIDI edges, ports, nodes
const juce::Colour kSelection  (0xffffffff);
const juce::Colour kProblem    (0xffe5534b);
const juce::Colour kInsert     (0xff4fc3f7);  // cable a dragged node will join
constexpr float    kCorner     = 6.0f;

bool hasExtension(const juce::String& path, const juce::StringArray& exts)
{
    return exts.contains(juce::File(path).getFileExtension().toLowerCase());
}

const juce::StringArray& audioExtensions()
{ static const juce::StringArray e{ ".wav", ".flac", ".mp3", ".ogg" }; return e; }
const juce::StringArray& midiExtensions()
{ static const juce::StringArray e{ ".mid", ".midi" }; return e; }
const juce::StringArray& pluginExtensions()
{ static const juce::StringArray e{ ".vst3", ".component", ".lv2" }; return e; }

// Clears a flag for a scope: rebuilds that restore or reparse a document
// must not move its nodes into the open chain.
struct FlagOff
{
    explicit FlagOff(bool& f) : f_(f), old_(f) { f_ = false; }
    ~FlagOff() { f_ = old_; }
    bool& f_;
    bool  old_;
};

// "fx_12" -> "fx"; the prefix for a pasted node's fresh id.
juce::String idStem(const juce::String& id, const juce::String& fallback)
{
    auto stem = id.trimCharactersAtEnd("0123456789").trimCharactersAtEnd("_");
    return stem.isEmpty() ? fallback : stem;
}

} // namespace

// Invisible stand-in for one painted node: a focusable, accessible child
// at the node's bounds. It takes no mouse clicks; the canvas handles those.
class NodeProxy : public juce::Component
{
public:
    NodeProxy(CanvasComponent& c, juce::String id) : canvas_(c), id_(std::move(id))
    {
        setInterceptsMouseClicks(false, false);
        setWantsKeyboardFocus(true);
        setAccessible(true);
    }

    const juce::String& id() const { return id_; }

    void focusGained(FocusChangeType) override
    {
        if (canvas_.selection_ != juce::StringArray{ id_ })
            canvas_.selectOnly(id_);
    }

    bool keyPressed(const juce::KeyPress& key) override
    { return canvas_.keyPressed(key); }

    std::unique_ptr<juce::AccessibilityHandler> createAccessibilityHandler() override
    {
        using T = juce::AccessibilityActionType;
        juce::AccessibilityActions actions;
        actions.addAction(T::press, [this] {
            canvas_.selectOnly(id_);
            canvas_.keyPressed(juce::KeyPress(juce::KeyPress::returnKey));
        });
        actions.addAction(T::showMenu, [this] {
            const int i = canvas_.indexOf(id_);
            if (i >= 0) canvas_.showNodeMenu(i, getScreenBounds().getCentre());
        });
        actions.addAction(T::focus, [this] { grabKeyboardFocus(); });
        return std::make_unique<juce::AccessibilityHandler>(
            *this, juce::AccessibilityRole::group, std::move(actions));
    }

private:
    CanvasComponent& canvas_;
    juce::String     id_;
};

CanvasComponent::CanvasComponent()
{
    setOpaque(true);
    setWantsKeyboardFocus(true);
    setFocusContainerType(FocusContainerType::keyboardFocusContainer);
    setTitle("Project graph");
    setDescription("Nodes and the cables between them. Tab moves between nodes.");
}

CanvasComponent::~CanvasComponent() = default;

CanvasComponent::Clipboard& CanvasComponent::clipboard()
{
    static Clipboard c;
    return c;
}

void CanvasComponent::setDocument(project::ProjectDocument* doc)
{
    doc_ = doc;
    scope_.clear();
    drag_kind_      = DragKind::None;
    dragging_node_  = -1;
    selection_.clear();
    selected_edge_  = -1;
    add_at_.reset();
    pending_plugin_at_.reset();
    known_ids_.clear();
    quick_add_.reset();
    // A different document starts with a clean history; the old undo
    // stack refers to the previous document's state.
    undo_history_.clear();
    // An edit notification still pending belongs to the old document.
    cancelPendingUpdate();
    rebuildLayout();
    zoom_   = 1.0f;
    offset_ = {};
    fit_pending_ = true;
    resized();
    repaint();
    notifySelection();
}

void CanvasComponent::recordUndo()
{
    if (doc_ == nullptr) return;
    if (undo_group_depth_ > 0)
    {
        if (undo_group_recorded_) return;
        undo_group_recorded_ = true;
    }
    undo_history_.snapshot(*doc_);
    triggerAsyncUpdate();
}

bool CanvasComponent::undo()
{
    if (doc_ == nullptr) return false;
    if (!undo_history_.undo(*doc_)) return false;
    FlagOff no_adopt(adopting_);
    selected_edge_ = -1;
    drag_kind_     = DragKind::None;
    dragging_node_ = -1;
    rebuildLayout();
    repaint();
    if (on_document_edited_) on_document_edited_();
    notifySelection();
    return true;
}

bool CanvasComponent::redo()
{
    if (doc_ == nullptr) return false;
    if (!undo_history_.redo(*doc_)) return false;
    FlagOff no_adopt(adopting_);
    selected_edge_ = -1;
    drag_kind_     = DragKind::None;
    dragging_node_ = -1;
    rebuildLayout();
    repaint();
    if (on_document_edited_) on_document_edited_();
    notifySelection();
    return true;
}

void CanvasComponent::rebuildLayout()
{
    nodes_.clear();
    edges_.clear();
    if (doc_ == nullptr) { syncProxies(); return; }
    if (std::none_of(doc_->chains.begin(), doc_->chains.end(),
                     [this](const project::ChainSpec& c) { return c.id == scope_; }))
        scope_.clear();

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
            n.midi_in          = info.midi_input_port;
            if (chains::isPortKind(n.kind)) n.port_dir = n.kind.endsWith("inlet") ? 1 : -1;
            const int slots = n.num_input_ports + (n.midi_in ? 1 : 0);
            n.bounds = juce::Rectangle<float>(
                0, 0, kNodeWidth,
                std::max(kNodeHeight, kHeaderHeight + kPortPitch * (float) slots + 6.0f));
            nodes_.push_back(std::move(n));
        }
    }

    // At the top, each chain is one box; its port nodes are the box's
    // pins, top to bottom as placed inside it (chains::pinY).
    const int registry_count = (int) nodes_.size();
    for (int i = 0; i < registry_count; ++i)
        nodes_[(size_t) i].shown = chainOf(nodes_[(size_t) i].id) == scope_;
    for (const auto& c : doc_->chains)
    {
        if (scope_.isNotEmpty()) break;
        NodeLayout b;
        bool any = false;
        for (int i = 0; i < registry_count; ++i)
        {
            const auto& m = nodes_[(size_t) i];
            if (chainOf(m.id) != c.id) continue;
            any = true;
            if (m.port_dir != 0) (m.port_dir > 0 ? b.ins : b.outs).push_back(i);
        }
        if (!any) continue;
        for (auto* list : { &b.ins, &b.outs })
            std::stable_sort(list->begin(), list->end(), [this](int x, int y) {
                return chains::pinY(doc_->layout, nodes_[(size_t) x].id)
                     < chains::pinY(doc_->layout, nodes_[(size_t) y].id);
            });
        b.id    = c.id;
        b.kind  = "chain";
        b.label = c.id + "\n(" + (c.source.isNotEmpty() ? "chain " + c.source : juce::String("chain")) + ")";
        const int box = (int) nodes_.size();
        for (size_t k = 0; k < b.ins.size(); ++k)  { nodes_[(size_t) b.ins[k]].box = box;  nodes_[(size_t) b.ins[k]].slot = (int) k; }
        for (size_t k = 0; k < b.outs.size(); ++k) { nodes_[(size_t) b.outs[k]].box = box; nodes_[(size_t) b.outs[k]].slot = (int) k; }
        const auto slots = (float) std::max(b.ins.size(), b.outs.size());
        b.bounds = { 0, 0, kNodeWidth, std::max(kNodeHeight, kHeaderHeight + kPortPitch * slots + 6.0f) };
        nodes_.push_back(std::move(b));
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
                         (int) i, e.kind, {} };
        edges_.push_back(edge);
    }

    // Nodes this canvas has not shown before: added by an edit (not a
    // freshly loaded document). They go where the user asked, if they
    // did, and become the selection.
    juce::StringArray fresh;
    if (!known_ids_.empty())
        for (const auto& n : nodes_)
            if (std::find(known_ids_.begin(), known_ids_.end(), n.id) == known_ids_.end())
                fresh.add(n.id);
    if (add_at_.has_value())
    {
        for (int k = 0; k < fresh.size(); ++k)
            doc_->layout[fresh[k].toStdString()] = project::NodePosition{
                add_at_->x + 24.0f * (float) k, add_at_->y + 24.0f * (float) k };
        add_at_.reset();
    }
    if (adopting_ && scope_.isNotEmpty())
    {
        juce::StringArray stray;
        for (const auto& id : fresh)
            if (chainOf(id) != scope_) stray.add(id);
        if (!stray.isEmpty()) { adoptIntoScope(stray); return; }
    }

    autoPositionNodes();
    findProblems();

    // Draw the edges of the open scope: at the top, those that reach a
    // chain's pins but not those inside it.
    auto drawable = [this](int i) { return nodes_[(size_t) i].shown || nodes_[(size_t) i].box >= 0; };
    edges_.erase(std::remove_if(edges_.begin(), edges_.end(), [&](const EdgeLayout& e) {
        const auto c = chainOf(nodes_[(size_t) e.src_node_index].id);
        return !drawable(e.src_node_index) || !drawable(e.dst_node_index)
            || (scope_.isEmpty() && c.isNotEmpty() && c == chainOf(nodes_[(size_t) e.dst_node_index].id));
    }), edges_.end());

    known_ids_.clear();
    known_ids_.push_back({});  // non-empty marks "a document is shown"
    for (const auto& n : nodes_) known_ids_.push_back(n.id);

    // Keep selected ids that still exist; select what was just added.
    juce::StringArray sel;
    for (const auto& id : selection_)
        if (indexOf(id) >= 0) sel.add(id);
    if (!fresh.isEmpty()) sel = fresh;
    const bool changed = sel != selection_;
    selection_ = sel;
    if (selected_edge_ >= (int) edges_.size()) selected_edge_ = -1;
    syncProxies();
    if (changed) notifySelection();
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

    // Saved positions first; the rest take the next free slot in their
    // column, so a node added to a laid-out project does not land on one.
    std::vector<bool> placed(N, false);
    for (int i = 0; i < N && doc_ != nullptr; ++i)
    {
        auto it = doc_->layout.find(nodes_[(size_t) i].id.toStdString());
        if (it == doc_->layout.end()) continue;
        nodes_[(size_t) i].bounds.setPosition(it->second.x, it->second.y);
        placed[(size_t) i] = true;
    }
    auto occupied = [&](const juce::Rectangle<float>& r) {
        for (int j = 0; j < N; ++j)
            if (placed[(size_t) j]
                && nodes_[(size_t) j].bounds.expanded(8.0f).intersects(r))
                return true;
        return false;
    };
    std::unordered_map<int, int> next_row_for_column;
    for (int i = 0; i < N; ++i)
    {
        if (placed[(size_t) i] || nodes_[(size_t) i].kind == "chain") continue;
        auto& b = nodes_[(size_t) i].bounds;
        const int col = column[(size_t) i];
        int& row = next_row_for_column[col];
        do
            b.setPosition(kCanvasPadding + col * kColumnWidth,
                          kCanvasPadding + row++ * kRowHeight);
        while (occupied(b));
        placed[(size_t) i] = true;
    }
    // A chain box without a saved place goes where its nodes are.
    for (int i = 0; i < N; ++i)
    {
        if (placed[(size_t) i]) continue;
        juce::Rectangle<float> r;
        for (const auto& m : nodes_)
            if (chainOf(m.id) == nodes_[(size_t) i].id)
                r = r.isEmpty() ? m.bounds : r.getUnion(m.bounds);
        nodes_[(size_t) i].bounds.setPosition(r.isEmpty() ? juce::Point<float>(kCanvasPadding, kCanvasPadding)
                                                          : r.getPosition());
    }
}

void CanvasComponent::findProblems()
{
    // The graph compiler requires every input port of a non-plugin node
    // to be fed (plugins get silence on an unwired port), and an audio
    // edge's channel counts to match.
    auto channels = [&](int idx, bool out) {
        const auto lk = mapCanvasIndex(*doc_, idx);
        return lk.entry ? lk.entry->channels_for(*doc_, lk.spec_index, out) : -1;
    };
    for (int i = 0; i < (int) nodes_.size(); ++i)
    {
        auto& n = nodes_[(size_t) i];
        if (chains::isPortKind(n.kind) && chainOf(n.id).isEmpty())
            n.problems.add("A port works only inside a chain.");
        // Plugins and chain ports with no cable in get silence.
        if (n.kind == "plugin" || n.port_dir != 0) continue;
        for (int port = 0; port < n.num_input_ports; ++port)
        {
            const bool fed = std::any_of(edges_.begin(), edges_.end(),
                [&](const EdgeLayout& e) { return e.dst_node_index == i && e.dst_port == port; });
            if (fed) continue;
            n.missing_inputs.push_back(port);
            n.problems.add(n.num_input_ports == 1
                ? juce::String("Input is not connected.")
                : "Input " + juce::String(port + 1) + " is not connected.");
        }
    }
    for (auto& e : edges_)
    {
        if (e.kind != project::EdgeKind::Audio) continue;
        const int out_ch = channels(e.src_node_index, true);
        const int in_ch  = channels(e.dst_node_index, false);
        if (out_ch < 0 || in_ch < 0 || out_ch == in_ch) continue;
        e.problem = juce::String(out_ch) + "ch > " + juce::String(in_ch) + "ch";
        auto& dst = nodes_[(size_t) e.dst_node_index];
        dst.problems.add(nodes_[(size_t) e.src_node_index].id + " sends "
                         + juce::String(out_ch) + " channels; this input takes "
                         + juce::String(in_ch) + ".");
    }
    // A chain box lists its nodes' problems.
    for (auto& b : nodes_)
        if (b.kind == "chain")
            for (const auto& m : nodes_)
                if (chainOf(m.id) == b.id)
                    for (const auto& line : m.problems)
                        b.problems.add(m.id.fromFirstOccurrenceOf("/", false, false) + ": " + line);
}

void CanvasComponent::resized()
{
    if (fit_pending_ && !getLocalBounds().isEmpty())
    {
        fit_pending_ = false;
        // Saved layouts open at 100% when they fit; only zoom out when not.
        const auto content = contentBounds().toFloat();
        if (content.getRight() > (float) getWidth()
            || content.getBottom() > (float) getHeight())
            fitToView();
    }
    syncProxies();
}

void CanvasComponent::fitToView()
{
    juce::Rectangle<float> r;
    for (const auto& n : nodes_)
        if (n.shown) r = r.isEmpty() ? n.bounds : r.getUnion(n.bounds);
    if (r.isEmpty() || getLocalBounds().isEmpty()) { resetZoom(); return; }
    r = r.expanded(kCanvasPadding);
    zoom_ = juce::jlimit(kMinZoom, 1.0f,
                         std::min((float) getWidth() / r.getWidth(),
                                  (float) getHeight() / r.getHeight()));
    offset_ = getLocalBounds().getCentre().toFloat() - r.getCentre() * zoom_;
    syncProxies();
    repaint();
}

void CanvasComponent::zoomBy(float factor, juce::Point<float> anchor)
{
    const auto c = toCanvas(anchor);
    zoom_   = juce::jlimit(kMinZoom, kMaxZoom, zoom_ * factor);
    offset_ = anchor - c * zoom_;
    syncProxies();
    repaint();
}

void CanvasComponent::resetZoom()
{
    zoom_   = 1.0f;
    offset_ = {};
    syncProxies();
    repaint();
}

int CanvasComponent::indexOf(const juce::String& id) const
{
    for (int i = 0; i < (int) nodes_.size(); ++i)
        if (nodes_[(size_t) i].id == id) return i;
    return -1;
}

juce::Path CanvasComponent::edgePath(const EdgeLayout& e) const
{
    const auto p0 = outPos(e.src_node_index);
    const auto p1 = edgeEndPos(e);
    juce::Point<float> c0, c1;
    edgeCubic(p0, p1, c0, c1);
    juce::Path path;
    path.startNewSubPath(p0);
    path.cubicTo(c0, c1, p1);
    return path;
}

void CanvasComponent::paint(juce::Graphics& g)
{
    g.fillAll(kCanvasBg);

    if (doc_ == nullptr)
    {
        g.setColour(juce::Colours::grey);
        g.setFont(juce::FontOptions(14.0f));
        g.drawText("No project loaded.",
                   getLocalBounds(), juce::Justification::centred, false);
        return;
    }

    // Dot grid, so panning and zooming have a visible reference.
    if (zoom_ >= 0.5f)
    {
        g.setColour(kGridDot);
        const float step = kGrid * zoom_;
        const float x0 = std::fmod(offset_.x, step), y0 = std::fmod(offset_.y, step);
        for (float y = y0; y < (float) getHeight(); y += step)
            for (float x = x0; x < (float) getWidth(); x += step)
                g.fillRect(x, y, 1.5f, 1.5f);
    }

    if (nodes_.empty())
    {
        g.setColour(kSubtitle);
        g.setFont(juce::FontOptions(14.0f));
        g.drawText("Empty project. Double-click or press Cmd+K to add a node, "
                   "or drop audio, MIDI or plugin files here.",
                   getLocalBounds(), juce::Justification::centred, false);
        return;
    }

    g.addTransform(viewTransform());

    // Edges (under nodes).
    for (size_t i = 0; i < edges_.size(); ++i)
    {
        const auto& e = edges_[i];
        auto path = edgePath(e);
        const bool is_sel    = ((int) i == selected_edge_);
        const bool is_midi   = (e.kind == project::EdgeKind::Midi);
        const bool is_insert = ((int) i == insert_target_);
        auto col = is_midi ? kMidiColour : juce::Colour(0xff8f949c);
        if (e.problem.isNotEmpty()) col = kProblem;
        if (is_insert)              col = kInsert;
        if (is_sel)                 col = kSelection;
        g.setColour(col);
        juce::PathStrokeType stroke(is_sel || is_insert ? 2.5f : 1.6f);
        if (is_midi)
        {
            // Dashed stroke to read as MIDI even on grayscale displays.
            const float dashes[] = { 6.0f, 4.0f };
            stroke.createDashedStroke(path, path, dashes, 2);
        }
        g.strokePath(path, stroke);

        if (e.problem.isNotEmpty())
        {
            const auto mid = path.getPointAlongPath(path.getLength() * 0.5f);
            const juce::Rectangle<float> tag(mid.x - 32.0f, mid.y - 8.0f, 64.0f, 16.0f);
            g.setColour(kProblem);
            g.fillRoundedRectangle(tag, 8.0f);
            g.setColour(juce::Colours::white);
            g.setFont(juce::FontOptions(10.5f, juce::Font::bold));
            g.drawText(e.problem, tag, juce::Justification::centred, false);
        }
    }

    // In-progress connect drag.
    if (drag_kind_ == DragKind::ConnectingFromPort
        && connect_from_node_ >= 0)
    {
        const auto p0 = outPos(connect_from_node_);
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

    // Nodes: dark body, category-coloured header band, id as title, the
    // kind's detail as subtitle.
    for (int i = 0; i < (int) nodes_.size(); ++i)
    {
        const auto& n = nodes_[(size_t) i];
        if (!n.shown) continue;
        const auto accent = kindColour(n.kind);
        if (n.kind == "chain")
        {
            // A second outline behind the box marks a container.
            g.setColour(juce::Colour(0xff4a4d55));
            g.drawRoundedRectangle(n.bounds.translated(4.0f, 4.0f), kCorner, 1.0f);
        }
        g.setColour(kNodeBody);
        g.fillRoundedRectangle(n.bounds, kCorner);

        auto header = n.bounds.withHeight(kHeaderHeight);
        {
            juce::Graphics::ScopedSaveState s(g);
            g.reduceClipRegion(header.toNearestInt());
            g.setColour(accent.withMultipliedBrightness(0.55f));
            g.fillRoundedRectangle(n.bounds, kCorner);
        }
        g.setColour(accent);
        g.fillRect(header.removeFromBottom(2.0f));

        const bool sel = isSelected(i);
        g.setColour(sel ? kSelection : juce::Colour(0xff4a4d55));
        g.drawRoundedRectangle(n.bounds, kCorner, sel ? 2.0f : 1.0f);

        // label is "id\n(detail)" or "id\nplugin name". Inside a chain
        // the id is shown without the chain's prefix.
        auto title = n.label.upToFirstOccurrenceOf("\n", false, false);
        if (scope_.isNotEmpty()) title = title.fromFirstOccurrenceOf(scope_ + "/", false, false);
        const auto text  = n.bounds.reduced(10.0f, 0.0f);
        auto title_area  = text.withHeight(kHeaderHeight);
        if (!n.problems.isEmpty())
        {
            // "!" badge: the node would make the render fail.
            const auto badge = title_area.removeFromRight(14.0f)
                                         .withSizeKeepingCentre(12.0f, 12.0f);
            g.setColour(kProblem);
            g.fillEllipse(badge);
            g.setColour(juce::Colours::white);
            g.setFont(juce::FontOptions(10.0f, juce::Font::bold));
            g.drawText("!", badge, juce::Justification::centred, false);
        }
        g.setColour(juce::Colours::white);
        g.setFont(juce::FontOptions(12.0f, juce::Font::bold));
        g.drawFittedText(title, title_area.toNearestInt(),
                         juce::Justification::centredLeft, 1, 0.85f);
        auto sub_area = text.withTrimmedTop(kHeaderHeight + 2.0f).withHeight(14.0f);
        if (n.kind == "plugin")
        {
            // Latency (when any) and, while Live runs, CPU share.
            juce::StringArray stats;
            const int lat = latencyOf(n.id);
            if (lat > 0) stats.add(juce::String(lat) + " smp");
            const float load = loadOf(n.id);
            if (load >= 0.0f) stats.add(juce::String(juce::roundToInt(load * 100.0f)) + "%");
            if (automationLanesOf(n.id) > 0) stats.add("auto");
            if (!stats.isEmpty())
            {
                const auto s = stats.joinIntoString(" ");
                g.setColour(load >= 0.5f ? kProblem : kSubtitle.withAlpha(0.8f));
                g.setFont(juce::FontOptions(10.0f));
                g.drawText(s, sub_area.removeFromRight(6.0f * (float) s.length() + 4.0f),
                           juce::Justification::centredRight, false);
            }
        }
        g.setColour(kSubtitle);
        g.setFont(juce::FontOptions(11.0f));
        g.drawFittedText(subtitleOf(n.id), sub_area.toNearestInt(),
                         juce::Justification::centredLeft, 1, 0.85f);

        if (n.kind == "meter")     paintMeter(g, n);
        if (n.kind == "sequencer") paintSequencerSteps(g, n);

        juce::Colour act_col;
        juce::String act_tag;
        if (nodeActivity(n, act_col, act_tag))
        {
            g.setColour(act_col);
            g.drawRoundedRectangle(n.bounds.expanded(3.0f), kCorner + 2.0f, 2.5f);
            // Tag pill straddling the top edge.
            const float w = 12.0f + (float) act_tag.length() * 7.0f;
            const juce::Rectangle<float> pill(n.bounds.getRight() - w - 6.0f,
                                              n.bounds.getY() - 9.0f, w, 16.0f);
            g.fillRoundedRectangle(pill, 8.0f);
            g.setColour(juce::Colours::white);
            g.setFont(juce::FontOptions(11.0f, juce::Font::bold));
            g.drawText(act_tag, pill, juce::Justification::centred, false);
        }

        // Ports: a dark ring keeps them visible against an edge's end. An
        // unconnected required input is a red ring.
        auto port = [&g](juce::Point<float> p, juce::Colour c, bool square) {
            const auto r = juce::Rectangle<float>(kPortRadius * 2, kPortRadius * 2)
                               .withCentre(p);
            g.setColour(c);
            if (square) g.fillRect(r); else g.fillEllipse(r);
            g.setColour(kCanvasBg);
            if (square) g.drawRect(r, 1.5f); else g.drawEllipse(r, 1.5f);
        };
        for (int k = 0; k < n.num_input_ports && drawsInput(n); ++k)
        {
            const bool missing = std::find(n.missing_inputs.begin(), n.missing_inputs.end(), k)
                              != n.missing_inputs.end();
            port(inputPortPos(n, k), missing ? kProblem : kPort, false);
        }
        if (drawsOutput(n))
            port(outputPortPos(n), kPort, false);
        if (n.midi_in)
            port(inputPortPos(n, kMidiPort), kMidiColour, true);
        // A chain's pins: MIDI square, an unfed inlet red.
        for (int side = 0; side < 2; ++side)
        {
            const auto& pins = side == 0 ? n.ins : n.outs;
            for (size_t k = 0; k < pins.size(); ++k)
            {
                const auto& pn = nodes_[(size_t) pins[k]];
                const bool midi = pn.kind.startsWith("midi_");
                port(boxPinPos(n, (int) k, side == 1),
                     midi ? kMidiColour : kPort, midi);
            }
        }
    }

    if (drag_kind_ == DragKind::Lasso && !lasso_.isEmpty())
    {
        g.setColour(kInsert.withAlpha(0.12f));
        g.fillRect(lasso_);
        g.setColour(kInsert.withAlpha(0.7f));
        g.drawRect(lasso_, 1.0f / zoom_);
    }

    if (scope_.isNotEmpty())
    {
        // The open chain's bar, in component coordinates.
        g.addTransform(viewTransform().inverted());
        auto bar = scopeBar();
        g.setColour(juce::Colour(0xf0222428));
        g.fillRect(bar);
        g.setColour(juce::Colour(0xff4a4d55));
        g.drawLine(bar.getX(), bar.getBottom(), bar.getRight(), bar.getBottom());
        juce::String source;
        for (const auto& c : doc_->chains) if (c.id == scope_) source = c.source;
        g.setFont(juce::FontOptions(12.0f, juce::Font::bold));
        g.setColour(kInsert);
        g.drawText("< Project", bar.removeFromLeft(80.0f).withTrimmedLeft(10.0f),
                   juce::Justification::centredLeft, false);
        g.setColour(juce::Colours::white);
        g.drawText("/  chain " + scope_ + (source.isNotEmpty() && source != scope_ ? "  (" + source + ")" : juce::String())
                       + "      Esc closes", bar, juce::Justification::centredLeft, true);
    }
}
juce::var CanvasComponent::nodeBounds() const
{
    auto* o = new juce::DynamicObject();
    for (const auto& n : nodes_)
        if (n.shown) o->setProperty(n.id, juce::Array<juce::var>{ n.bounds.getX(), n.bounds.getY(),
                                                     n.bounds.getWidth(),
                                                     n.bounds.getHeight() });
    return juce::var(o);
}

juce::Rectangle<int> CanvasComponent::contentBounds() const
{
    juce::Rectangle<float> r;
    for (const auto& n : nodes_)
        if (n.shown) r = r.isEmpty() ? n.bounds : r.getUnion(n.bounds);
    if (r.isEmpty()) return {};
    return r.withPosition(0.0f, 0.0f)
            .withSize(r.getRight() + kCanvasPadding, r.getBottom() + kCanvasPadding)
            .getSmallestIntegerContainer();
}

juce::Point<float> CanvasComponent::outputPortPos(const NodeLayout& n) const
{
    return { n.bounds.getRight(), n.bounds.withTrimmedTop(kHeaderHeight).getCentreY() };
}

juce::Point<float> CanvasComponent::boxPinPos(const NodeLayout& box, int slot, bool out) const
{
    const auto body  = box.bounds.withTrimmedTop(kHeaderHeight);
    const int  slots = (int) (out ? box.outs.size() : box.ins.size());
    return { out ? body.getRight() : body.getX(),
             body.getCentreY() + kPortPitch * ((float) slot - (float) (slots - 1) / 2.0f) };
}

juce::Point<float> CanvasComponent::outPos(int i) const
{
    const auto& n = nodes_[(size_t) i];
    return n.box >= 0 ? boxPinPos(nodes_[(size_t) n.box], n.slot, true) : outputPortPos(n);
}

juce::Point<float> CanvasComponent::edgeEndPos(const EdgeLayout& e) const
{
    const auto& dst = nodes_[(size_t) e.dst_node_index];
    if (dst.box >= 0) return boxPinPos(nodes_[(size_t) dst.box], dst.slot, false);
    const bool to_midi = e.kind == project::EdgeKind::Midi && dst.midi_in;
    return inputPortPos(dst, to_midi ? kMidiPort : e.dst_port);
}

juce::Point<float> CanvasComponent::inputPortPos(const NodeLayout& n,
                                                 int port) const
{
    // Audio ports, then the MIDI port, kPortPitch apart around the body's
    // centre.
    const auto body  = n.bounds.withTrimmedTop(kHeaderHeight);
    const int  slots = n.num_input_ports + (n.midi_in ? 1 : 0);
    const int  slot  = port == kMidiPort ? slots - 1 : port;
    return { body.getX(),
             body.getCentreY() + kPortPitch * ((float) slot - (float) (slots - 1) / 2.0f) };
}

void CanvasComponent::edgeCubic(juce::Point<float> p0, juce::Point<float> p1,
                                juce::Point<float>& c0, juce::Point<float>& c1) const
{
    const float dx = std::max(40.0f, std::abs(p1.x - p0.x) * 0.4f);
    c0 = { p0.x + dx, p0.y };
    c1 = { p1.x - dx, p1.y };
}

void CanvasComponent::feedMeter(const juce::String& id, int channel, float peak, double now)
{
    auto& m = meters_[id];
    const auto c = (size_t) channel;
    if (m.level_db.size() <= c)
    {
        m.level_db.resize(c + 1, kMeterFloorDb);
        m.hold_db.resize(c + 1, kMeterFloorDb);
        m.hold_at.resize(c + 1, 0.0);
        m.last_at.resize(c + 1, now);
    }
    const float db = juce::Decibels::gainToDecibels(peak, kMeterFloorDb);
    const float dt = (float) std::max(0.0, now - m.last_at[c]);
    m.last_at[c]  = now;
    m.level_db[c] = std::max(db, std::max(kMeterFloorDb, m.level_db[c] - 24.0f * dt));
    if (db >= m.hold_db[c] || now - m.hold_at[c] > 1.5)
    {
        m.hold_db[c] = std::max(db, m.level_db[c]);
        m.hold_at[c] = now;
    }
    if (peak >= 1.0f) m.clip = true;
}

void CanvasComponent::paintMeter(juce::Graphics& g, const NodeLayout& n) const
{
    int ch = 2;
    for (const auto& m : doc_->meters)
        if (m.id == n.id) ch = m.channels;
    auto box = nodeStrip(n);
    const auto it = meters_.find(n.id);
    const MeterView* mv = it == meters_.end() ? nullptr : &it->second;

    // Clip light at the right end of the strip.
    const auto light = box.removeFromRight(box.getHeight()).reduced(0.5f);
    box.removeFromRight(2.0f);
    g.setColour(mv != nullptr && mv->clip ? kProblem : kWell);
    g.fillRect(light);

    g.setColour(kWell);
    g.fillRect(box);
    auto frac = [](float db) {
        return juce::jlimit(0.0f, 1.0f, (db - kMeterFloorDb) / -kMeterFloorDb);
    };
    const float row_h = box.getHeight() / (float) std::max(1, ch);
    for (int c = 0; mv != nullptr && c < std::min(ch, (int) mv->level_db.size()); ++c)
    {
        const float db = mv->level_db[(size_t) c];
        const float y  = box.getY() + (float) c * row_h + 0.5f;
        const float h  = std::max(1.0f, row_h - 1.0f);
        if (db > kMeterFloorDb)
        {
            g.setColour(db < -12.0f ? juce::Colour(0xff3aa84a)     // green
                      : db < -3.0f  ? juce::Colour(0xffd0c020)     // yellow
                      :               juce::Colour(0xffd03030));   // red
            g.fillRect(box.getX(), y, box.getWidth() * frac(db), h);
        }
        const float hold = mv->hold_db[(size_t) c];
        if (hold > kMeterFloorDb)
        {
            g.setColour(juce::Colours::white);
            g.fillRect(box.getX() + box.getWidth() * frac(hold) - 1.0f, y, 1.5f, h);
        }
    }
    g.setColour(juce::Colour(0xff4a4d55));
    g.drawRect(box, 1.0f);

    // Highest hold, as a number, on the subtitle line.
    if (mv != nullptr && !mv->hold_db.empty())
    {
        const float top = *std::max_element(mv->hold_db.begin(), mv->hold_db.end());
        const auto text = mv->clip ? juce::String("CLIP")
                        : top <= kMeterFloorDb ? juce::String("-inf dB")
                        : juce::String(top, 1) + " dB";
        g.setColour(mv->clip ? kProblem : kSubtitle);
        g.setFont(juce::FontOptions(10.0f));
        g.drawText(text, n.bounds.reduced(10.0f, 0.0f)
                             .withTrimmedTop(kHeaderHeight + 2.0f).withHeight(14.0f),
                   juce::Justification::centredRight, false);
    }
}

void CanvasComponent::paintSequencerSteps(juce::Graphics& g,
                                          const NodeLayout& n) const
{
    const project::SequencerParams* p = nullptr;
    for (const auto& s : doc_->sequencers)
        if (s.id == n.id) p = &s.params;
    if (p == nullptr) return;

    const bool running = activity_.live && activity_.playing;
    const double bpm  = activity_.bpm > 0.0 ? activity_.bpm : doc_->effectiveBpm();
    const double step = p->rate * 60.0 * doc_->sample_rate / bpm;
    const long long cur = running && step >= 1.0
                        ? (long long) ((double) activity_.position / step) : -1;
    const long long pass_start = cur >= 0 ? (cur / p->steps) * p->steps : 0;

    const auto strip = nodeStrip(n);
    g.setColour(kWell);
    g.fillRect(strip);
    const float w = strip.getWidth() / (float) p->steps;
    for (int j = 0; j < p->steps; ++j)
    {
        const auto st = project::sequencerStep(*p, pass_start + j);
        juce::Rectangle<float> cell(strip.getX() + j * w + 0.5f, strip.getY() + 1.0f,
                                    std::max(1.0f, w - 1.0f), strip.getHeight() - 2.0f);
        if (st.on)
        {
            g.setColour(juce::Colour(0xff9ad0a8).withAlpha(0.4f + 0.6f * st.velocity / 127.0f));
            g.fillRect(cell);
        }
        if (pass_start + j == cur)
        {
            g.setColour(kSelection);
            g.drawRect(cell.expanded(0.5f), 1.0f);
        }
    }
}

bool CanvasComponent::nodeActivity(const NodeLayout& n, juce::Colour& colour,
                                   juce::String& tag) const
{
    if (doc_ == nullptr) return false;
    const juce::Colour green(0xff3aa84a), red(0xffd03030), blue(0xff3a78c8);
    if (n.kind == "input")
    {
        for (const auto& in : doc_->inputs)
        {
            if (in.id != n.id) continue;
            if (activity_.live && activity_.playing)
            {
                // Past the end of a non-looping file the input is silent.
                if (!activity_.looping && live_project_ != nullptr)
                {
                    const auto& li = live_project_->doc.inputs;
                    for (size_t k = 0; k < li.size(); ++k)
                        if (li[k].id == in.id
                            && activity_.position >= live_project_->input_frames[k])
                        {
                            colour = juce::Colour(0xff808080);
                            tag = "ENDED";
                            return true;
                        }
                }
                colour = green;
                tag = activity_.looping ? "LOOPING" : "PLAYING";
                return true;
            }
            if (activity_.preview != juce::File() && activity_.preview == in.source)
            { colour = blue; tag = "PLAY FILE"; return true; }
        }
        return false;
    }
    if (n.kind == "midi_input" && activity_.live && activity_.playing)
    {
        // Only nodes playing a .mid file; device MIDI has no transport.
        if (live_project_ == nullptr) return false;
        for (const auto& fmi : live_project_->file_midi_inputs)
        {
            if (fmi.id != n.id) continue;
            if (!activity_.looping && !fmi.events.empty()
                && activity_.position > fmi.events.back().sample_offset)
            { colour = juce::Colour(0xff808080); tag = "ENDED"; return true; }
            colour = green;
            tag = activity_.looping ? "LOOPING" : "PLAYING";
            return true;
        }
        return false;
    }
    if (n.kind == "output")
    {
        for (const auto& on : doc_->outputs)
        {
            if (on.id != n.id) continue;
            if (activity_.recording)
            { colour = red; tag = "REC"; return true; }
            if (activity_.preview != juce::File() && activity_.preview == on.sink)
            { colour = blue; tag = "PLAY FILE"; return true; }
        }
        return false;
    }
    if (n.kind == "sequencer" && activity_.live && activity_.playing)
    {
        colour = green;
        tag = activity_.looping ? "LOOPING" : "PLAYING";
        return true;
    }
    if (n.kind == "device_output" && activity_.live)
    {
        colour = activity_.playing ? green : juce::Colour(0xff808080);
        tag    = activity_.playing ? "OUT" : "LIVE";
        return true;
    }
    return false;
}

int CanvasComponent::hitTestNode(juce::Point<float> p) const
{
    for (int i = (int) nodes_.size() - 1; i >= 0; --i)
        if (nodes_[(size_t) i].shown && nodes_[(size_t) i].bounds.contains(p))
            return i;
    return -1;
}

int CanvasComponent::hitTestOutputPort(juce::Point<float> p) const
{
    for (int i = (int) nodes_.size() - 1; i >= 0; --i)
    {
        const auto& n = nodes_[(size_t) i];
        if (!n.shown) continue;
        for (size_t k = 0; k < n.outs.size(); ++k)
            if (p.getDistanceFrom(boxPinPos(n, (int) k, true)) <= kPortHitRadius) return n.outs[k];
        if (drawsOutput(n) && p.getDistanceFrom(outputPortPos(n)) <= kPortHitRadius) return i;
    }
    return -1;
}

juce::String CanvasComponent::pinTooltip(juce::Point<float> p) const
{
    int i = hitTestOutputPort(p);
    if (i < 0 || nodes_[(size_t) i].box < 0) i = hitTestInputPort(p).first;
    if (i < 0 || nodes_[(size_t) i].box < 0) return {};
    const auto& n = nodes_[(size_t) i];
    return n.id.fromFirstOccurrenceOf("/", false, false) + " "
         + n.label.fromFirstOccurrenceOf("\n", false, false);
}

juce::String CanvasComponent::getTooltip()
{
    return pinTooltip(toCanvas(getMouseXYRelative().toFloat()));
}

std::pair<int, int> CanvasComponent::hitTestInputPort(juce::Point<float> p) const
{
    for (int i = (int) nodes_.size() - 1; i >= 0; --i)
    {
        const auto& n = nodes_[(size_t) i];
        if (!n.shown) continue;
        for (size_t k = 0; k < n.ins.size(); ++k)
            if (p.getDistanceFrom(boxPinPos(n, (int) k, false)) <= kPortHitRadius)
                return { n.ins[k], 0 };
        for (int port = 0; port < n.num_input_ports && drawsInput(n); ++port)
        {
            const auto pp = inputPortPos(n, port);
            if (p.getDistanceFrom(pp) <= kPortHitRadius)
                return { i, port };
        }
        if (n.midi_in
            && p.getDistanceFrom(inputPortPos(n, kMidiPort)) <= kPortHitRadius)
            return { i, kMidiPort };
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
        const auto p0 = outPos(e.src_node_index);
        const auto p1 = edgeEndPos(e);
        juce::Point<float> c0, c1;
        edgeCubic(p0, p1, c0, c1);
        juce::Path path;
        path.startNewSubPath(p0);
        path.cubicTo(c0, c1, p1);

        const auto bounds = path.getBounds().expanded(tolerance + 2.0f);
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

int CanvasComponent::insertTargetFor(int node_index) const
{
    const auto& n = nodes_[(size_t) node_index];
    if (n.num_output_ports <= 0 || n.port_dir != 0) return -1;
    for (const auto& e : edges_)
        if (e.src_node_index == node_index || e.dst_node_index == node_index)
            return -1;  // only a node with no cables can be inserted
    const auto* entry = findKind(n.kind);
    const bool midi_processor = entry != nullptr && n.kind != "plugin"
                             && entry->is_midi_source && entry->is_midi_sink;
    const bool audio_through  = !midi_processor && n.num_input_ports > 0;
    if (!midi_processor && !audio_through) return -1;

    const auto centre = n.bounds.getCentre();
    const int  e = hitTestEdge(centre, std::min(24.0f, n.bounds.getHeight() * 0.5f));
    if (e < 0) return -1;
    const bool midi_edge = edges_[(size_t) e].kind == project::EdgeKind::Midi;
    return midi_edge == midi_processor ? e : -1;
}

bool CanvasComponent::insertOnEdge(int node_index, int edge_index)
{
    if (doc_ == nullptr || edge_index < 0 || edge_index >= (int) edges_.size())
        return false;
    const auto e    = edges_[(size_t) edge_index];
    const auto& n   = nodes_[(size_t) node_index];
    const auto& src = nodes_[(size_t) e.src_node_index];
    const auto& dst = nodes_[(size_t) e.dst_node_index];

    if (e.kind == project::EdgeKind::Audio)
    {
        auto channels = [&](int idx, bool out) {
            const auto lk = mapCanvasIndex(*doc_, idx);
            return lk.entry ? lk.entry->channels_for(*doc_, lk.spec_index, out) : -1;
        };
        const int a = channels(e.src_node_index, true), b = channels(node_index, false);
        const int c = channels(node_index, true),       d = channels(e.dst_node_index, false);
        if ((a >= 0 && b >= 0 && a != b) || (c >= 0 && d >= 0 && c != d))
        {
            warn("Cannot insert " + n.id,
                 src.id + " -> " + dst.id + " carries " + juce::String(a)
                     + " channels; " + n.id + " takes " + juce::String(b)
                     + " and sends " + juce::String(c) + ".");
            return false;
        }
    }

    const int port = (e.kind == project::EdgeKind::Midi && dst.midi_in) ? kMidiPort
                                                                         : e.dst_port;
    const int src_i = e.src_node_index, dst_i = e.dst_node_index;
    UndoGroup group(*this);
    recordUndo();
    removeEdgeFromDoc(e.doc_edge_index);
    rebuildLayout();
    bool ok = addEdgeToDoc(src_i, node_index, 0);
    ok = ok && addEdgeToDoc(node_index, dst_i, port);
    return ok;
}

void CanvasComponent::mouseDown(const juce::MouseEvent& e)
{
    quick_add_.reset();
    grabKeyboardFocus();
    const auto p = toCanvas(e.position);
    if (scope_.isNotEmpty() && scopeBar().contains(e.position)) { closeChain(); return; }

    if (e.mods.isMiddleButtonDown() || (e.mods.isAltDown() && e.mods.isLeftButtonDown()))
    {
        drag_kind_        = DragKind::Panning;
        pan_start_offset_ = offset_;
        return;
    }

    if (e.mods.isPopupMenu())
    {
        // Right-click on a node shows the per-node menu; on empty
        // space, the add-node menu.
        const int hit = hitTestNode(p);
        if (hit >= 0)
        {
            if (!isSelected(hit)) selectOnly(nodes_[(size_t) hit].id);
            showNodeMenu(hit, e.getScreenPosition());
            return;
        }
        showContextMenu(e.getScreenPosition(), p);
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
            selected_edge_     = -1;
            repaint();
            return;
        }
    }

    // Node body -- select, then move the selection.
    const int hit = hitTestNode(p);
    if (hit >= 0)
    {
        const auto& id = nodes_[(size_t) hit].id;
        if (auto m = meters_.find(id); m != meters_.end()) m->second.clip = false;
        selected_edge_ = -1;
        if (e.mods.isShiftDown() || e.mods.isCommandDown())
        {
            auto sel = selection_;
            if (sel.contains(id)) { sel.removeString(id); setSelection(sel); repaint(); return; }
            sel.add(id);
            setSelection(sel);
        }
        else if (!isSelected(hit))
        {
            selectOnly(id);
        }
        startMove(hit, p);
        return;
    }

    // Edge?
    const int edge_hit = hitTestEdge(p);
    if (edge_hit >= 0)
    {
        selected_edge_ = edge_hit;
        setSelection({});
        drag_kind_     = DragKind::None;
        repaint();
        return;
    }

    // Empty space: rectangle selection.
    selected_edge_ = -1;
    lasso_base_    = e.mods.isShiftDown() ? selection_ : juce::StringArray();
    if (!e.mods.isShiftDown()) setSelection({});
    drag_kind_  = DragKind::Lasso;
    drag_start_ = p;
    lasso_      = {};
    repaint();
}

void CanvasComponent::mouseDrag(const juce::MouseEvent& e)
{
    const auto p = toCanvas(e.position);
    if (drag_kind_ == DragKind::MovingNodes)
    {
        const auto delta = p - drag_start_;
        for (const auto& [i, pos] : move_start_)
            nodes_[(size_t) i].bounds.setPosition(pos + delta);
        insert_target_ = move_start_.size() == 1 ? insertTargetFor(move_start_[0].first) : -1;
        repaint();
    }
    else if (drag_kind_ == DragKind::ConnectingFromPort)
    {
        connect_to_pos_ = p;
        repaint();
    }
    else if (drag_kind_ == DragKind::Lasso)
    {
        lasso_ = juce::Rectangle<float>(drag_start_, p);
        auto sel = lasso_base_;
        for (const auto& n : nodes_)
            if (n.shown && n.bounds.intersects(lasso_)) sel.addIfNotAlreadyThere(n.id);
        if (sel != selection_) setSelection(sel);
        repaint();
    }
    else if (drag_kind_ == DragKind::Panning)
    {
        offset_ = pan_start_offset_ + e.getOffsetFromDragStart().toFloat();
        repaint();
    }
}

void CanvasComponent::mouseUp(const juce::MouseEvent& e)
{
    if (drag_kind_ == DragKind::MovingNodes && doc_ != nullptr)
    {
        // A press-without-drag also enters MovingNodes (it is how a node
        // is selected); only record an undo step if a node moved, so
        // plain clicks don't pollute the history.
        const bool moved = std::any_of(move_start_.begin(), move_start_.end(),
            [&](const auto& m) { return nodes_[(size_t) m.first].bounds.getPosition() != m.second; });
        if (moved)
        {
            const int target = insert_target_;
            UndoGroup group(*this);
            recordUndo();
            for (const auto& [i, pos] : move_start_)
            {
                const auto& n = nodes_[(size_t) i];
                doc_->layout[n.id.toStdString()]
                    = project::NodePosition{ n.bounds.getX(), n.bounds.getY() };
            }
            if (target >= 0)
                insertOnEdge(move_start_[0].first, target);
        }
        if (auto* proxy = proxies_.count(nodes_[(size_t) dragging_node_].id) != 0
                              ? proxies_[nodes_[(size_t) dragging_node_].id].get() : nullptr;
            proxy != nullptr && selection_.size() == 1)
            proxy->grabKeyboardFocus();
    }
    else if (drag_kind_ == DragKind::ConnectingFromPort
             && connect_from_node_ >= 0)
    {
        auto [dst_idx, dst_port] = hitTestInputPort(toCanvas(e.position));
        if (dst_idx >= 0 && dst_idx != connect_from_node_)
            addEdgeToDoc(connect_from_node_, dst_idx, dst_port);
    }
    drag_kind_         = DragKind::None;
    dragging_node_     = -1;
    connect_from_node_ = -1;
    insert_target_     = -1;
    move_start_.clear();
    lasso_ = {};
    syncProxies();
    repaint();
}

void CanvasComponent::mouseDoubleClick(const juce::MouseEvent& e)
{
    if (doc_ == nullptr) return;
    const auto p = toCanvas(e.position);
    const int hit = hitTestNode(p);
    if (hit < 0)
    {
        if (hitTestEdge(p) < 0) showQuickAdd(p);
        return;
    }
    selectOnly(nodes_[(size_t) hit].id);
    if (nodes_[(size_t) hit].kind == "plugin")     openEditorFor(nodes_[(size_t) hit].id);
    else if (nodes_[(size_t) hit].kind == "chain") openChain(nodes_[(size_t) hit].id);
    else if (on_show_properties_)                   on_show_properties_();
}

void CanvasComponent::mouseWheelMove(const juce::MouseEvent& e,
                                     const juce::MouseWheelDetails& w)
{
    if (e.mods.isCommandDown() || e.mods.isCtrlDown())
    {
        zoomBy(std::pow(2.0f, w.deltaY * 2.0f), e.position);
        return;
    }
    // Same scale as juce::Viewport's wheel step.
    offset_ += juce::Point<float>(w.deltaX, w.deltaY) * 224.0f;
    syncProxies();
    repaint();
}

void CanvasComponent::mouseMagnify(const juce::MouseEvent& e, float scale)
{
    zoomBy(scale, e.position);
}

bool CanvasComponent::keyPressed(const juce::KeyPress& key)
{
    const auto mods = key.getModifiers();
    const int  code = key.getKeyCode();
    const bool cmd  = mods.isCommandDown();
    auto is = [&](char c) { return code == c || code == juce::CharacterFunctions::toLowerCase((juce::juce_wchar) c); };

    if (key == juce::KeyPress::deleteKey || key == juce::KeyPress::backspaceKey)
    { deleteSelection(); return true; }
    if (key == juce::KeyPress::escapeKey && scope_.isNotEmpty()
        && selection_.isEmpty() && selected_edge_ < 0)
    { closeChain(); return true; }
    if (cmd && key.isKeyCode(juce::KeyPress::upKey) && scope_.isNotEmpty())
    { closeChain(); return true; }
    if (key == juce::KeyPress::escapeKey)
    {
        selected_edge_ = -1;
        setSelection({});
        grabKeyboardFocus();
        repaint();
        return true;
    }
    if (key == juce::KeyPress::returnKey)
    {
        if (selection_.size() != 1) return false;
        if (kindOf(selection_[0]) == "plugin")     openEditorFor(selection_[0]);
        else if (kindOf(selection_[0]) == "chain") openChain(selection_[0]);
        else if (on_show_properties_)              on_show_properties_();
        return true;
    }
    // Cmd/Ctrl+Z = undo; Cmd/Ctrl+Shift+Z = redo. The command modifier
    // is Cmd on macOS, Ctrl elsewhere (JUCE maps it per-platform).
    if (cmd && is('Z'))
    {
        if (mods.isShiftDown()) redo();
        else                    undo();
        return true;
    }
    if (cmd && is('C')) { copySelection();      return true; }
    if (cmd && is('X')) { cutSelection();       return true; }
    if (cmd && is('V')) { paste();              return true; }
    if (cmd && is('D')) { duplicateSelection(); return true; }
    if (cmd && is('G')) { promptSaveAsChain();  return true; }
    if (cmd && is('A')) { selectAll();          return true; }
    if (cmd && is('K')) { showQuickAdd();       return true; }
    if (cmd && code == '0') { fitToView();      return true; }
    if (cmd && code == '1') { resetZoom();      return true; }
    if (cmd && (code == '=' || code == '+')) { zoomBy(1.25f);        return true; }
    if (cmd && code == '-')                  { zoomBy(1.0f / 1.25f); return true; }

    const float step = mods.isShiftDown() ? 1.0f : 10.0f;
    if (key.isKeyCode(juce::KeyPress::leftKey))  { nudgeSelection({ -step, 0 }); return true; }
    if (key.isKeyCode(juce::KeyPress::rightKey)) { nudgeSelection({  step, 0 }); return true; }
    if (key.isKeyCode(juce::KeyPress::upKey))    { nudgeSelection({ 0, -step }); return true; }
    if (key.isKeyCode(juce::KeyPress::downKey))  { nudgeSelection({ 0,  step }); return true; }
    return false;
}

void CanvasComponent::nudgeSelection(juce::Point<float> delta)
{
    if (doc_ == nullptr || selection_.isEmpty()) return;
    recordUndo();
    for (const auto& id : selection_)
    {
        const int i = indexOf(id);
        if (i < 0) continue;
        auto& b = nodes_[(size_t) i].bounds;
        b.translate(delta.x, delta.y);
        doc_->layout[id.toStdString()] = project::NodePosition{ b.getX(), b.getY() };
    }
    syncProxies();
    repaint();
}

// ----- selection ----- //

void CanvasComponent::setSelection(juce::StringArray ids)
{
    if (ids == selection_) return;
    selection_ = std::move(ids);
    repaint();
    notifySelection();
}

void CanvasComponent::selectOnly(const juce::String& id)
{
    selected_edge_ = -1;
    setSelection(id.isEmpty() ? juce::StringArray() : juce::StringArray{ id });
    auto it = proxies_.find(id);
    if (it != proxies_.end() && !it->second->hasKeyboardFocus(false) && isShowing())
        it->second->grabKeyboardFocus();
}

void CanvasComponent::selectAll()
{
    juce::StringArray all;
    for (const auto& n : nodes_) if (n.shown) all.add(n.id);
    selected_edge_ = -1;
    setSelection(all);
}

void CanvasComponent::deleteSelection()
{
    if (doc_ == nullptr) return;
    if (selected_edge_ >= 0 && selected_edge_ < (int) edges_.size())
    {
        recordUndo();
        removeEdgeFromDoc(edges_[(size_t) selected_edge_].doc_edge_index);
        selected_edge_ = -1;
        rebuildLayout();
        repaint();
        return;
    }
    if (selection_.isEmpty()) return;
    recordUndo();
    // removeNodeFromDoc reads nodes_, which stays as it is until the
    // rebuild below, so every index stays valid.
    // A chain box takes its nodes with it.
    for (const auto& id : selection_)
    {
        const int i = indexOf(id);
        if (i < 0) continue;
        if (nodes_[(size_t) i].kind != "chain") { removeNodeFromDoc(i); continue; }
        for (const auto& m : chainMembers(id)) removeNodeFromDoc(indexOf(m));
        doc_->layout.erase(id.toStdString());
    }
    doc_->chains.erase(std::remove_if(doc_->chains.begin(), doc_->chains.end(),
                                      [this](const project::ChainSpec& c) { return selection_.contains(c.id); }),
                       doc_->chains.end());
    selection_.clear();
    rebuildLayout();
    repaint();
    notifySelection();
}

// ----- clipboard ----- //

juce::var CanvasComponent::serializeNode(int node_index) const
{
    const auto& n = nodes_[(size_t) node_index];
    const auto* entry = findKind(n.kind);
    if (entry == nullptr) return {};
    juce::var found;
    NodeKindEntry::PushNodeFn push = [&](juce::DynamicObject* obj, const juce::String& kind,
                                         const juce::String& id) {
        juce::var v(obj);  // owns obj either way
        if (id != n.id) return;
        obj->setProperty("id", id);
        obj->setProperty("kind", kind);
        found = v;
    };
    entry->serialize_all(*doc_, push);
    return found;
}

void CanvasComponent::copySelection()
{
    if (doc_ == nullptr || selection_.isEmpty()) return;
    Clipboard c;
    for (int i = 0; i < (int) nodes_.size(); ++i)
    {
        if (!isSelected(i)) continue;
        juce::var v;
        if (nodes_[(size_t) i].kind == "chain")
        {
            // In library form, with the inner layout, under the box's id.
            try
            {
                v = chains::extract(serializedAsShown(), nodes_[(size_t) i].id, {});
                for (const auto& ch : doc_->chains)
                    if (ch.id == nodes_[(size_t) i].id) v.getDynamicObject()->setProperty("name", ch.source);
                v.getDynamicObject()->setProperty("id", nodes_[(size_t) i].id);
                v.getDynamicObject()->setProperty("kind", "chain");
            }
            catch (const std::exception&) { continue; }
        }
        else v = serializeNode(i);
        if (v.isVoid()) continue;
        c.nodes.add(v);
        const auto pos = nodes_[(size_t) i].bounds.getPosition();
        c.positions.add(juce::Array<juce::var>{ pos.x, pos.y });
    }
    // A selected box's pins count as selected.
    auto copied = [this](const juce::String& id) {
        return selection_.contains(id) || selection_.contains(chainOf(id));
    };
    for (const auto& e : doc_->edges)
    {
        if (!copied(e.src) || !copied(e.dst)) continue;
        auto* o = new juce::DynamicObject();
        o->setProperty("src", e.src);
        o->setProperty("dst", e.dst);
        o->setProperty("dst_port", e.dst_port);
        o->setProperty("midi", e.kind == project::EdgeKind::Midi);
        c.edges.add(juce::var(o));
    }
    clipboard() = std::move(c);
}

void CanvasComponent::cutSelection()
{
    copySelection();
    deleteSelection();
}

bool CanvasComponent::canPaste() const
{
    return doc_ != nullptr && !clipboard().nodes.isEmpty();
}

void CanvasComponent::paste(std::optional<juce::Point<float>> at)
{
    if (!canPaste()) return;
    auto& c = clipboard();

    juce::Point<float> top_left{ 1e9f, 1e9f };
    for (const auto& p : c.positions)
        top_left = { std::min(top_left.x, (float) p[0]), std::min(top_left.y, (float) p[1]) };
    ++c.pastes;
    const auto delta = at.has_value() ? *at - top_left
                                      : juce::Point<float>(24.0f, 24.0f) * (float) c.pastes;

    UndoGroup group(*this);
    recordUndo();
    std::map<juce::String, juce::String> renamed;  // old id -> new id, chains too
    juce::StringArray pasted;
    for (int k = 0; k < c.nodes.size(); ++k)
    {
        auto node = c.nodes[k];
        const auto old_id = node["id"].toString();
        const auto kind   = node["kind"].toString();
        const auto pos    = c.positions[k];
        const juce::Point<float> at{ (float) pos[0] + delta.x, (float) pos[1] + delta.y };
        if (scope_.isNotEmpty() && chains::forbiddenKind(kind))
        {
            warn("Cannot paste " + old_id, kind == "chain" ? juce::String("Chains cannot contain chains.")
                                                          : "A " + kind + " node cannot be inside a chain.");
            continue;
        }
        if (kind == "chain")
        {
            const auto cid = freeChainId(old_id);
            try
            {
                auto j = serializedAsShown();
                chains::insert(j, node, cid, at);
                FlagOff no_adopt(adopting_);
                *doc_ = project::parseProject(j, project_dir_, /*require_output=*/false);
            }
            catch (const std::exception& ex)
            {
                warn("Cannot paste " + old_id, ex.what());
                continue;
            }
            renamed[old_id] = cid;
            pasted.add(cid);
            continue;
        }
        const auto* entry = findKind(kind);
        if (entry == nullptr) continue;
        // The copy joins the open scope: "vox/fx_1" pasted at the top is "fx_1".
        const auto local  = old_id.fromLastOccurrenceOf("/", false, false);
        const auto new_id = idInUse(*doc_, scoped(local)) ? generateUniqueId(idStem(local, kind))
                                                          : scoped(local);
        try
        {
            entry->parse(node, new_id, *doc_, project_dir_);
        }
        catch (const std::exception& ex)
        {
            warn("Cannot paste " + old_id, ex.what());
            continue;
        }
        renamed[old_id] = new_id;
        pasted.add(new_id);
        doc_->layout[new_id.toStdString()] = project::NodePosition{ at.x, at.y };
    }
    // Edge ends inside a copied chain follow the chain's new id.
    auto map_id = [&](const juce::String& id) -> juce::String {
        if (auto it = renamed.find(id); it != renamed.end()) return it->second;
        const auto head = id.upToFirstOccurrenceOf("/", false, false);
        if (auto it = renamed.find(head); id.contains("/") && it != renamed.end())
            return it->second + "/" + id.fromFirstOccurrenceOf("/", false, false);
        return {};
    };
    for (const auto& ev : c.edges)
    {
        const auto s = map_id(ev["src"].toString()), d = map_id(ev["dst"].toString());
        if (s.isEmpty() || d.isEmpty()) continue;
        project::EdgeSpec e;
        e.src      = s;
        e.dst      = d;
        e.dst_port = (int) ev["dst_port"];
        e.kind     = (bool) ev["midi"] ? project::EdgeKind::Midi : project::EdgeKind::Audio;
        doc_->edges.push_back(std::move(e));
    }
    rebuildLayout();
    setSelection(pasted);
    repaint();
}

void CanvasComponent::duplicateSelection()
{
    // Duplicate keeps the clipboard as it was.
    auto saved = clipboard();
    copySelection();
    clipboard().pastes = 0;
    paste();
    clipboard() = std::move(saved);
}

// ----- accessibility ----- //

juce::String CanvasComponent::accessibleDescription(int node_index) const
{
    const auto& n = nodes_[(size_t) node_index];
    juce::StringArray parts{ kindOf(n.id) + ", " + subtitleOf(n.id) };
    juce::StringArray from, to;
    auto drawn = [this](int i) { const auto b = nodes_[(size_t) i].box; return b >= 0 ? b : i; };
    for (const auto& e : edges_)
    {
        if (drawn(e.dst_node_index) == node_index) from.add(nodes_[(size_t) drawn(e.src_node_index)].id);
        if (drawn(e.src_node_index) == node_index) to.add(nodes_[(size_t) drawn(e.dst_node_index)].id);
    }
    if (!from.isEmpty()) parts.add("Input from " + from.joinIntoString(", "));
    if (!to.isEmpty())   parts.add("Output to " + to.joinIntoString(", "));
    parts.addArray(n.problems);
    return parts.joinIntoString(". ");
}

void CanvasComponent::syncProxies()
{
    std::map<juce::String, std::unique_ptr<juce::Component>> keep;
    for (int i = 0; i < (int) nodes_.size(); ++i)
    {
        const auto& n = nodes_[(size_t) i];
        if (!n.shown) continue;
        auto it = proxies_.find(n.id);
        std::unique_ptr<juce::Component> p;
        if (it != proxies_.end()) p = std::move(it->second);
        else
        {
            p = std::make_unique<NodeProxy>(*this, n.id);
            addAndMakeVisible(*p);
        }
        p->setBounds(n.bounds.transformedBy(viewTransform()).getSmallestIntegerContainer());
        p->setExplicitFocusOrder(i + 1);
        p->setTitle(scope_.isEmpty() ? n.id : n.id.fromFirstOccurrenceOf(scope_ + "/", false, false));
        p->setDescription(accessibleDescription(i));
        keep[n.id] = std::move(p);
    }
    proxies_ = std::move(keep);  // proxies of removed nodes are deleted here
}

// ----- file drops ----- //

bool CanvasComponent::isInterestedInFileDrag(const juce::StringArray& files)
{
    if (doc_ == nullptr) return false;
    for (const auto& f : files)
        if (hasExtension(f, audioExtensions()) || hasExtension(f, midiExtensions())
            || hasExtension(f, pluginExtensions()) || hasExtension(f, { ".json" })
            || hasExtension(f, { chains::kExtension }))
            return true;
    return false;
}

void CanvasComponent::filesDropped(const juce::StringArray& files, int x, int y)
{
    if (doc_ == nullptr) return;
    const auto at = toCanvas({ (float) x, (float) y });
    if (files.size() == 1 && hasExtension(files[0], { ".json" }))
    {
        if (on_open_project_file_) on_open_project_file_(juce::File(files[0]));
        return;
    }
    int k = 0;
    for (const auto& f : files)
    {
        const juce::File file(f);
        if (scope_.isNotEmpty() && !hasExtension(f, pluginExtensions()))
        {
            warn("Cannot add " + file.getFileName(),
                 "Only plugins can be dropped inside a chain. Close the chain to add files "
                 "or chains to the project.");
            continue;
        }
        add_at_ = at + juce::Point<float>(24.0f, 24.0f) * (float) k;
        if      (hasExtension(f, audioExtensions()))  { addInputFromFile(file);     ++k; }
        else if (hasExtension(f, midiExtensions()))   { addMidiInputFromFile(file); ++k; }
        else if (hasExtension(f, pluginExtensions())) { addPluginFromFile(file);    ++k; }
        else if (hasExtension(f, { chains::kExtension }))
        {
            juce::String err;
            if (insertChain(file, *add_at_, err).isEmpty()) warn("Cannot insert chain", err);
            ++k;
        }
        add_at_.reset();
    }
}

// ----- quick add ----- //

void CanvasComponent::showQuickAdd(std::optional<juce::Point<float>> at_opt)
{
    if (doc_ == nullptr) return;
    const auto at = at_opt.value_or(viewCentre());
    auto items = std::vector<QuickAddItem>{};
    auto place = [this, at] { add_at_ = at; };

    const bool inside = scope_.isNotEmpty();
    if (inside)
    {
        const char* names[] = { "Audio inlet", "Audio outlet", "MIDI inlet", "MIDI outlet" };
        for (int k = 0; k < 4; ++k)
            items.push_back({ names[k], "port chain inlet outlet",
                              [this, place, k] { place(); addPort(k >= 2, k % 2 == 0); } });
    }
    else
    {
        items.push_back({ "Input from file...", "input audio wav flac",
                          [this, place] { place(); addInputNode(); } });
        items.push_back({ "Output to file...", "output render wav flac",
                          [this, place] { place(); addOutputNode(); } });
    }
    items.push_back({ "Plugin from file...", "plugin vst3 au lv2",
                      [this, place, at] {
                          place();
                          pending_plugin_at_ = at;
                          addPluginNode();
                      } });
    items.push_back({ "Channel split (stereo -> 2 mono)", "pick_channel",
                      [this, place] { place(); addChannelSplitStereo(); } });
    for (const auto& entry : nodeRegistry())
    {
        if (entry.menu_label.isEmpty() || !entry.menu_add) continue;
        if (inside && chains::forbiddenKind(entry.kind_string)) continue;
        const auto* e = &entry;
        items.push_back({ entry.menu_label.fromFirstOccurrenceOf("Add ", false, false),
                          entry.kind_string,
                          [this, place, e] { place(); addRegistryNode(*e); } });
    }
    for (const auto& name : inside ? juce::StringArray() : chains::names())
        items.push_back({ "Chain: " + name, "chain library saved",
                          [this, at, name] {
                              juce::String err;
                              if (insertChain(chains::fileFor(name), at, err).isEmpty())
                                  warn("Cannot insert chain", err);
                          } });
    if (plugin_list_)
    {
        auto plugins = plugin_list_();
        std::sort(plugins.begin(), plugins.end(), [](const auto& a, const auto& b) {
            return a.name.compareIgnoreCase(b.name) < 0;
        });
        for (const auto& pd : plugins)
            items.push_back({ pd.name,
                              pd.pluginFormatName + " " + pd.manufacturerName
                                  + (pd.isInstrument ? " instrument" : " effect"),
                              [this, place, pd] { place(); addPluginFromDescription(pd); } });
    }

    quick_add_ = std::make_unique<QuickAddPopup>(std::move(items),
        [sp = juce::Component::SafePointer<CanvasComponent>(this)] {
            // Deleting the popup from inside its own callback is unsafe.
            juce::MessageManager::callAsync([sp] { if (sp) sp->quick_add_.reset(); });
        });
    addAndMakeVisible(*quick_add_);
    const auto pos = at.transformedBy(viewTransform()).toInt();
    quick_add_->setBounds(juce::Rectangle<int>(pos.x, pos.y, 320, 280)
                              .constrainedWithin(getLocalBounds()));
    quick_add_->focus();
}

// ----- menus ----- //

void CanvasComponent::showNodeMenu(int node_index, juce::Point<int> screen_pos)
{
    if (nodes_[(size_t) node_index].kind == "chain")
    {
        showChainMenu(nodes_[(size_t) node_index].id, screen_pos);
        return;
    }
    juce::PopupMenu mn;
    enum { kEditProps = 1, kOpenEditor, kDelete, kPlayFile, kStopFile,
           kCopy, kDuplicate, kCut, kSaveChain };
    const auto& nl = nodes_[(size_t) node_index];
    // Input nodes play their source; output nodes their sink.
    juce::File file;
    if (nl.kind == "input")
    {
        for (const auto& in : doc_->inputs)
            if (in.id == nl.id) file = in.source;
    }
    else if (nl.kind == "output")
    {
        for (const auto& on : doc_->outputs)
            if (on.id == nl.id) file = on.sink;
    }
    if (file != juce::File())
    {
        mn.addItem(kPlayFile, "Play File",
                   file.existsAsFile() && on_play_file_ != nullptr);
        mn.addItem(kStopFile, "Stop File", on_stop_file_ != nullptr);
        mn.addSeparator();
    }
    if (nl.kind == "plugin") mn.addItem(kOpenEditor, "Open Editor    Return");
    mn.addItem(kEditProps, "Properties", on_show_properties_ != nullptr);
    mn.addSeparator();
    mn.addItem(kCut,       "Cut          Cmd+X");
    mn.addItem(kCopy,      "Copy         Cmd+C");
    mn.addItem(kDuplicate, "Duplicate    Cmd+D");
    mn.addItem(kDelete,    "Delete       Delete");
    if (scope_.isEmpty())
    {
        mn.addSeparator();
        mn.addItem(kSaveChain, "Save Selection as Chain...    Cmd+G");
    }
    juce::PopupMenu::Options opts;
    opts = opts.withTargetScreenArea({ screen_pos, screen_pos });
    const auto id = nl.id;
    mn.showMenuAsync(opts, [sp = SafePointer<CanvasComponent>(this), id, file](int chosen) {
        if (sp == nullptr) return;
        auto& c = *sp;
        if (chosen == kPlayFile && c.on_play_file_) c.on_play_file_(file);
        else if (chosen == kStopFile && c.on_stop_file_) c.on_stop_file_();
        else if (chosen == kOpenEditor) c.openEditorFor(id);
        else if (chosen == kEditProps && c.on_show_properties_) { c.selectOnly(id); c.on_show_properties_(); }
        else if (chosen == kCut)       c.cutSelection();
        else if (chosen == kCopy)      c.copySelection();
        else if (chosen == kDuplicate) c.duplicateSelection();
        else if (chosen == kDelete)    c.deleteSelection();
        else if (chosen == kSaveChain) c.promptSaveAsChain();
    });
}

void CanvasComponent::showContextMenu(juce::Point<int> screen_pos, juce::Point<float> at)
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
        kQuickAdd,
        kPaste,
        kFit,
        kCloseChain,
        kPortBase     = 500,   // + midi * 2 + outlet
        kRegistryBase = 1000,
        kChainBase    = 2000,
    };

    const bool inside = scope_.isNotEmpty();
    juce::PopupMenu m;
    m.addItem(kQuickAdd, "Add Node...    Cmd+K");
    m.addSeparator();
    if (inside)
    {
        m.addItem(kPortBase + 0, "Add Audio Inlet");
        m.addItem(kPortBase + 1, "Add Audio Outlet");
        m.addItem(kPortBase + 2, "Add MIDI Inlet");
        m.addItem(kPortBase + 3, "Add MIDI Outlet");
        m.addSeparator();
    }
    else
    {
        m.addItem(kAddInput,  "Add Input...");
        m.addItem(kAddOutput, "Add Output...");
    }
    m.addItem(kAddPlugin, "Add Plugin...");
    m.addSeparator();
    m.addItem(kAddSplitStereo,
              "Add Channel Split (stereo -> 2 mono)");
    m.addSeparator();
    for (int i = 0; i < (int) nodeRegistry().size(); ++i)
    {
        const auto& entry = nodeRegistry()[(size_t) i];
        if (entry.menu_label.isNotEmpty() && entry.menu_add
            && !(inside && chains::forbiddenKind(entry.kind_string)))
            m.addItem(kRegistryBase + i, entry.menu_label);
    }
    const auto library = inside ? juce::StringArray() : chains::names();
    juce::PopupMenu insert_chain;
    for (int i = 0; i < library.size(); ++i) insert_chain.addItem(kChainBase + i, library[i]);
    m.addSeparator();
    if (!inside) m.addSubMenu("Insert Chain", insert_chain, !library.isEmpty());
    else         m.addItem(kCloseChain, "Close Chain    Esc");
    m.addSeparator();
    m.addItem(kPaste, "Paste    Cmd+V", canPaste());
    m.addItem(kFit,   "Fit to Window    Cmd+0");

    juce::PopupMenu::Options opts;
    opts = opts.withTargetScreenArea({ screen_pos, screen_pos });
    m.showMenuAsync(opts,
        [sp = SafePointer<CanvasComponent>(this), at, library](int chosen) {
            if (sp == nullptr || chosen == 0) return;
            auto& c = *sp;
            if (chosen == kQuickAdd) { c.showQuickAdd(at); return; }
            if (chosen >= kChainBase && chosen < kChainBase + library.size())
            {
                juce::String err;
                if (c.insertChain(chains::fileFor(library[chosen - kChainBase]), at, err).isEmpty())
                    c.warn("Cannot insert chain", err);
                return;
            }
            if (chosen == kPaste)    { c.paste(at);        return; }
            if (chosen == kFit)      { c.fitToView();      return; }
            if (chosen == kCloseChain) { c.closeChain();   return; }
            c.add_at_ = at;
            if (chosen >= kPortBase && chosen < kPortBase + 4)
            {
                c.addPort((chosen - kPortBase) >= 2, (chosen - kPortBase) % 2 == 0);
                return;
            }
            if (chosen == kAddInput)        { c.addInputNode();          return; }
            if (chosen == kAddOutput)       { c.addOutputNode();         return; }
            if (chosen == kAddPlugin)       { c.pending_plugin_at_ = at; c.addPluginNode(); return; }
            if (chosen == kAddSplitStereo)  { c.addChannelSplitStereo(); return; }
            const int reg_idx = chosen - kRegistryBase;
            if (reg_idx >= 0 && reg_idx < (int) nodeRegistry().size())
                c.addRegistryNode(nodeRegistry()[(size_t) reg_idx]);
            c.add_at_.reset();
        });
}

// ----- properties ----- //

std::unique_ptr<juce::AlertWindow> CanvasComponent::createPropertiesFor(const juce::String& id) const
{
    if (doc_ == nullptr) return nullptr;
    const int i = indexOf(id);
    if (i < 0) return nullptr;
    const auto lookup = mapCanvasIndex(*doc_, i);
    if (lookup.entry == nullptr) return nullptr;
    std::unique_ptr<juce::AlertWindow> aw(createPropertiesDialog(*lookup.entry, lookup.spec_index));
    // A chain's node shows its id inside the chain.
    if (const auto cid = chainOf(id); cid.isNotEmpty())
        aw->getTextEditor("id")->setText(id.fromFirstOccurrenceOf(cid + "/", false, false), false);
    return aw;
}

void CanvasComponent::applyPropertiesFor(const juce::String& id, juce::AlertWindow& aw)
{
    if (doc_ == nullptr) return;
    const int i = indexOf(id);
    if (i < 0) return;
    const auto lookup = mapCanvasIndex(*doc_, i);
    if (lookup.entry == nullptr) return;
    auto* id_field = aw.getTextEditor("id");
    if (id_field->getText().contains("/"))
    {
        warn("Invalid id", "\"" + id_field->getText() + "\" contains '/'. Ids cannot contain '/'.");
        return;
    }
    if (const auto cid = chainOf(id); cid.isNotEmpty())
        id_field->setText(cid + "/" + id_field->getText().trim(), false);
    applyPropertiesDialog(aw, *lookup.entry, lookup.spec_index);
    // A rename keeps the node selected under its new id.
    const auto new_id = aw.getTextEditorContents("id").trim();
    if (new_id != id && indexOf(new_id) >= 0) selection_ = { new_id };
    notifySelection();
}

juce::String CanvasComponent::kindOf(const juce::String& id) const
{
    const int i = indexOf(id);
    return i < 0 ? juce::String() : nodes_[(size_t) i].kind;
}

juce::String CanvasComponent::subtitleOf(const juce::String& id) const
{
    const int i = indexOf(id);
    if (i < 0) return {};
    // label is "id\n(detail)" or "id\nplugin name".
    auto s = nodes_[(size_t) i].label.fromFirstOccurrenceOf("\n", false, false)
                                     .replace("\n", " ");
    if (s.startsWithChar('(') && s.endsWithChar(')'))
        s = s.substring(1, s.length() - 1);
    return s;
}

juce::StringArray CanvasComponent::problemsOf(const juce::String& id) const
{
    const int i = indexOf(id);
    return i < 0 ? juce::StringArray() : nodes_[(size_t) i].problems;
}

void CanvasComponent::openEditorFor(const juce::String& id)
{
    if (doc_ == nullptr || !on_open_plugin_editor_) return;
    for (int k = 0; k < (int) doc_->plugins.size(); ++k)
        if (doc_->plugins[(size_t) k].id == id) { on_open_plugin_editor_(k); return; }
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
    doc.automation.erase(
        std::remove_if(doc.automation.begin(), doc.automation.end(),
                       [&](const project::AutomationLane& a) { return a.node == id; }),
        doc.automation.end());
}

void CanvasComponent::removeEdgeFromDoc(int doc_edge_index)
{
    if (doc_ == nullptr) return;
    if (doc_edge_index < 0
        || doc_edge_index >= (int) doc_->edges.size()) return;
    doc_->edges.erase(doc_->edges.begin() + doc_edge_index);
}

bool CanvasComponent::addEdgeToDoc(int src_node_index, int dst_node_index,
                                   int dst_port)
{
    if (doc_ == nullptr) return false;
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
    const bool dst_is_midi = (dst_entry && dst_entry->is_midi_sink)
                          || dst_port == kMidiPort;
    if (src_is_midi || dst_is_midi)
    {
        dst_port = 0;   // kMidiPort is canvas-only; plugins have one MIDI input
        if (!src_is_midi && src_n.kind != "plugin")
        {
            warn("MIDI edge requires a MIDI source",
                "Connect from a MIDI source (input, clock, processor, "
                "merge, or MIDI-producing plugin) into a MIDI sink "
                "(output, processor, merge, or MIDI-accepting plugin).");
            return false;
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
                warn("Merge full",
                    "All input ports on this midi_merge are already "
                    "connected. Edit the merge to increase num_inputs.");
                return false;
            }
            // Past every rejection: this edge will be added.
            recordUndo();
        }
        else
        {
            // Single-port consumer: replace any existing edge at port 0.
            recordUndo();
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
        return true;
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
        warn("Channel mismatch",
            "Cannot connect "
                + src_n.id + " (" + juce::String(src_ch) + "ch out)"
                + " -> " + dst_n.id
                + " (" + juce::String(dst_ch) + "ch in).\n\n"
                "Channel counts must match. Insert a mix or use a "
                "differently-configured plugin.");
        return false;
    }

    const auto src_id = src_n.id;
    const auto dst_id = dst_n.id;

    // Past channel validation: this edge will be added.
    recordUndo();

    // One audio edge per (dst_node, dst_port): replace any existing. A
    // plugin's MIDI edge also has dst_port 0 and must survive.
    doc_->edges.erase(
        std::remove_if(doc_->edges.begin(), doc_->edges.end(),
            [&](const project::EdgeSpec& e) {
                return e.dst == dst_id && e.dst_port == dst_port
                    && e.kind == project::EdgeKind::Audio;
            }),
        doc_->edges.end());

    project::EdgeSpec e;
    e.src      = src_id;
    e.dst      = dst_id;
    e.dst_port = dst_port;
    doc_->edges.push_back(std::move(e));
    rebuildLayout();
    return true;
}

void CanvasComponent::addRegistryNode(const NodeKindEntry& entry)
{
    if (!entry.menu_add || doc_ == nullptr) return;
    recordUndo();
    entry.menu_add(*doc_);
    rebuildLayout();
    repaint();
}

namespace {
enum ChainMenuId { kChainSave = 1, kChainUpdate, kChainUngroup, kChainOpen,
                   kChainCut, kChainCopy, kChainDuplicate, kChainDelete };
}

bool CanvasComponent::saveAsChainScripted(const juce::var& op, juce::DynamicObject& result)
{
    const auto suggested = saveAsChainSuggestion();
    result.setProperty("shown", suggested.has_value());
    if (!suggested) return true;
    result.setProperty("suggested", *suggested);
    if (!op.hasProperty("name")) return true;
    bool asked = false;
    saveAsChainAnswered(op["name"].toString(), [&](std::function<void()> save) {
        asked = true;
        if ((bool) op["replace"]) save();
    });
    result.setProperty("asked_replace", asked);
    return true;
}

void CanvasComponent::warn(const juce::String& title, const juce::String& message)
{
    if (script_warnings_ != nullptr)
        script_warnings_->add(title + ": " + message);
    else
        juce::AlertWindow::showMessageBoxAsync(
            juce::AlertWindow::WarningIcon, title, message);
}

bool CanvasComponent::applyScriptOp(const juce::var& op,
                                    juce::StringArray& warnings,
                                    juce::String& error,
                                    juce::DynamicObject& result)
{
    if (doc_ == nullptr) { error = "no document"; return false; }
    const auto name = op["op"].toString();
    auto node_index = [this, &error](const juce::var& id) {
        const int i = indexOf(id.toString());
        if (i < 0) error = "no node " + id.toString();
        return i;
    };
    auto point = [&op]() -> std::optional<juce::Point<float>> {
        if (!op.hasProperty("x")) return std::nullopt;
        return juce::Point<float>((float) (double) op["x"], (float) (double) op["y"]);
    };
    // Warnings land in `warnings` instead of opening dialogs.
    script_warnings_ = &warnings;
    struct Reset { juce::StringArray*& p; ~Reset() { p = nullptr; } } reset{ script_warnings_ };

    if (name == "connect")
    {
        const int src = node_index(op["src"]);
        const int dst = node_index(op["dst"]);
        if (src < 0 || dst < 0) return false;
        const auto port = op["port"];
        addEdgeToDoc(src, dst, port.toString() == "midi" ? kMidiPort
                                                         : (int) port);
        return true;
    }
    if (name == "add")
    {
        add_at_ = point();
        if (op["kind"].toString() == "channel_split")
        { addChannelSplitStereo(); return true; }
        if (const auto k = op["kind"].toString(); chains::isPortKind(k))
        {
            if (scope_.isEmpty()) { add_at_.reset(); error = "open a chain to add a port"; return false; }
            addPort(k.startsWith("midi_"), k.endsWith("inlet"));
            return true;
        }
        if (const auto* entry = findKind(op["kind"].toString()))
        { addRegistryNode(*entry); return true; }
        add_at_.reset();
        error = "no addable kind " + op["kind"].toString();
        return false;
    }
    if (name == "delete")
    {
        const int i = node_index(op["id"]);
        if (i < 0) return false;
        selected_edge_ = -1;
        selection_ = { nodes_[(size_t) i].id };
        deleteSelection();
        return true;
    }
    if (name == "properties")
    {
        const auto id = op["id"].toString();
        if (node_index(id) < 0) return false;
        auto aw = createPropertiesFor(id);
        if (aw == nullptr) { error = "no registry entry"; return false; }
        // Fields are set as the user would type or pick them.
        if (auto* fields = op["fields"].getDynamicObject())
            for (const auto& f : fields->getProperties())
            {
                const auto key = f.name.toString();
                if (auto* te = aw->getTextEditor(key))
                    te->setText(f.value.toString(), false);
                else if (auto* cb = aw->getComboBoxComponent(key))
                    cb->setText(f.value.toString(), juce::dontSendNotification);
                else { error = "no field " + key; return false; }
            }
        applyPropertiesFor(id, *aw);
        return true;
    }
    if (name == "undo") { undo(); return true; }
    if (name == "redo") { redo(); return true; }
    if (name == "select")
    {
        juce::StringArray ids;
        if (const auto* arr = op["ids"].getArray())
            for (const auto& id : *arr)
            {
                if (node_index(id) < 0) return false;
                ids.add(id.toString());
            }
        setSelection(ids);
        return true;
    }
    if (name == "select_all")       { selectAll();          return true; }
    if (name == "copy")             { copySelection();      return true; }
    if (name == "cut")              { cutSelection();       return true; }
    if (name == "paste")            { paste(point());       return true; }
    if (name == "duplicate")        { duplicateSelection(); return true; }
    if (name == "delete_selection") { deleteSelection();    return true; }
    if (name == "move")
    {
        // A drag release: the same steps as mouseUp after MovingNodes.
        const int i = node_index(op["id"]);
        if (i < 0) return false;
        const auto to = point();
        if (!to) { error = "move needs x and y"; return false; }
        nodes_[(size_t) i].bounds.setPosition(*to);
        const int target = insertTargetFor(i);
        UndoGroup group(*this);
        recordUndo();
        doc_->layout[nodes_[(size_t) i].id.toStdString()] = project::NodePosition{ to->x, to->y };
        if (target >= 0) insertOnEdge(i, target);
        rebuildLayout();
        return true;
    }
    if (name == "drop")
    {
        juce::StringArray files;
        if (const auto* arr = op["files"].getArray())
            for (const auto& f : *arr) files.add(f.toString());
        if (!isInterestedInFileDrag(files)) { error = "no usable file"; return false; }
        const auto at = point().value_or(juce::Point<float>());
        const auto pos = at.transformedBy(viewTransform());
        filesDropped(files, juce::roundToInt(pos.x), juce::roundToInt(pos.y));
        return true;
    }
    if (name == "quick_add")
    {
        showQuickAdd(point());
        const bool ran = quick_add_ != nullptr && quick_add_->runFirstMatch(op["query"].toString());
        quick_add_.reset();
        if (!ran) error = "nothing matches " + op["query"].toString();
        return ran;
    }
    if (name == "bounds")   { result.setProperty("bounds", nodeBounds()); return true; }
    if (name == "chain_save")
    {
        if (op.hasProperty("chain"))
            return saveChainToLibrary(op["chain"].toString(), op["name"].toString(), error);
        if (!saveSelectionAsChain(op["name"].toString(), error)) return false;
        result.setProperty("id", selection_[0]);
        return true;
    }
    if (name == "chain_insert")
    {
        const auto file = op.hasProperty("file") ? juce::File(op["file"].toString())
                                                 : chains::fileFor(op["name"].toString());
        const auto cid = insertChain(file, point().value_or(viewCentre()), error);
        result.setProperty("id", cid);
        return cid.isNotEmpty();
    }
    if (name == "chain_ungroup") return ungroupChain(op["chain"].toString(), error);
    if (name == "key")
    {
        const auto key = juce::KeyPress::createFromDescription(op["key"].toString());
        if (!key.isValid()) { error = "bad key " + op["key"].toString(); return false; }
        result.setProperty("handled", keyPressed(key));
        return true;
    }
    if (name == "chain_menu")
    {
        // A right-click on the box: lists the menu, then runs "choose".
        const int i = node_index(op["chain"]);
        if (i < 0) return false;
        if (nodes_[(size_t) i].kind != "chain") { error = "not a chain: " + nodes_[(size_t) i].id; return false; }
        const auto cid = nodes_[(size_t) i].id;
        if (!isSelected(i)) selectOnly(cid);
        juce::Array<juce::var> items;
        int chosen = 0;
        bool enabled = false;
        for (const auto& item : chainMenuItems(cid))
        {
            if (item.id == 0) continue;
            auto* o = new juce::DynamicObject();
            o->setProperty("text", item.text);
            o->setProperty("enabled", item.enabled);
            items.add(juce::var(o));
            if (op.hasProperty("choose") && item.text.startsWith(op["choose"].toString()))
            { chosen = item.id; enabled = item.enabled; }
        }
        result.setProperty("items", items);
        if (!op.hasProperty("choose")) return true;
        if (chosen == 0) { error = "no menu item " + op["choose"].toString(); return false; }
        if (!enabled) { error = "menu item disabled: " + op["choose"].toString(); return false; }
        if (chosen == kChainSave) { selectOnly(cid); return saveAsChainScripted(op, result); }
        runChainMenuItem(cid, chosen);
        return true;
    }
    if (name == "save_as_chain") return saveAsChainScripted(op, result);
    if (name == "open_chain")
    {
        const auto cid = op["chain"].toString();
        if (chainMembers(cid).isEmpty()) { error = "no chain " + cid; return false; }
        openChain(cid);
        return true;
    }
    if (name == "close_chain") { closeChain(); return true; }
    if (name == "chain_update")  return updateChainFromLibrary(op["chain"].toString(), error);
    if (name == "chains")
    {
        juce::Array<juce::var> list;
        for (const auto& c : doc_->chains)
        {
            auto* o = new juce::DynamicObject();
            o->setProperty("id", c.id);
            o->setProperty("source", c.source);
            juce::Array<juce::var> members;
            for (const auto& m : chainMembers(c.id)) members.add(m);
            o->setProperty("members", members);
            if (const int b = indexOf(c.id); b >= 0)
            {
                const auto& n = nodes_[(size_t) b];
                o->setProperty("box", juce::Array<juce::var>{ n.bounds.getX(), n.bounds.getY(),
                                                              n.bounds.getWidth(), n.bounds.getHeight() });
                juce::Array<juce::var> ins, outs;
                for (int i : n.ins)  ins.add(nodes_[(size_t) i].id);
                for (int i : n.outs) outs.add(nodes_[(size_t) i].id);
                o->setProperty("inlets", ins);
                o->setProperty("outlets", outs);
            }
            list.add(juce::var(o));
        }
        result.setProperty("chains", list);
        result.setProperty("open", scope_);
        juce::Array<juce::var> library;
        for (const auto& n : chains::names()) library.add(n);
        result.setProperty("library", library);
        return true;
    }
    if (name == "problems")
    {
        auto* o = new juce::DynamicObject();
        for (const auto& n : nodes_)
            if (!n.problems.isEmpty())
            {
                juce::Array<juce::var> lines;
                for (const auto& s : n.problems) lines.add(s);
                o->setProperty(n.id, lines);
            }
        result.setProperty("problems", juce::var(o));
        return true;
    }
    if (name == "meter")
    {
        const auto id = op["id"].toString();
        if (node_index(id) < 0) return false;
        if (const auto* peaks = op["peaks"].getArray())
            for (int c = 0; c < peaks->size(); ++c)
                feedMeter(id, c, (float) (double) (*peaks)[c], (double) op["t"]);
        return true;
    }
    if (name == "meter_state")
    {
        const auto id = op["id"].toString();
        if (node_index(id) < 0) return false;
        const auto& m = meters_[id];
        juce::Array<juce::var> level, hold;
        for (float v : m.level_db) level.add(v);
        for (float v : m.hold_db)  hold.add(v);
        result.setProperty("level_db", level);
        result.setProperty("hold_db", hold);
        result.setProperty("clip", m.clip);
        return true;
    }
    if (name == "tooltip")
    {
        // At canvas point x, y, or at port `id`'s pin on its chain box.
        juce::Point<float> at;
        if (const auto xy = point()) at = *xy;
        else
        {
            const int i = node_index(op["id"]);
            if (i < 0 || nodes_[(size_t) i].box < 0) { error = "no pin " + op["id"].toString(); return false; }
            const auto& n = nodes_[(size_t) i];
            at = boxPinPos(nodes_[(size_t) n.box], n.slot, n.port_dir < 0);
        }
        result.setProperty("tooltip", pinTooltip(at));
        return true;
    }
    if (name == "click")
    {
        // A node's centre, canvas point x, y, or the open chain's bar.
        juce::Point<float> pos;
        if ((bool) op["bar"])
        {
            if (scope_.isEmpty()) { error = "no chain is open"; return false; }
            pos = scopeBar().getCentre();
        }
        else if (const auto xy = point()) pos = xy->transformedBy(viewTransform());
        else
        {
            const int i = node_index(op["id"]);
            if (i < 0) return false;
            pos = nodes_[(size_t) i].bounds.getCentre().transformedBy(viewTransform());
        }
        const juce::ModifierKeys mods(juce::ModifierKeys::leftButtonModifier);
        auto src = juce::Desktop::getInstance().getMainMouseSource();
        const juce::MouseEvent ev(src, pos, mods, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                                  this, this, juce::Time::getCurrentTime(), pos,
                                  juce::Time::getCurrentTime(), 1, false);
        mouseDown(ev);
        mouseUp(ev);
        return true;
    }
    if (name == "fit")
    {
        fitToView();
        result.setProperty("zoom", zoom_);
        return true;
    }
    error = "unknown op " + name;
    return false;
}

void CanvasComponent::setLiveProject(project::LoadedProject* lp)
{
    live_project_ = lp;
    loads_.clear();
    if (lp != nullptr) startTimerHz(30);
    else               stopTimer();
    repaint();
}

void CanvasComponent::timerCallback()
{
    // Meters: the highest peak since the last tick, per channel. Live's
    // meter_states follow the document it compiled, so match by id.
    if (live_project_ != nullptr)
    {
        const double now = juce::Time::getMillisecondCounterHiRes() * 0.001;
        const auto& meters = live_project_->doc.meters;
        for (size_t k = 0; k < meters.size() && k < live_project_->meter_states.size(); ++k)
        {
            auto* st = live_project_->meter_states[k].get();
            for (int c = 0; c < (int) st->held.size(); ++c)
                feedMeter(meters[k].id, c, st->take(c), now);
        }
    }
    // Plugin load: processing time over the real time rendered, sampled
    // every 15 ticks (0.5 s).
    if (live_project_ != nullptr && live_project_->graph != nullptr
        && ++load_ticks_ >= 15)
    {
        load_ticks_ = 0;
        const double sr = (double) live_project_->doc.sample_rate;
        for (const auto& p : live_project_->doc.plugins)
        {
            auto it = live_project_->node_ids.find(p.id.toStdString());
            if (it == live_project_->node_ids.end()) continue;
            const auto [seconds, frames] = live_project_->graph->nodeTime(it->second);
            auto& m = loads_[p.id];
            if (frames > m.frames && m.frames > 0)
                m.load = (float) ((seconds - m.seconds) / ((double) (frames - m.frames) / sr));
            m.seconds = seconds;
            m.frames  = frames;
        }
    }
    repaint();
}

int CanvasComponent::latencyOf(const juce::String& id) const
{
    if (live_project_ != nullptr)
        for (size_t k = 0; k < live_project_->doc.plugins.size(); ++k)
            if (live_project_->doc.plugins[k].id == id && k < live_project_->plugins.size()
                && live_project_->plugins[k] != nullptr)
                return std::max(0, mh_get_latency_samples_rt(live_project_->plugins[k]));
    if (doc_ != nullptr)
        for (const auto& p : doc_->plugins)
            if (p.id == id) return p.probed_latency;
    return -1;
}

float CanvasComponent::loadOf(const juce::String& id) const
{
    if (live_project_ == nullptr) return -1.0f;
    auto it = loads_.find(id);
    return it == loads_.end() ? -1.0f : it->second.load;
}

juce::AlertWindow* CanvasComponent::createPropertiesDialog(
    const NodeKindEntry& entry, int spec_index) const
{
    auto* aw = new juce::AlertWindow(
        "Node properties",
        entry.dialog_title(*doc_, spec_index),
        juce::AlertWindow::QuestionIcon);
    // Every kind shares an editable id field.
    aw->addTextEditor("id", entry.id_at(*doc_, spec_index), "id");
    entry.dialog_emit(*doc_, spec_index, *aw);
    return aw;
}

void CanvasComponent::applyPropertiesDialog(juce::AlertWindow& aw,
                                            const NodeKindEntry& entry,
                                            int spec_index)
{
    if (doc_ == nullptr) return;
    const auto new_id = aw.getTextEditorContents("id").trim();
    if (new_id.isEmpty()) return;
    // A duplicate id merges two nodes on the canvas and makes the saved
    // file fail to load.
    if (new_id != entry.id_at(*doc_, spec_index) && idInUse(*doc_, new_id))
    {
        warn("Id already in use",
             "Another node is already called \"" + new_id
                 + "\". Node ids must be unique.");
        return;
    }
    // A typed relative path resolves against the project folder, as the
    // loader resolves one read from the file. juce::File needs it absolute.
    for (const char* key : { "source", "sink" })
        if (auto* te = aw.getTextEditor(key))
        {
            const auto text = te->getText().trim();
            if (text.isEmpty() || juce::File::isAbsolutePath(text)) continue;
            if (project_dir_ == juce::File())
            {
                warn("Path is relative",
                     "\"" + text + "\" is relative and the project is not saved "
                     "yet. Enter an absolute path, or save the project first.");
                return;
            }
            te->setText(project_dir_.getChildFile(text).getFullPathName(), false);
        }

    NodeKindEntry::RenameFn rename =
        [this](const juce::String& old_id, const juce::String& nid) {
            for (auto& e : doc_->edges)
            {
                if (e.src == old_id) e.src = nid;
                if (e.dst == old_id) e.dst = nid;
            }
            for (auto& a : doc_->automation)
                if (a.node == old_id) a.node = nid;
            auto it = doc_->layout.find(old_id.toStdString());
            if (it != doc_->layout.end())
            {
                doc_->layout[nid.toStdString()] = it->second;
                doc_->layout.erase(it);
            }
            if (on_node_renamed_) on_node_renamed_(old_id, nid);
        };

    recordUndo();
    entry.dialog_apply(*doc_, spec_index, aw, new_id, rename);
    FlagOff no_adopt(adopting_);
    rebuildLayout();
    repaint();
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
        while (idInUse(*doc_, id));
        return id;
    };
    recordUndo();
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
        [this, at = std::exchange(add_at_, std::nullopt)](const juce::FileChooser& fc) {
            const auto file = fc.getResult();
            if (file == juce::File() || doc_ == nullptr) return;
            add_at_ = at;
            addInputFromFile(file);
        });
}

void CanvasComponent::addInputFromFile(const juce::File& file)
{
    const auto at = std::exchange(add_at_, std::nullopt);
    if (doc_ == nullptr) return;
    // Probe the file to take its actual channel count + sr.
    char err[256] = {0};
    MH_AudioFileInfo info{};
    if (!mh_audio_get_file_info(file.getFullPathName().toRawUTF8(),
                                &info, err, sizeof(err)))
    {
        warn("Failed to read audio file", juce::String(err));
        return;
    }
    // Commit the input node with a chosen resample flag.
    auto commit = [this, file, at, ch = (int) info.channels](bool resample) {
        if (doc_ == nullptr) return;
        project::InputNodeSpec n;
        n.id       = generateUniqueId("in");
        n.channels = ch;
        n.source   = file;
        n.resample = resample;
        recordUndo();
        doc_->inputs.push_back(std::move(n));
        add_at_ = at;
        rebuildLayout();
        repaint();
    };

    if ((int) info.sample_rate != doc_->sample_rate && script_warnings_ == nullptr)
    {
        // Offer to resample instead of adding a node that will
        // fail at render (both loaders reject a rate mismatch
        // unless the input opts into resampling).
        juce::AlertWindow::showOkCancelBox(
            juce::AlertWindow::WarningIcon,
            "Sample rate mismatch",
            "File is " + juce::String((int) info.sample_rate)
          + " Hz but the project is "
          + juce::String(doc_->sample_rate)
          + " Hz.\n\nResample the input to the project rate on "
            "render? (Otherwise the node is added as-is and the "
            "render will fail until rates match.)",
            "Resample", "Add as-is",
            /*associatedComponent=*/nullptr,
            juce::ModalCallbackFunction::create(
                [commit](int result) { commit(result == 1); }));
    }
    else
    {
        // Scripted: resample, as the dialog's default button does.
        commit((int) info.sample_rate != doc_->sample_rate);
    }
}

void CanvasComponent::addMidiInputFromFile(const juce::File& file)
{
    if (doc_ == nullptr) return;
    project::MidiInputNodeSpec n;
    n.id     = generateUniqueId("midi");
    n.source = file;
    recordUndo();
    doc_->midi_inputs.push_back(std::move(n));
    rebuildLayout();
    repaint();
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
        [this, at = std::exchange(add_at_, std::nullopt)](const juce::FileChooser& fc) {
            const auto file = fc.getResult();
            if (file == juce::File() || doc_ == nullptr) return;

            add_at_ = at;
            project::OutputNodeSpec n;
            n.id        = generateUniqueId("out");
            n.channels  = 2;     // sensible default; user can edit JSON
            n.bit_depth = 24;
            n.sink      = file;
            recordUndo();
            doc_->outputs.push_back(std::move(n));
            rebuildLayout();
            repaint();
        });
}

juce::String CanvasComponent::generateUniqueId(const juce::String& prefix) const
{
    if (doc_ == nullptr) return prefix + "_1";
    for (int i = 1;; ++i)
    {
        const auto candidate = scoped(prefix + "_" + juce::String(i));
        if (!idInUse(*doc_, candidate)) return candidate;
    }
}


void CanvasComponent::addPluginNode()
{
    // Prefer the application's shared library picker (scanned list +
    // browse + scan). Fall back to a raw file chooser when the canvas is
    // used standalone (no application wired the callback). The position
    // waits in pending_plugin_at_ until the plugin arrives.
    add_at_.reset();
    if (on_add_plugin_requested_)
    {
        on_add_plugin_requested_();
        return;
    }
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
            if (file == juce::File()) return;
            addPluginFromFile(file);
        });
}

void CanvasComponent::addPluginFromFile(const juce::File& file,
                                       std::optional<bool> is_instrument)
{
    if (doc_ == nullptr || file == juce::File()) return;
    if (pending_plugin_at_) add_at_ = std::exchange(pending_plugin_at_, std::nullopt);
    project::PluginNodeSpec p;
    p.path = file;
    probeAndAddPlugin(std::move(p), /*probe_desc_xml=*/{}, is_instrument);
}

void CanvasComponent::addPluginFromDescription(const juce::PluginDescription& pd)
{
    if (doc_ == nullptr) return;
    if (pending_plugin_at_) add_at_ = std::exchange(pending_plugin_at_, std::nullopt);

    // isInstrument is the plugin's own declared category (VST3 kInstrument,
    // AU 'aumu', LV2 InstrumentPlugin) and is the only reliable synth-vs-fx
    // signal -- an instantiated instrument can look exactly like an effect
    // (Surge XT is an instrument whose sole input bus is a stereo audio in).
    // A scanned description carries it; the "Browse to file..." fallback
    // synthesizes a description holding nothing but a path, so its
    // isInstrument is a meaningless default. A non-empty name is what tells
    // the two apart -- only then is the flag worth trusting.
    const std::optional<bool> is_instrument
        = pd.name.isNotEmpty() ? std::optional<bool>(pd.isInstrument)
                               : std::nullopt;

    // Path-based formats (VST3, LV2, or a browsed file) keep loading by
    // path -- their fileOrIdentifier is a real file. Only formats without a
    // usable file path (AudioUnits) take the descriptor route.
    const juce::File f(pd.fileOrIdentifier);
    if (f.exists())
    {
        addPluginFromFile(f, is_instrument);
        return;
    }

    project::PluginNodeSpec p;
    p.display_name = pd.name;
    juce::String pd_xml;
    if (auto xml = pd.createXml())
    {
        pd_xml = xml->toString();
        juce::MemoryOutputStream os;
        juce::Base64::convertToBase64(os, pd_xml.toRawUTF8(),
                                      pd_xml.getNumBytesAsUTF8());
        p.descriptor = os.toString();
    }
    probeAndAddPlugin(std::move(p), pd_xml, is_instrument);
}

// Shared tail for both add paths: probe the plugin (via descriptor when
// `probe_desc_xml` is set, else by path), cache channel/MIDI info for
// connect-time validation, name it, and push it as an undoable edit.
void CanvasComponent::probeAndAddPlugin(project::PluginNodeSpec p,
                                        const juce::String& probe_desc_xml,
                                        std::optional<bool> is_instrument)
{
    // Use the project's sample_rate / block_size so prepareToPlay matches
    // what loadProject will request later.
    char err[256] = {0};
    const bool via_desc = probe_desc_xml.isNotEmpty();
    MH_Plugin* probe = via_desc
        ? mh_open_desc(probe_desc_xml.toRawUTF8(),
                       (double) doc_->sample_rate, doc_->block_size,
                       /*req_in=*/0, /*req_out=*/0, err, sizeof(err))
        : mh_open(p.path.getFullPathName().toRawUTF8(),
                  (double) doc_->sample_rate, doc_->block_size,
                  /*req_in=*/0, /*req_out=*/0, err, sizeof(err));
    // Default to "fx"; "synth" when the plugin is an instrument.
    juce::String id_prefix = "fx";
    if (is_instrument.has_value())
        id_prefix = *is_instrument ? "synth" : "fx";
    if (probe != nullptr)
    {
        MH_Info info{};
        if (mh_get_info(probe, &info))
        {
            p.probed_in_channels  = info.num_input_ch;
            p.probed_out_channels = info.num_output_ch;
            p.accepts_midi        = info.accepts_midi != 0;
            p.produces_midi       = info.produces_midi != 0;
            p.probed_latency      = std::max(0, mh_get_latency_samples(probe));
            // Fallback for callers with no declared category (the raw
            // file-chooser path). An instantiated plugin does not expose its
            // category, so this only guesses: "no audio in, or pure MIDI"
            // catches classic synths but misses instruments that also take
            // audio in, and every MIDI-driven effect would break the
            // converse test on accepts_midi. Measured over 36 installed
            // plugins this errs on 6, all instruments-with-audio-in.
            if (! is_instrument.has_value()
                && (info.num_input_ch == 0 || info.is_midi_effect))
                id_prefix = "synth";
        }
        mh_close(probe);
    }
    else
    {
        juce::AlertWindow::showMessageBoxAsync(
            juce::AlertWindow::WarningIcon,
            "Could not probe plugin",
            juce::String(via_desc ? "mh_open_desc failed: "
                                  : "mh_open failed: ")
                + juce::String(static_cast<const char*>(err))
                + "\n\nAdding the node anyway; channel "
                  "validation at connect time will be disabled "
                  "until the next load.");
    }
    p.id = generateUniqueId(id_prefix);
    recordUndo();
    doc_->plugins.push_back(std::move(p));
    rebuildLayout();
    repaint();
}

} // namespace minihost_desktop

// ----- chains ----- //

namespace minihost_desktop {

juce::String CanvasComponent::chainOf(const juce::String& node_id) const
{
    if (doc_ == nullptr) return {};
    for (const auto& c : doc_->chains)
    {
        const auto rest = node_id.fromFirstOccurrenceOf(c.id + "/", false, false);
        if (node_id.startsWith(c.id + "/") && rest.isNotEmpty() && !rest.contains("/"))
            return c.id;
    }
    return {};
}

juce::StringArray CanvasComponent::chainMembers(const juce::String& cid) const
{
    juce::StringArray out;
    for (const auto& n : nodes_)
        if (chainOf(n.id) == cid) out.add(n.id);
    return out;
}

void CanvasComponent::openChain(const juce::String& cid)
{
    if (doc_ == nullptr || cid == scope_ || chainMembers(cid).isEmpty()) return;
    if (scope_.isEmpty()) { top_zoom_ = zoom_; top_offset_ = offset_; }
    scope_ = cid;
    selected_edge_ = -1;
    selection_.clear();
    known_ids_.clear();  // a scope change adds nothing
    rebuildLayout();
    fitToView();
    notifySelection();
}

void CanvasComponent::closeChain()
{
    if (scope_.isEmpty()) return;
    const auto cid = scope_;
    scope_.clear();
    selected_edge_ = -1;
    selection_.clear();
    known_ids_.clear();
    rebuildLayout();
    zoom_   = top_zoom_;
    offset_ = top_offset_;
    syncProxies();
    selectOnly(cid);
    repaint();
}

void CanvasComponent::adoptIntoScope(const juce::StringArray& fresh)
{
    for (const auto& id : fresh)
    {
        const int i = indexOf(id);
        if (i < 0) continue;
        const auto kind = nodes_[(size_t) i].kind;
        if (chains::forbiddenKind(kind))
        {
            removeNodeFromDoc(i);
            warn("Cannot add " + id, "A " + kind + " node cannot be inside a chain. "
                 "Close the chain to add it to the project.");
            continue;
        }
        const auto local = id.fromLastOccurrenceOf("/", false, false);
        auto to = scoped(local);
        for (int k = 2; idInUse(*doc_, to); ++k) to = scoped(local + "_" + juce::String(k));
        renameNode(id, to);
    }
    rebuildLayout();
}

void CanvasComponent::renameNode(const juce::String& from, const juce::String& to)
{
    const int i = indexOf(from);
    const auto* entry = i >= 0 ? findKind(nodes_[(size_t) i].kind) : nullptr;
    const auto v = i >= 0 ? serializeNode(i) : juce::var();
    if (entry == nullptr || v.isVoid()) return;
    entry->erase_by_id(*doc_, from);
    try { entry->parse(v, to, *doc_, project_dir_); }
    catch (const std::exception& e) { warn("Cannot add " + to, e.what()); return; }
    for (auto& e : doc_->edges)
    {
        if (e.src == from) e.src = to;
        if (e.dst == from) e.dst = to;
    }
    for (auto& a : doc_->automation)
        if (a.node == from) a.node = to;
    if (auto it = doc_->layout.find(from.toStdString()); it != doc_->layout.end())
    {
        doc_->layout[to.toStdString()] = it->second;
        doc_->layout.erase(from.toStdString());
    }
}

void CanvasComponent::addPort(bool midi, bool in)
{
    if (doc_ == nullptr || scope_.isEmpty()) { add_at_.reset(); return; }
    recordUndo();
    if (midi)
    {
        project::MidiPortNodeSpec p;
        p.id = generateUniqueId(in ? "midi_inlet" : "midi_outlet");
        (in ? doc_->midi_inlets : doc_->midi_outlets).push_back(std::move(p));
    }
    else
    {
        project::AudioPortNodeSpec p;
        p.id = generateUniqueId(in ? "inlet" : "outlet");
        (in ? doc_->audio_inlets : doc_->audio_outlets).push_back(std::move(p));
    }
    rebuildLayout();
    repaint();
}

void CanvasComponent::startMove(int node_index, juce::Point<float> p)
{
    drag_kind_     = DragKind::MovingNodes;
    dragging_node_ = node_index;
    drag_start_    = p;
    move_start_.clear();
    for (int i = 0; i < (int) nodes_.size(); ++i)
        if (isSelected(i)) move_start_.push_back({ i, nodes_[(size_t) i].bounds.getPosition() });
    repaint();
}

juce::String CanvasComponent::freeChainId(const juce::String& name) const
{
    juce::String stem;
    for (auto ch : name.toLowerCase())
        stem << (juce::CharacterFunctions::isLetterOrDigit(ch) ? juce::String::charToString(ch) : "_");
    stem = stem.trimCharactersAtStart("_").trimCharactersAtEnd("_");
    if (stem.isEmpty()) stem = "chain";
    auto taken = [this](const juce::String& id) {
        if (idInUse(*doc_, id)) return true;
        for (const auto& c : doc_->chains) if (c.id == id) return true;
        for (const auto& n : nodes_) if (n.id.startsWith(id + "/")) return true;
        return false;
    };
    if (!taken(stem)) return stem;
    for (int i = 2;; ++i)
        if (!taken(stem + "_" + juce::String(i))) return stem + "_" + juce::String(i);
}

juce::var CanvasComponent::serializedAsShown() const
{
    // Auto-placed nodes have no layout entry; record where they are drawn,
    // so ids that change keep their place.
    auto j = juce::JSON::parse(project::serializeProject(*doc_));
    if (!j["layout"].isObject())
        j.getDynamicObject()->setProperty("layout", juce::var(new juce::DynamicObject()));
    auto* layout = j["layout"].getDynamicObject();
    for (const auto& n : nodes_)
        if (!layout->hasProperty(n.id))
        {
            auto* o = new juce::DynamicObject();
            o->setProperty("x", (double) n.bounds.getX());
            o->setProperty("y", (double) n.bounds.getY());
            layout->setProperty(n.id, juce::var(o));
        }
    return j;
}

bool CanvasComponent::editAsJson(const std::function<void(juce::var&)>& fn, juce::String& error)
{
    if (doc_ == nullptr) { error = "no document"; return false; }
    try
    {
        auto j = serializedAsShown();
        fn(j);
        auto next = project::parseProject(j, project_dir_, /*require_output=*/false);
        UndoGroup group(*this);
        recordUndo();
        *doc_ = std::move(next);
    }
    catch (const std::exception& e)
    {
        error = e.what();
        return false;
    }
    selected_edge_ = -1;
    FlagOff no_adopt(adopting_);
    rebuildLayout();
    repaint();
    return true;
}

bool CanvasComponent::saveChainToLibrary(const juce::String& cid, const juce::String& name,
                                         juce::String& error)
{
    if (!chains::validName(name)) { error = "invalid chain name '" + name + "'"; return false; }
    try
    {
        const auto j = serializedAsShown();
        chains::writeChain(chains::fileFor(name), chains::extract(j, cid, name));
    }
    catch (const std::exception& e)
    {
        error = e.what();
        return false;
    }
    for (auto& c : doc_->chains)
        if (c.id == cid && c.source != name)
        {
            recordUndo();
            c.source = name;
        }
    repaint();
    return true;
}

bool CanvasComponent::saveSelectionAsChain(const juce::String& name, juce::String& error)
{
    if (doc_ == nullptr || selection_.isEmpty()) { error = "select the nodes to save"; return false; }
    if (!chains::validName(name)) { error = "invalid chain name '" + name + "'"; return false; }
    if (selection_.size() == 1 && kindOf(selection_[0]) == "chain")
        return saveChainToLibrary(selection_[0], name, error);
    if (scope_.isNotEmpty()) { error = "chains cannot contain chains"; return false; }

    const auto cid = freeChainId(name);
    UndoGroup group(*this);
    auto channels_of = [this](const juce::String& id) {
        const auto lk = mapCanvasIndex(*doc_, indexOf(id));
        return lk.entry ? lk.entry->channels_for(*doc_, lk.spec_index, true) : -1;
    };
    if (!editAsJson([&](juce::var& j) { chains::group(j, selection_, cid, name, channels_of); },
                    error))
        return false;
    if (!saveChainToLibrary(cid, name, error)) return false;
    setSelection({ cid });
    return true;
}

juce::String CanvasComponent::insertChain(const juce::File& file, juce::Point<float> at,
                                          juce::String& error)
{
    if (doc_ == nullptr) { error = "no document"; return {}; }
    if (scope_.isNotEmpty()) { error = "chains cannot contain chains"; return {}; }
    juce::var chain;
    try { chain = chains::readChain(file); }
    catch (const std::exception& e) { error = e.what(); return {}; }
    const auto cid = freeChainId(chain["name"].toString().isNotEmpty()
                                     ? chain["name"].toString()
                                     : file.getFileNameWithoutExtension());
    if (!editAsJson([&](juce::var& j) { chains::insert(j, chain, cid, at); }, error))
        return {};
    setSelection({ cid });
    return cid;
}

bool CanvasComponent::ungroupChain(const juce::String& cid, juce::String& error)
{
    return editAsJson([&](juce::var& j) { chains::ungroup(j, cid); }, error);
}

bool CanvasComponent::updateChainFromLibrary(const juce::String& cid, juce::String& error)
{
    juce::String source;
    for (const auto& c : doc_->chains) if (c.id == cid) source = c.source;
    if (source.isEmpty()) { error = "chain " + cid + " has no library source"; return false; }
    juce::var chain;
    try { chain = chains::readChain(chains::fileFor(source)); }
    catch (const std::exception& e) { error = e.what(); return false; }
    return editAsJson([&](juce::var& j) { chains::replace(j, cid, chain); }, error);
}

std::optional<juce::String> CanvasComponent::saveAsChainSuggestion()
{
    if (doc_ == nullptr || selection_.isEmpty()) return std::nullopt;
    if (scope_.isNotEmpty() && !(selection_.size() == 1 && kindOf(selection_[0]) == "chain"))
    {
        warn("Cannot save chain", "Chains cannot contain chains. Close the chain to group nodes.");
        return std::nullopt;
    }
    juce::String suggested = kindOf(selection_[0]) == "chain" ? selection_[0] : juce::String();
    for (const auto& c : doc_->chains)
        if (c.id == suggested && c.source.isNotEmpty()) suggested = c.source;
    return suggested;
}

void CanvasComponent::saveAsChainAnswered(
    const juce::String& raw, const std::function<void(std::function<void()>)>& ask_replace)
{
    const auto name = raw.trim();
    auto save = [sp = SafePointer<CanvasComponent>(this), name] {
        if (sp == nullptr) return;
        juce::String err;
        if (!sp->saveSelectionAsChain(name, err)) sp->warn("Cannot save chain", err);
    };
    if (chains::validName(name) && chains::fileFor(name).existsAsFile()) ask_replace(save);
    else save();
}

void CanvasComponent::promptSaveAsChain()
{
    const auto suggested = saveAsChainSuggestion();
    if (!suggested) return;
    auto* aw = new juce::AlertWindow("Save as Chain",
        "Save the selected nodes to the chain library.", juce::MessageBoxIconType::NoIcon, this);
    aw->addTextEditor("name", *suggested, "Name:");
    aw->addButton("Save", 1, juce::KeyPress(juce::KeyPress::returnKey));
    aw->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
    aw->enterModalState(true, juce::ModalCallbackFunction::create(
        [sp = SafePointer<CanvasComponent>(this), aw](int r) {
            if (sp == nullptr || r != 1) return;
            const auto name = aw->getTextEditorContents("name").trim();
            sp->saveAsChainAnswered(name,
                [sp, name](std::function<void()> save) {
                    juce::AlertWindow::showOkCancelBox(juce::MessageBoxIconType::QuestionIcon,
                        "Replace chain?", "The library already has a chain named \"" + name
                            + "\". Projects that use it keep their copy.",
                        "Replace", "Cancel", sp.getComponent(),
                        juce::ModalCallbackFunction::create([save](int ok) { if (ok) save(); }));
                });
        }), /*deleteWhenDismissed=*/true);
}

std::vector<CanvasComponent::MenuItem> CanvasComponent::chainMenuItems(const juce::String& cid) const
{
    juce::String source;
    for (const auto& c : doc_->chains) if (c.id == cid) source = c.source;
    const bool in_library = source.isNotEmpty() && chains::fileFor(source).existsAsFile();
    return {
        { kChainOpen, "Open Chain    Return", true },
        { 0, {}, false },
        { kChainSave, "Save to Library...", true },
        { kChainUpdate, "Update from Library"
              + (source.isNotEmpty() ? " (" + source + ")" : juce::String()), in_library },
        { kChainUngroup, "Ungroup", true },
        { 0, {}, false },
        { kChainCut,       "Cut          Cmd+X", true },
        { kChainCopy,      "Copy         Cmd+C", true },
        { kChainDuplicate, "Duplicate    Cmd+D", true },
        { kChainDelete,    "Delete       Delete", true },
    };
}

void CanvasComponent::runChainMenuItem(const juce::String& cid, int chosen)
{
    juce::String err;
    switch (chosen)
    {
        case kChainOpen:      openChain(cid);       return;
        case kChainCut:       cutSelection();       return;
        case kChainCopy:      copySelection();      return;
        case kChainDuplicate: duplicateSelection(); return;
        case kChainDelete:    deleteSelection();    return;
        case kChainSave:      selectOnly(cid); promptSaveAsChain(); return;
        case kChainUpdate:    if (!updateChainFromLibrary(cid, err)) warn("Chain " + cid, err); return;
        case kChainUngroup:   if (!ungroupChain(cid, err)) warn("Chain " + cid, err); return;
        default: return;
    }
}

void CanvasComponent::showChainMenu(const juce::String& cid, juce::Point<int> screen_pos)
{
    juce::PopupMenu m;
    for (const auto& item : chainMenuItems(cid))
        if (item.id == 0) m.addSeparator();
        else m.addItem(item.id, item.text, item.enabled);
    m.showMenuAsync(juce::PopupMenu::Options().withTargetScreenArea({ screen_pos, screen_pos }),
        [sp = SafePointer<CanvasComponent>(this), cid](int chosen) {
            if (sp != nullptr && chosen != 0) sp->runChainMenuItem(cid, chosen);
        });
}

} // namespace minihost_desktop
