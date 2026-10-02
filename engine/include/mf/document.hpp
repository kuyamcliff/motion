// Project document with transactional, undoable edits (JSON-patch history).
#pragma once
#include <deque>
#include <mutex>

#include "common.hpp"

namespace mf {

struct HistoryEntry {
    std::string label;
    double timestamp = 0;  // wall-clock seconds
    json forward;          // RFC 6902 patch old -> new
    json backward;         // patch new -> old
};

struct EditError : std::runtime_error {
    explicit EditError(const std::string& m) : std::runtime_error(m) {}
};

class Document {
   public:
    explicit Document(json initial = json::object());

    // Immutable snapshot usable from render threads.
    std::shared_ptr<const json> snapshot() const;
    const json& doc() const { return *current_; }

    // Commit a new full document state as one undoable transaction.
    // Returns false if nothing changed.
    bool commit(const std::string& label, json next);
    // Apply an RFC 6902 patch as one transaction.
    bool applyPatch(const std::string& label, const json& patch);

    // Interactive previews (e.g. while dragging): visible to renderer, not in history.
    void preview(json next);
    bool commitPreview(const std::string& label);
    void cancelPreview();
    bool inPreview() const { return previewBase_ != nullptr; }

    bool canUndo() const { return index_ > 0; }
    bool canRedo() const { return index_ < (int)history_.size(); }
    bool undo();
    bool redo();
    bool jumpTo(int index);  // 0 = initial state, history.size() = latest
    int historyIndex() const { return index_; }
    const std::deque<HistoryEntry>& history() const { return history_; }
    std::string undoLabel() const { return canUndo() ? history_[index_ - 1].label : std::string(); }
    std::string redoLabel() const { return canRedo() ? history_[index_].label : std::string(); }

    // State the document would have at a given history index (for history preview).
    json stateAt(int index) const;

    uint64_t revision() const { return revision_; }
    bool dirty() const { return revision_ != savedRevision_; }
    void markSaved() { savedRevision_ = revision_; }
    void setMaxHistory(size_t n) { maxHistory_ = n; }

    // Replace document without history (load/recovery).
    void reset(json doc);

    // Called after each committed change with (label, forward patch, revision).
    std::function<void(const std::string&, const json&, uint64_t)> onCommit;

   private:
    void setCurrent(json next);
    std::shared_ptr<const json> current_;
    std::shared_ptr<const json> previewBase_;
    std::deque<HistoryEntry> history_;
    int index_ = 0;
    uint64_t revision_ = 0, savedRevision_ = 0;
    size_t maxHistory_ = 500;
    mutable std::mutex mutex_;
};

}  // namespace mf
