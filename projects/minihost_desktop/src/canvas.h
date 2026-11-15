// canvas.h
//
// Interactive node-graph view for a ProjectDocument. Paints each node
// as a rounded rectangle with port circles and each edge as a bezier
// curve from a source's output port to a destination's input port.
// Auto-layout assigns columns by topological depth and rows by sequence
// within a column.
//
// Mouse:
//   - drag from an output port to an input port to connect
//   - click a node or edge to select it; Shift-click adds to the
//     selection; drag on empty space selects with a rectangle
//   - drag a selected node to move the selection; drop a lone node on a
//     cable to insert it there
//   - scroll to pan; Cmd+scroll or pinch to zoom; Alt-drag or
//     middle-drag to pan
//   - double-click a plugin to open its editor, a chain to open it,
//     another node to edit its properties, empty space to add a node
//   - a chain is one box with a pin per inlet and outlet; inside an
//     open chain, its port nodes are the inlets and outlets. The bar at
//     the top, Escape or Cmd+Up closes it
//   - right-click for the node or add-node menu; files dropped on the
//     canvas become nodes
//
// Keys: Delete, Cmd+Z / Cmd+Shift+Z, Cmd+C / X / V / D / A, Cmd+K (add
// node), Cmd+0 (fit), Cmd+= / Cmd+- (zoom), arrows (nudge), Return
// (editor or properties), Escape (clear selection). Tab moves keyboard
// focus from node to node; each node is an accessible element.
//
// Edits mutate doc_ directly so File > Save Project persists them.

#pragma once

#include "project.h"
#include "undo_history.h"

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>  // juce::PluginDescription

#include <functional>
#include <map>
#include <optional>
#include <unordered_map>
#include <vector>

