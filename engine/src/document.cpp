#include "mf/document.hpp"

namespace mf {

Document::Document(json initial) { current_ = std::make_shared<const json>(std::move(initial)); }

std::shared_ptr<const json> Document::snapshot() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return current_;
}

void Document::setCurrent(json next) {
    auto p = std::make_shared<const json>(std::move(next));
    std::lock_guard<std::mutex> lk(mutex_);
    current_ = std::move(p);
}

bool Document::commit(const std::string& label, json next) {
    const json& base = previewBase_ ? *previewBase_ : *current_;
    json fwd = json::diff(base, next);
    if (fwd.empty()) {
        if (previewBase_) { setCurrent(*previewBase_); previewBase_.reset(); }
        return false;
    }
    HistoryEntry e;
    e.label = label;
    e.timestamp = nowSeconds();
    e.forward = fwd;
    e.backward = json::diff(next, base);
    // Branching: discard the invalidated redo future only.
    while ((int)history_.size() > index_) history_.pop_back();
    history_.push_back(std::move(e));
    index_ = (int)history_.size();
    while (history_.size() > maxHistory_) {
        history_.pop_front();
        index_--;
    }
    previewBase_.reset();
    setCurrent(std::move(next));
    ++revision_;
    if (onCommit) onCommit(label, history_.back().forward, revision_);
    return true;
}

bool Document::applyPatch(const std::string& label, const json& patch) {
    json next = (previewBase_ ? *previewBase_ : *current_).patch(patch);
    return commit(label, std::move(next));
}

void Document::preview(json next) {
    if (!previewBase_) previewBase_ = current_;
    setCurrent(std::move(next));
}

bool Document::commitPreview(const std::string& label) {
    if (!previewBase_) return false;
    json next = *current_;
    return commit(label, std::move(next));
}

void Document::cancelPreview() {
    if (!previewBase_) return;
    setCurrent(*previewBase_);
    previewBase_.reset();
}

bool Document::undo() {
    cancelPreview();
    if (!canUndo()) return false;
    const HistoryEntry& e = history_[index_ - 1];
    json next = current_->patch(e.backward);
    index_--;
    setCurrent(std::move(next));
    ++revision_;
    if (onCommit) onCommit("Undo " + e.label, e.backward, revision_);
    return true;
}

bool Document::redo() {
    cancelPreview();
    if (!canRedo()) return false;
    const HistoryEntry& e = history_[index_];
    json next = current_->patch(e.forward);
    index_++;
    setCurrent(std::move(next));
    ++revision_;
    if (onCommit) onCommit("Redo " + e.label, e.forward, revision_);
    return true;
}

bool Document::jumpTo(int target) {
    if (target < 0 || target > (int)history_.size()) return false;
    while (index_ > target) undo();
    while (index_ < target) redo();
    return true;
}

json Document::stateAt(int target) const {
    json s = *current_;
    int i = index_;
    while (i > target && i > 0) { s = s.patch(history_[i - 1].backward); --i; }
    while (i < target && i < (int)history_.size()) { s = s.patch(history_[i].forward); ++i; }
    return s;
}

void Document::reset(json doc) {
    previewBase_.reset();
    history_.clear();
    index_ = 0;
    setCurrent(std::move(doc));
    ++revision_;
    savedRevision_ = revision_;
}

}  // namespace mf
