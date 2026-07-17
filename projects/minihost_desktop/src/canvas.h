// canvas.h
//
// Interactive node-graph view for a ProjectDocument. Paints each node
// as a draggable rounded rectangle with port circles; paints each edge
// as a bezier curve from a source's output port to a destination's
// input port. Auto-layout assigns columns by topological depth and
// rows by sequence within a column.
//
// Edit operations supported:
//   - drag from an output port to an input port to create an edge
//   - click an edge near its midpoint to select it
//   - Delete / Backspace removes the selected node (cascading its
//     edges) or the selected edge
//   - right-click brings up a context menu: add Mix node (preset
//     shapes) or add Plugin node (file chooser)
//
// Edits mutate doc_ directly so File > Save Project persists them.

#pragma once

#include "project.h"
#include "undo_history.h"

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>  // juce::PluginDescription

#include <functional>
#include <unordered_map>
#include <vector>

namespace minihost_desktop {

class CanvasComponent : public juce::Component,
                        private juce::Timer
{
public:
    CanvasComponent();

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

    // Replace the displayed document. Triggers a re-layout: nodes
    // whose ids appear in `doc->layout` use those saved positions;
    // any remaining nodes are auto-positioned by topological depth.
    //
    // Pointer-to-mutable so drags + edits can write back into doc.
    // The caller (MainWindow) keeps the document alive for the
    // canvas's lifetime.
    void setDocument(project::ProjectDocument* doc);

    // After the application updates a plugin's state_b64 (e.g. user
    // captured editor state), call this so the canvas refreshes its
    // label cache. Pure repaint trigger; no layout recompute.
    void notifyDocumentChanged() { repaint(); }

    // Undo / redo of canvas edits (add / delete / connect / move /
    // properties). Snapshot-based: each edit records the pre-edit
    // document; undo/redo swap the whole ProjectDocument in place (the
    // doc_ pointer is stable, so other holders stay valid). Return true
    // if a step was applied.
    bool undo();
    bool redo();
    bool canUndo() const { return undo_history_.canUndo(); }
    bool canRedo() const { return undo_history_.canRedo(); }

    // Fired after any edit, undo, or redo so the application can mark the
    // window dirty and refresh the Edit menu's enabled state.
    using DocumentEditedCb = std::function<void()>;
    void setOnDocumentEdited(DocumentEditedCb cb)
    { on_document_edited_ = std::move(cb); }

    // Add a plugin node for the plugin at `file`, probing its channel
    // counts (same path as the "Add Plugin..." file chooser). Used by
    // the plugin browser to add a selected plugin to the canvas. No-op
    // if no document is loaded. Records an undo step.
    void addPluginFromFile(const juce::File& file);

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
    bool keyPressed(const juce::KeyPress& key) override;

private:
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
    };

    // Per-edge visual data. doc_edge_index is the position of the
    // corresponding entry in doc_->edges so deletes remove the right
    // one even if the user drags create new edges in between.
    struct EdgeLayout {
        int          src_node_index;
        int          dst_node_index;
        int          dst_port;
        int          doc_edge_index;
        project::EdgeKind kind = project::EdgeKind::Audio;
    };

    // What did the user grab on mouseDown? Tracks the in-progress
    // gesture so mouseDrag/mouseUp know how to interpret subsequent
    // events.
    enum class DragKind { None, MovingNode, ConnectingFromPort };

    void rebuildLayout();
    void autoPositionNodes();

    // Hit-tests. Return -1 if no hit.
    int hitTestNode(juce::Point<float> p) const;
    int hitTestEdge(juce::Point<float> p, float tolerance = 6.0f) const;
    // Returns (node_index, port_index) for the input port hit, or
    // (-1, -1) if none. Port radius is the visual port-circle size.
    std::pair<int, int> hitTestInputPort(juce::Point<float> p) const;
    // Returns node_index of an output-port hit, or -1.
    int hitTestOutputPort(juce::Point<float> p) const;

    juce::Point<float> outputPortPos(const NodeLayout& n) const;
    juce::Point<float> inputPortPos (const NodeLayout& n, int port) const;

    // Bezier control points used by both paint and edge hit-testing.
    void edgeCubic(juce::Point<float> p0, juce::Point<float> p1,
                   juce::Point<float>& c0, juce::Point<float>& c1) const;

    void showContextMenu(juce::Point<int> screen_pos);
    // File-chooser flows (registry can't express async UI cleanly):
    void addPluginNode();           // launches an async file chooser
    // Shared tail of the add-plugin paths: probe (via descriptor when
    // probe_desc_xml is set, else by path), cache channel/MIDI info, name,
    // and push as an undoable edit.
    void probeAndAddPlugin(project::PluginNodeSpec p,
                           const juce::String& probe_desc_xml);
    void addInputNode();            // file chooser -> source WAV; channels from file
    void addOutputNode();           // file chooser -> sink path; defaults to 2 channels
    // Convenience helper: stereo split into two pick_channel nodes
    // (registry doesn't model multi-spec menu helpers).
    void addChannelSplitStereo();

    // Property editing entry point. Inspects the kind of node_index
    // and dispatches to the appropriate dialog.
    void showNodePropertiesDialog(int node_index);

    void deleteSelected();
    void removeNodeFromDoc(int node_index);
    void removeEdgeFromDoc(int doc_edge_index);
    void addEdgeToDoc(int src_node_index, int dst_node_index, int dst_port);
    juce::String generateUniqueId(const juce::String& prefix) const;

    // Capture the current document as an undo restore point. Call
    // immediately before any mutation of *doc_. No-op when doc_ is null.
    // Also notifies the application that an edit is happening.
    void recordUndo();

    project::ProjectDocument* doc_ = nullptr;
    project::LoadedProject*   live_project_ = nullptr;
    UndoHistory               undo_history_;
    DocumentEditedCb          on_document_edited_;

    // juce::Timer
    void timerCallback() override { repaint(); }
    OpenPluginEditorCb        on_open_plugin_editor_;
    AddPluginRequestedCb      on_add_plugin_requested_;
    std::vector<NodeLayout>   nodes_;
    std::vector<EdgeLayout>   edges_;

    // Selection / drag state.
    DragKind                  drag_kind_     = DragKind::None;
    int                       dragging_node_ = -1;
    juce::Point<float>        drag_offset_;
    juce::Point<float>        move_start_pos_;   // node pos at drag start
    int                       selected_node_ = -1;
    int                       selected_edge_ = -1;
    int                       connect_from_node_ = -1; // ConnectingFromPort source
    juce::Point<float>        connect_to_pos_;          // current drag target

    std::unique_ptr<juce::FileChooser> chooser_;

    static constexpr float    kColumnWidth   = 220.0f;
    static constexpr float    kRowHeight     = 100.0f;
    static constexpr float    kNodeWidth     = 160.0f;
    static constexpr float    kNodeHeight    = 64.0f;
    static constexpr float    kPortRadius    = 5.0f;
    static constexpr float    kPortHitRadius = 10.0f;  // generous hit area
    static constexpr float    kCanvasPadding = 40.0f;
};

} // namespace minihost_desktop