namespace minihost_desktop {

struct NodeKindEntry;
class QuickAddPopup;

class CanvasComponent : public juce::Component,
                        public juce::FileDragAndDropTarget,
                        public juce::TooltipClient,
                        private juce::Timer,
                        private juce::AsyncUpdater
{
public:
    CanvasComponent();
    ~CanvasComponent() override;

    juce::String getTooltip() override;

    // Callback fired when the user double-clicks a plugin node. The
    // argument is the index into doc->plugins (NOT the canvas's
    // internal nodes_ index). The application opens an editor for
    // that PluginNodeSpec.
    using OpenPluginEditorCb = std::function<void(int /*plugin_index*/)>;
    void setOnOpenPluginEditor(OpenPluginEditorCb cb)
    { on_open_plugin_editor_ = std::move(cb); }

    // When set, the "Add Plugin..." context action delegates to this
    // callback (the application shows its shared library picker, which
    // ultimately calls addPluginFromFile). When unset, the canvas falls
    // back to its own raw file chooser so it stays usable standalone.
    using AddPluginRequestedCb = std::function<void()>;
    void setOnAddPluginRequested(AddPluginRequestedCb cb)
    { on_add_plugin_requested_ = std::move(cb); }

    // The scanned plugin library, listed by the quick-add box.
    using PluginListFn = std::function<juce::Array<juce::PluginDescription>()>;
    void setPluginListProvider(PluginListFn fn) { plugin_list_ = std::move(fn); }

    // A project file dropped on the canvas. The app opens it.
    void setOnOpenProjectFile(std::function<void(const juce::File&)> cb)
    { on_open_project_file_ = std::move(cb); }

    // The selection changed, or the selected node was edited.
    void setOnSelectionChanged(std::function<void()> cb)
    { on_selection_changed_ = std::move(cb); }

    // The user asked to edit the selected node's properties (Return,
    // double-click, the context menu). The app focuses its panel.
    void setOnShowProperties(std::function<void()> cb)
    { on_show_properties_ = std::move(cb); }

    // "Play File" / "Stop File" on input and output nodes. The app plays
    // the input's source or the output's sink straight to the device.
    using PlayFileCb = std::function<void(const juce::File&)>;
    void setOnPlayFile(PlayFileCb cb) { on_play_file_ = std::move(cb); }
    void setOnStopFile(std::function<void()> cb) { on_stop_file_ = std::move(cb); }

    // A node's id changed in Properties (old id, new id). The app moves
    // open plugin editors to the new id.
    using RenamedCb = std::function<void(const juce::String&, const juce::String&)>;
    void setOnNodeRenamed(RenamedCb cb) { on_node_renamed_ = std::move(cb); }

    // The saved project's folder, or File() when untitled. A relative path
    // typed in Properties resolves against it.
    void setProjectDirectory(const juce::File& dir) { project_dir_ = dir; }

    // What the engine is doing, drawn as node markers: file inputs and
    // device outputs while the transport plays, output nodes while they
    // record, and the node whose file Play File is playing.
    struct Activity {
        bool       live      = false;
        bool       playing   = false;
        bool       recording = false;
        bool       looping   = false;
        long long  position  = 0;   // transport, in samples
        double     bpm       = 0.0; // live tempo; 0 = use the project's
        juce::File preview;
        bool operator==(const Activity& o) const
        {
            return live == o.live && playing == o.playing
                && recording == o.recording && looping == o.looping
                && position == o.position && bpm == o.bpm
                && preview == o.preview;
        }
    };
    void setActivity(const Activity& a)
    {
        if (a == activity_) return;
        if (a.playing && !activity_.playing)
            for (auto& [id, m] : meters_) m.clip = false;
        activity_ = a;
        repaint();
    }

    // Replace the displayed document. Triggers a re-layout: nodes
    // whose ids appear in `doc->layout` use those saved positions;
    // any remaining nodes are auto-positioned by topological depth.
    // The view is fitted to the graph when it does not fit at 100%.
    //
    // Pointer-to-mutable so drags + edits can write back into doc.
    // The caller (MainWindow) keeps the document alive for the
    // canvas's lifetime.
    void setDocument(project::ProjectDocument* doc);

    // After the application updates a plugin's state_b64 (e.g. user
    // captured editor state), call this so the canvas refreshes its
    // label cache. Pure repaint trigger; no layout recompute.
    void notifyDocumentChanged() { repaint(); }

    // Re-derives node labels and ports from the document (e.g. after
    // probe data was filled in). Keeps selection and undo history.
    void refreshLayout() { rebuildLayout(); repaint(); }

    // Union of node bounds plus padding, from the canvas origin. Empty
    // without nodes.
    juce::Rectangle<int> contentBounds() const;
    // {id: [x, y, w, h]} for every node, for `--canvas-script` tests.
    juce::var nodeBounds() const;

    // ----- view ----- //
    // Zoom so every node is visible (at most 100%) and centre the graph.
    void fitToView();
    // Multiply the zoom by `factor`, keeping `anchor` (component
    // coordinates) fixed. Zoom stays within [kMinZoom, kMaxZoom].
    void zoomBy(float factor, juce::Point<float> anchor);
    void zoomBy(float factor) { zoomBy(factor, getLocalBounds().getCentre().toFloat()); }
    void resetZoom();
    float zoom() const { return zoom_; }

    // ----- selection and clipboard ----- //
    juce::StringArray selectedIds() const { return selection_; }
    void selectOnly(const juce::String& id);
    void selectAll();
    bool hasSelection() const { return !selection_.isEmpty(); }
    void copySelection();
    void cutSelection();
    bool canPaste() const;
    // Pastes at `at` (canvas coordinates), or offset from the copied
    // nodes when unset. The pasted nodes become the selection.
    void paste(std::optional<juce::Point<float>> at = std::nullopt);
    void duplicateSelection();
    void deleteSelection();
    // Opens the type-to-search add-node box at `at` (canvas
    // coordinates), or at the view's centre.
    void showQuickAdd(std::optional<juce::Point<float>> at = std::nullopt);

    // ----- properties (the app's side panel) ----- //
    // Builds the node's property fields: an AlertWindow that is never
    // shown, holding one TextEditor or ComboBox per field. Null for an
    // unknown id.
    std::unique_ptr<juce::AlertWindow> createPropertiesFor(const juce::String& id) const;
    // Writes the fields back as one undo step. A warning (duplicate id,
    // relative path in an untitled project) leaves the node unchanged.
    void applyPropertiesFor(const juce::String& id, juce::AlertWindow& aw);
    // "plugin", "input", ...; empty for an unknown id.
    juce::String kindOf(const juce::String& id) const;
    // The node's detail line, e.g. "mix 2 x 2ch" or the plugin's name.
    juce::String subtitleOf(const juce::String& id) const;
    // Problems the graph compiler would reject, one line each.
    juce::StringArray problemsOf(const juce::String& id) const;
    // A plugin's latency in samples (the running instance's while Live
    // runs, else the probed value), or -1 when unknown.
    int latencyOf(const juce::String& id) const;
    // A plugin's share of the real-time budget over the last half second
    // of Live, 0..1+, or -1 when Live is not running.
    float loadOf(const juce::String& id) const;
    int sampleRate() const { return doc_ != nullptr ? doc_->sample_rate : 0; }
    // Automation lanes on the node in the project file.
    int automationLanesOf(const juce::String& id) const
    {
        if (doc_ == nullptr) return 0;
        return (int) std::count_if(doc_->automation.begin(), doc_->automation.end(),
                                   [&](const auto& a) { return a.node == id; });
    }
    void openEditorFor(const juce::String& id);

    // ----- chains (docs/dev/chains.md) ----- //
    // Show chain `cid`'s inside, or the project again. Edits made while a
    // chain is open add nodes to it.
    void openChain(const juce::String& cid);
    void closeChain();
    juce::String openChainId() const { return scope_; }
    // Save the selection to the library as `name`. A selection that is
    // exactly one chain's nodes saves that chain; any other is grouped
    // into a new chain first.
    bool saveSelectionAsChain(const juce::String& name, juce::String& error);
    bool saveChainToLibrary(const juce::String& cid, const juce::String& name,
                            juce::String& error);
    // Insert the chain in `file` at canvas point `at`; returns its id.
    juce::String insertChain(const juce::File& file, juce::Point<float> at,
                             juce::String& error);
    bool ungroupChain(const juce::String& cid, juce::String& error);
    // Replace the chain's nodes with the library version it came from.
    bool updateChainFromLibrary(const juce::String& cid, juce::String& error);
    // Asks for a name, then saveSelectionAsChain.
    void promptSaveAsChain();
    // The chain a node belongs to, or empty.
    juce::String chainOf(const juce::String& node_id) const;

    // Undo / redo of canvas edits (add / delete / connect / move /
    // properties). Snapshot-based: each edit records the pre-edit
    // document; undo/redo swap the whole ProjectDocument in place (the
    // doc_ pointer is stable, so other holders stay valid). Return true
    // if a step was applied.
    bool undo();
    bool redo();
    bool canUndo() const { return undo_history_.canUndo(); }
    bool canRedo() const { return undo_history_.canRedo(); }

    // For document edits made outside the canvas (e.g. Set BPM): records
    // an undo step and notifies like a canvas edit. Call before mutating.
    void recordExternalEdit() { recordUndo(); }

    // Applies one scripted edit through the code the mouse, context menu
    // and Properties panel use. Used by `minihost_desktop --canvas-script`
    // to test canvas edits without a window. Ops (JSON objects):
    //   {"op":"connect","src":id,"dst":id,"port":n|"midi"}
    //   {"op":"add","kind":k[,"x":x,"y":y]}  (registry kinds, or "channel_split")
    //   {"op":"delete","id":id}
    //   {"op":"properties","id":id,"fields":{name:value,...}}
    //   {"op":"undo"} / {"op":"redo"}
    //   {"op":"select","ids":[...]} / {"op":"select_all"}
    //   {"op":"copy"} / {"op":"cut"} / {"op":"paste"[,"x","y"]}
    //   {"op":"duplicate"} / {"op":"delete_selection"}
    //   {"op":"move","id":id,"x":x,"y":y}   (a drag release: may insert on a cable)
    //   {"op":"drop","files":[path,...],"x":x,"y":y}
    //   {"op":"quick_add","query":text[,"x","y"]}  (runs the first match)
    //   {"op":"bounds"}    result "bounds": {id: [x, y, w, h]}
    //   {"op":"problems"}  result "problems": {id: [line, ...]}
    //   {"op":"fit"}       result "zoom"
    //   {"op":"meter","id":id,"peaks":[gain,...],"t":seconds}  feeds a meter
    //   {"op":"meter_state","id":id}  result "level_db", "hold_db", "clip"
    //   {"op":"click","id":id}  a click on the node (clears a clip light)
    //   {"op":"add","kind":"audio_inlet"|"audio_outlet"|"midi_inlet"|"midi_outlet"}
    //   {"op":"open_chain","chain":id} / {"op":"close_chain"}
    //   {"op":"chains"}  result "chains" [{id, source, members, box, inlets,
    //                    outlets}], "library", "open"
    // Ops that report data set properties on `result`. Warnings that
    // would open a dialog are appended to `warnings`. Returns false and
    // sets `error` for a malformed op or unknown node.
    bool applyScriptOp(const juce::var& op, juce::StringArray& warnings,
                       juce::String& error, juce::DynamicObject& result);

    // Fired after any edit, undo, or redo so the application can mark the
    // window dirty and refresh the Edit menu's enabled state.
    using DocumentEditedCb = std::function<void()>;
    void setOnDocumentEdited(DocumentEditedCb cb)
    { on_document_edited_ = std::move(cb); }

    // Add a plugin node for the plugin at `file`, probing its channel
    // counts (same path as the "Add Plugin..." file chooser). Used by
    // the plugin browser to add a selected plugin to the canvas. No-op
    // if no document is loaded. Records an undo step.
    //
    // `is_instrument` is the plugin's declared category, when the caller
    // knows it (a scanned juce::PluginDescription does). It only picks the
    // generated node id's prefix -- "synth_N" vs "fx_N". Pass nullopt when
    // unknown; the probe-based fallback then guesses. See probeAndAddPlugin.
    void addPluginFromFile(const juce::File& file,
                           std::optional<bool> is_instrument = std::nullopt);

    // Add a plugin node from a scanned juce::PluginDescription. Real files
    // (VST3/LV2) delegate to addPluginFromFile; formats without a usable
    // file path (AudioUnits) store the serialized descriptor and load via
    // mh_open_desc. Records an undo step.
    void addPluginFromDescription(const juce::PluginDescription& pd);

    // Wire the canvas to the live LoadedProject so meter nodes show
    // real-time per-channel peak. Pass nullptr when live stops.
    // Starts/stops an internal repaint timer accordingly.
    void setLiveProject(project::LoadedProject* lp);

    void paint(juce::Graphics& g) override;
    void resized() override;

    void mouseDown(const juce::MouseEvent& e) override;
    void mouseDrag(const juce::MouseEvent& e) override;
    void mouseUp  (const juce::MouseEvent& e) override;
    void mouseDoubleClick(const juce::MouseEvent& e) override;
    void mouseWheelMove(const juce::MouseEvent& e,
                        const juce::MouseWheelDetails& w) override;
    void mouseMagnify(const juce::MouseEvent& e, float scale) override;
    bool keyPressed(const juce::KeyPress& key) override;

    // juce::FileDragAndDropTarget
    bool isInterestedInFileDrag(const juce::StringArray& files) override;
    void filesDropped(const juce::StringArray& files, int x, int y) override;

    static constexpr float kMinZoom = 0.25f;
    static constexpr float kMaxZoom = 2.0f;

private:
    friend class NodeProxy;

    // Per-node visual state. Index matches the order nodes appear in
    // the document (inputs first, then plugins, mixes, outputs --
    // the order the parser stores them).
    struct NodeLayout {
        juce::String        id;
        juce::String        kind;
        juce::String        label;
        juce::Rectangle<float> bounds;
        int                 num_input_ports  = 0;
        int                 num_output_ports = 0;  // 0 for output kind, 1 otherwise
        bool                midi_in          = false;  // separate MIDI input port
        // Input ports the compiler requires but no edge feeds.
        std::vector<int>    missing_inputs;
        juce::StringArray   problems;
        bool                shown = true;   // drawn in the open scope
        int                 port_dir = 0;   // port nodes: 1 inlet, -1 outlet
        // A port node drawn as a pin of its chain's box: the box's index
        // and the pin's slot on its side.
        int                 box = -1;
        int                 slot = 0;
        std::vector<int>    ins, outs;      // a chain box's pins: port nodes
    };

    // Port index of a node's MIDI input port (see NodeLayout::midi_in).
    static constexpr int kMidiPort = -2;

    // Per-edge visual data. doc_edge_index is the position of the
    // corresponding entry in doc_->edges so deletes remove the right
    // one even if the user drags create new edges in between.
    struct EdgeLayout {
        int          src_node_index;
        int          dst_node_index;
        int          dst_port;
        int          doc_edge_index;
        project::EdgeKind kind = project::EdgeKind::Audio;
        juce::String problem;  // e.g. "2ch > 1ch"; empty when valid
    };

    // What did the user grab on mouseDown? Tracks the in-progress
    // gesture so mouseDrag/mouseUp know how to interpret subsequent
    // events.
    enum class DragKind { None, MovingNodes, ConnectingFromPort, Lasso, Panning };

    void rebuildLayout();
    void autoPositionNodes();
    // Flags unconnected required inputs and channel mismatches.
    void findProblems();

    // View transform: component = canvas * zoom_ + offset_.
    juce::AffineTransform viewTransform() const
    { return juce::AffineTransform::scale(zoom_).translated(offset_); }
    juce::Point<float> toCanvas(juce::Point<float> p) const
    { return (p - offset_) / zoom_; }
    juce::Point<float> viewCentre() const
    { return toCanvas(getLocalBounds().getCentre().toFloat()); }

    // Hit-tests in canvas coordinates. Return -1 if no hit.
    int hitTestNode(juce::Point<float> p) const;
    int hitTestEdge(juce::Point<float> p, float tolerance = 6.0f) const;
    // Returns (node_index, port_index) for the input port hit, or
    // (-1, -1) if none. Port radius is the visual port-circle size.
    std::pair<int, int> hitTestInputPort(juce::Point<float> p) const;
    // Returns node_index of an output-port hit, or -1.
    int hitTestOutputPort(juce::Point<float> p) const;
    // A chain box pin's port: "<local id> (<kind>)". Empty off a pin.
    juce::String pinTooltip(juce::Point<float> p) const;
    // The cable a lone dragged node would be inserted on, or -1.
    int insertTargetFor(int node_index) const;
    // Splices node_index into edges_[edge_index] as one undo step.
    // Returns false (document unchanged) when the channels do not fit.
    bool insertOnEdge(int node_index, int edge_index);

    juce::Point<float> outputPortPos(const NodeLayout& n) const;
    // Where node i's output is drawn: its port, or its pin on a chain box.
    juce::Point<float> outPos(int i) const;
    juce::Point<float> boxPinPos(const NodeLayout& box, int slot, bool out) const;
    // An open chain's inlets draw no input, its outlets no output.
    bool drawsInput(const NodeLayout& n) const  { return !(scope_.isNotEmpty() && n.port_dir > 0); }
    bool drawsOutput(const NodeLayout& n) const
    { return n.num_output_ports > 0 && !(scope_.isNotEmpty() && n.port_dir < 0); }
    juce::Point<float> inputPortPos (const NodeLayout& n, int port) const;
    // Where an edge ends: the MIDI port for MIDI edges into a node that
    // has one, else the audio port.
    juce::Point<float> edgeEndPos(const EdgeLayout& e) const;
    juce::Path edgePath(const EdgeLayout& e) const;

    // Bezier control points used by both paint and edge hit-testing.
    void edgeCubic(juce::Point<float> p0, juce::Point<float> p1,
                   juce::Point<float>& c0, juce::Point<float>& c1) const;

    void showNodeMenu(int node_index, juce::Point<int> screen_pos);
    void showContextMenu(juce::Point<int> screen_pos, juce::Point<float> at);

    // Serialize the document, apply `fn`, parse it back. One undo step.
    bool editAsJson(const std::function<void(juce::var&)>& fn, juce::String& error);
    juce::var serializedAsShown() const;
    juce::String freeChainId(const juce::String& name) const;
    juce::StringArray chainMembers(const juce::String& cid) const;
    void showChainMenu(const juce::String& cid, juce::Point<int> screen_pos);
    void startMove(int node_index, juce::Point<float> p);
    // `local` in the open chain ("vox/local"), or `local` at the top.
    juce::String scoped(const juce::String& local) const
    { return scope_.isEmpty() ? local : scope_ + "/" + local; }
    // Moves nodes added while a chain is open into it; drops I/O kinds.
    void adoptIntoScope(const juce::StringArray& fresh);
    void renameNode(const juce::String& from, const juce::String& to);
    // The open chain's title bar, in component coordinates.
    juce::Rectangle<float> scopeBar() const { return getLocalBounds().toFloat().removeFromTop(24.0f); }
    juce::String scope_;                 // open chain id; empty at the top
    float              top_zoom_ = 1.0f; // the top view, restored on close
    juce::Point<float> top_offset_;
    bool               adopting_ = true; // false while undo or JSON edits rebuild
    // File-chooser flows (registry can't express async UI cleanly):
    void addPluginNode();           // launches an async file chooser
    // Shared tail of the add-plugin paths: probe (via descriptor when
    // probe_desc_xml is set, else by path), cache channel/MIDI info, name,
    // and push as an undoable edit.
    void probeAndAddPlugin(project::PluginNodeSpec p,
                           const juce::String& probe_desc_xml,
                           std::optional<bool> is_instrument = std::nullopt);
    void addInputNode();            // file chooser -> addInputFromFile
    // Probes the file's channels and rate; offers to resample on a
    // rate mismatch.
    void addInputFromFile(const juce::File& file);
    void addMidiInputFromFile(const juce::File& file);
    void addOutputNode();           // file chooser -> sink path; defaults to 2 channels
    // Convenience helper: stereo split into two pick_channel nodes
    // (registry doesn't model multi-spec menu helpers).
    void addChannelSplitStereo();

    void addRegistryNode(const NodeKindEntry& entry);
    // An inlet or outlet of the open chain.
    void addPort(bool midi, bool in);
    juce::AlertWindow* createPropertiesDialog(const NodeKindEntry& entry,
                                              int spec_index) const;
    void applyPropertiesDialog(juce::AlertWindow& aw,
                               const NodeKindEntry& entry, int spec_index);
    // A warning dialog, or an entry in script_warnings_ while scripted.
    void warn(const juce::String& title, const juce::String& message);
    juce::StringArray* script_warnings_ = nullptr;
    void removeNodeFromDoc(int node_index);
    void removeEdgeFromDoc(int doc_edge_index);
    // Returns false when the edge was refused (a warning explains why).
    bool addEdgeToDoc(int src_node_index, int dst_node_index, int dst_port);
    juce::String generateUniqueId(const juce::String& prefix) const;
    int indexOf(const juce::String& id) const;
    // Kind-level serialization of one node, as in a project file.
    juce::var serializeNode(int node_index) const;

    // Capture the current document as an undo restore point. Call
    // immediately before any mutation of *doc_. No-op when doc_ is null.
    // Also notifies the application of the edit, asynchronously: the
    // callers mutate after this returns, and the listener (the live
    // sequencer push) must read the edited document.
    // Inside an UndoGroup only the first call records.
    void recordUndo();
    struct UndoGroup {
        explicit UndoGroup(CanvasComponent& c) : c_(c)
        { if (c_.undo_group_depth_++ == 0) c_.undo_group_recorded_ = false; }
        ~UndoGroup() { --c_.undo_group_depth_; }
        CanvasComponent& c_;
    };
    int  undo_group_depth_    = 0;
    bool undo_group_recorded_ = false;

    // Where the next added node(s) go, in canvas coordinates. Set by the
    // context menu, quick-add and file drops; rebuildLayout places nodes
    // it has not seen before there and clears it.
    std::optional<juce::Point<float>> add_at_;
    std::optional<juce::Point<float>> pending_plugin_at_;  // library picker in flight
    std::vector<juce::String>         known_ids_;

    void setSelection(juce::StringArray ids);
    bool isSelected(int node_index) const
    { return selection_.contains(nodes_[(size_t) node_index].id); }
    void nudgeSelection(juce::Point<float> delta);
    void notifySelection() { if (on_selection_changed_) on_selection_changed_(); }

    // One invisible, focusable child per node, so screen readers and
    // the Tab key reach individual nodes.
    void syncProxies();
    std::map<juce::String, std::unique_ptr<juce::Component>> proxies_;
    juce::String accessibleDescription(int node_index) const;

    project::ProjectDocument* doc_ = nullptr;
    project::LoadedProject*   live_project_ = nullptr;
    UndoHistory               undo_history_;
    DocumentEditedCb          on_document_edited_;

    // juce::Timer: repaints meters, updates plugin loads.
    void timerCallback() override;
    struct LoadMeter { double seconds = 0.0; long long frames = 0; float load = -1.0f; };
    std::map<juce::String, LoadMeter> loads_;
    int load_ticks_ = 0;

    // juce::AsyncUpdater
    void handleAsyncUpdate() override
    { if (on_document_edited_) on_document_edited_(); }
    OpenPluginEditorCb        on_open_plugin_editor_;
    AddPluginRequestedCb      on_add_plugin_requested_;
    PluginListFn              plugin_list_;
    std::function<void(const juce::File&)> on_open_project_file_;
    std::function<void()>     on_selection_changed_;
    std::function<void()>     on_show_properties_;
    PlayFileCb                on_play_file_;
    Activity                  activity_;

    // Activity marker for node n: outline colour and corner tag. Returns
    // false when the node is idle.
    bool nodeActivity(const NodeLayout& n, juce::Colour& colour,
                      juce::String& tag) const;
    // Sequencer node: one cell per step of the current pass, lit where a
    // note plays, the playing step outlined.
    void paintSequencerSteps(juce::Graphics& g, const NodeLayout& n) const;
    // Meter node: one horizontal bar per channel on a -60..0 dBFS scale,
    // with a peak-hold tick and a clip light.
    void paintMeter(juce::Graphics& g, const NodeLayout& n) const;
    // Meter ballistics. Levels fall 24 dB/s; a hold tick stays 1.5 s;
    // the clip light (a peak at or above 0 dBFS) stays until the meter is
    // clicked or the transport starts.
    struct MeterView {
        std::vector<float>  level_db, hold_db;
        std::vector<double> hold_at, last_at;
        bool                clip = false;
    };
    std::map<juce::String, MeterView> meters_;
    void feedMeter(const juce::String& id, int channel, float peak, double now);
    static constexpr float kMeterFloorDb = -60.0f;
    // The strip under a node's subtitle used by meters and sequencers.
    juce::Rectangle<float> nodeStrip(const NodeLayout& n) const
    { return n.bounds.reduced(10.0f, 5.0f).removeFromBottom(n.kind == "meter" ? 8.0f : 6.0f); }
    std::function<void()>     on_stop_file_;
    RenamedCb                 on_node_renamed_;
    juce::File                project_dir_;
    std::vector<NodeLayout>   nodes_;
    std::vector<EdgeLayout>   edges_;

    // Selection / drag state.
    DragKind                  drag_kind_     = DragKind::None;
    juce::StringArray         selection_;        // node ids
    int                       selected_edge_ = -1;
    int                       dragging_node_ = -1;  // node under the cursor at press
    juce::Point<float>        drag_start_;          // canvas coords at press
    std::vector<std::pair<int, juce::Point<float>>> move_start_;  // node, position
    int                       insert_target_ = -1;  // edge under a lone dragged node
    int                       connect_from_node_ = -1; // ConnectingFromPort source
    juce::Point<float>        connect_to_pos_;          // current drag target
    juce::Rectangle<float>    lasso_;
    juce::StringArray         lasso_base_;          // selection before a Shift-lasso
    juce::Point<float>        pan_start_offset_;

    float                     zoom_   = 1.0f;
    juce::Point<float>        offset_;
    bool                      fit_pending_ = false;

    std::unique_ptr<QuickAddPopup>     quick_add_;
    std::unique_ptr<juce::FileChooser> chooser_;

    // Copied nodes as project-file JSON, shared by every canvas in the
    // process so a copy can be pasted into another project.
    struct Clipboard {
        juce::Array<juce::var>   nodes;     // serialized nodes
        juce::Array<juce::var>   edges;     // {src, dst, dst_port, kind} among them
        juce::Array<juce::var>   positions; // [x, y] per node
        int                      pastes = 0;
    };
    static Clipboard&         clipboard();

    // Nodes with many input ports grow by kPortPitch per port.
    static constexpr float    kColumnWidth   = 200.0f;
    static constexpr float    kRowHeight     = 72.0f;
    static constexpr float    kNodeWidth     = 150.0f;
    static constexpr float    kNodeHeight    = 48.0f;
    static constexpr float    kHeaderHeight  = 18.0f;
    static constexpr float    kPortPitch     = 12.0f;
    static constexpr float    kPortRadius    = 4.5f;
    static constexpr float    kPortHitRadius = 6.0f;   // half kPortPitch
    static constexpr float    kCanvasPadding = 40.0f;
    static constexpr float    kGrid          = 20.0f;
};

} // namespace minihost_desktop
