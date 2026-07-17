// undo_history.h
//
// Snapshot-based undo/redo for a ProjectDocument.
//
// ProjectDocument is a plain aggregate of copyable specs (it round-trips
// losslessly to JSON), so a whole-document snapshot is both correct and
// cheap for graphs of the size the desktop app edits. This is deliberately
// simpler than a per-command inverse-op scheme: there is exactly one code
// path regardless of which of the ~19 node kinds or edge shapes changed, so
// adding a new node kind needs no undo work.
//
// Usage: call snapshot(doc) immediately BEFORE mutating the document. undo()
// restores the previous state into the caller's document (saving the current
// state for redo); redo() reverses it. Any fresh snapshot() clears the redo
// stack, matching the standard editor contract.
//
// GUI-free and header-only so it can be unit-tested headlessly (see the
// --undo-selftest binary mode and tests/test_desktop_undo.py).

#pragma once

#include "project.h"

#include <cstddef>
#include <vector>

namespace minihost_desktop {

class UndoHistory {
public:
    // Cap the depth so a long editing session can't grow without bound.
    // Documents are small; 100 steps is generous.
    explicit UndoHistory(std::size_t limit = 100) : limit_(limit) {}

    // Capture `current` as a restore point. Call BEFORE the mutation.
    // Clears the redo stack (a new edit invalidates any redo future).
    void snapshot(const project::ProjectDocument& current)
    {
        undo_.push_back(current);
        if (undo_.size() > limit_)
            undo_.erase(undo_.begin());
        redo_.clear();
    }

    bool canUndo() const { return !undo_.empty(); }
    bool canRedo() const { return !redo_.empty(); }

    // Restore the previous state into `current`, pushing the prior value
    // of `current` onto the redo stack. Returns false (and leaves
    // `current` untouched) when there is nothing to undo.
    bool undo(project::ProjectDocument& current)
    {
        if (undo_.empty()) return false;
        redo_.push_back(current);
        current = std::move(undo_.back());
        undo_.pop_back();
        return true;
    }

    // Reverse the most recent undo. Returns false when there is nothing
    // to redo.
    bool redo(project::ProjectDocument& current)
    {
        if (redo_.empty()) return false;
        undo_.push_back(current);
        current = std::move(redo_.back());
        redo_.pop_back();
        return true;
    }

    // Drop all history (e.g. when a different document is loaded).
    void clear()
    {
        undo_.clear();
        redo_.clear();
    }

    std::size_t undoDepth() const { return undo_.size(); }
    std::size_t redoDepth() const { return redo_.size(); }

private:
    std::vector<project::ProjectDocument> undo_;
    std::vector<project::ProjectDocument> redo_;
    std::size_t limit_;
};

} // namespace minihost_desktop
