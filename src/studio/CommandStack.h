#pragma once
// Undo/redo for every Studio edit. Edits are Command objects that know how to apply and revert themselves.
#include <memory>
#include <string>
#include <vector>

namespace mmdx::studio {

class Command {
public:
    virtual ~Command() = default;
    virtual void Do() = 0;
    virtual void Undo() = 0;
    virtual std::string Name() const = 0;  // UI label ("Move keys")
};

class CommandStack {
public:
    // Executes the command and records it; clears the redo branch.
    void Push(std::unique_ptr<Command> c) {
        c->Do();
        commands_.resize(cursor_);
        commands_.push_back(std::move(c));
        if (commands_.size() > kLimit) { commands_.erase(commands_.begin()); }
        cursor_ = commands_.size();
        ++version_;
    }
    bool CanUndo() const { return cursor_ > 0; }
    bool CanRedo() const { return cursor_ < commands_.size(); }
    void Undo() { if (CanUndo()) { commands_[--cursor_]->Undo(); ++version_; } }
    void Redo() { if (CanRedo()) { commands_[cursor_++]->Do(); ++version_; } }
    std::string UndoName() const { return CanUndo() ? commands_[cursor_ - 1]->Name() : std::string(); }
    std::string RedoName() const { return CanRedo() ? commands_[cursor_]->Name() : std::string(); }
    void Clear() { commands_.clear(); cursor_ = 0; ++version_; }
    // Increments on every push/undo/redo: dirty tracking and "rebuild the evaluated motion" checks.
    uint64_t Version() const { return version_; }

private:
    static constexpr size_t kLimit = 500;
    std::vector<std::unique_ptr<Command>> commands_;
    size_t cursor_ = 0;
    uint64_t version_ = 0;
};

} // namespace mmdx::studio
