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
    virtual size_t Bytes() const { return 256; }  // memory held by the command (undo budget); must not change after Push
};

// Several commands as one undo step (done in order, undone in reverse order).
class CompositeCommand : public Command {
public:
    CompositeCommand(std::string name, std::vector<std::unique_ptr<Command>> parts)
        : name_(std::move(name)), parts_(std::move(parts)) {}
    void Do() override { for (auto& c : parts_) c->Do(); }
    void Undo() override { for (auto it = parts_.rbegin(); it != parts_.rend(); ++it) (*it)->Undo(); }
    std::string Name() const override { return name_; }
    size_t Bytes() const override {
        size_t bytes = sizeof(*this) + name_.size();
        for (const auto& c : parts_) bytes += c->Bytes();
        return bytes;
    }

private:
    std::string name_;
    std::vector<std::unique_ptr<Command>> parts_;
};

class CommandStack {
public:
    // Executes the command and records it; clears the redo branch.
    void Push(std::unique_ptr<Command> c) {
        c->Do();
        size_t dropped = 0;
        for (size_t i = cursor_; i < commands_.size(); ++i) dropped += commands_[i]->Bytes();
        bytes_ -= dropped;
        commands_.resize(cursor_);
        bytes_ += c->Bytes();
        commands_.push_back(std::move(c));
        // Trim the oldest snapshots while over the count or byte limit; the newest command always stays.
        size_t trim = 0;
        while (commands_.size() - trim > 1 &&
               (commands_.size() - trim > kLimit || bytes_ > kByteBudget)) {
            bytes_ -= commands_[trim]->Bytes();
            ++trim;
        }
        if (trim > 0) commands_.erase(commands_.begin(), commands_.begin() + (ptrdiff_t)trim);
        cursor_ = commands_.size();
        ++version_;
    }
    bool CanUndo() const { return cursor_ > 0; }
    bool CanRedo() const { return cursor_ < commands_.size(); }
    void Undo() { if (CanUndo()) { commands_[--cursor_]->Undo(); ++version_; } }
    void Redo() { if (CanRedo()) { commands_[cursor_++]->Do(); ++version_; } }
    std::string UndoName() const { return CanUndo() ? commands_[cursor_ - 1]->Name() : std::string(); }
    std::string RedoName() const { return CanRedo() ? commands_[cursor_]->Name() : std::string(); }
    void Clear() { commands_.clear(); cursor_ = 0; bytes_ = 0; ++version_; }
    // Increments on every push/undo/redo: dirty tracking and "rebuild the evaluated motion" checks.
    uint64_t Version() const { return version_; }
    size_t Bytes() const { return bytes_; }
    size_t Count() const { return commands_.size(); }

private:
    static constexpr size_t kLimit = 500;
    static constexpr size_t kByteBudget = 256ull << 20;  // 256 MB of undo snapshots
    std::vector<std::unique_ptr<Command>> commands_;
    size_t bytes_ = 0;
    size_t cursor_ = 0;
    uint64_t version_ = 0;
};

} // namespace mmdx::studio
