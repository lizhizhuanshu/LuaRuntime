#include "behavior_tree_engine.h"

#include <algorithm>

#include <spdlog/spdlog.h>

#include "composite.h"
#include "script_node.h"
#include "tree_parser.h"

BehaviorTreeEngine::BehaviorTreeEngine() = default;

BehaviorTreeEngine::~BehaviorTreeEngine() {
    Stop();
}

bool BehaviorTreeEngine::Load(const std::string& json) {
    auto tree = TreeParser::Parse(json);
    if (!tree) {
        spdlog::error("BehaviorTreeEngine: failed to parse JSON");
        return false;
    }
    root_ = std::move(tree);
    blackboard_.Clear();
    event_queue_.Drain();
    return true;
}

void BehaviorTreeEngine::Run() {
    if (!root_) {
        spdlog::error("BehaviorTreeEngine: no tree loaded");
        return;
    }
    if (running_.load()) {
        spdlog::warn("BehaviorTreeEngine: already running");
        return;
    }

    running_.store(true);
    paused_.store(false);

    spdlog::info("BehaviorTreeEngine: started");
}

void BehaviorTreeEngine::Pause() {
    if (!running_.load() || paused_.load()) return;
    paused_.store(true);
}

void BehaviorTreeEngine::Resume() {
    if (!paused_.load()) return;
    paused_.store(false);
}

void BehaviorTreeEngine::Stop() {
    if (!running_.load()) return;

    running_.store(false);
    paused_.store(false);
    ResetTree();
    spdlog::info("BehaviorTreeEngine: stopped");
}

void BehaviorTreeEngine::Notify(const std::string& event_name, LuaValue data) {
    event_queue_.Push({event_name, std::move(data)});
}

std::string BehaviorTreeEngine::GetStatus() const {
    if (!running_.load()) return "stopped";
    if (paused_.load()) return "paused";
    return "running";
}

std::string BehaviorTreeEngine::GetCurrentNode() const {
    std::lock_guard<std::mutex> lock(current_node_mutex_);
    return current_node_path_;
}

void BehaviorTreeEngine::InitScriptNodes(lua_State* L, LuaContext* ctx) {
    if (root_) {
        InitScriptNodesRecursive(root_.get(), L, ctx);
    }
}

void BehaviorTreeEngine::InitScriptNodesRecursive(Node* node, lua_State* L, LuaContext* ctx) {
    if (auto* script = dynamic_cast<ScriptNode*>(node)) {
        script->Init(L, ctx);
    }
    if (auto* composite = dynamic_cast<Composite*>(node)) {
        for (auto& child : composite->children()) {
            InitScriptNodesRecursive(child.get(), L, ctx);
        }
    }
}

NodeStatus BehaviorTreeEngine::TickOnce() {
    // Returns kRunning when paused/no-root so the BT event loop doesn't break.
    // Only success/failure cause the event loop to stop and resume the bt.run() coroutine.
    if (!root_ || !running_.load() || paused_.load()) return NodeStatus::kRunning;

    HandleEvents();

    if (!EvaluateDecorators(root_.get())) {
        return NodeStatus::kRunning;
    }

    auto status = root_->Tick(blackboard_, event_queue_);

    if (status != NodeStatus::kRunning) {
        ResetTree();
    }
    return status;
}

bool BehaviorTreeEngine::EvaluateDecorators(Node* node) {
    for (auto& dec : node->decorators()) {
        bool now = dec->Evaluate(blackboard_);
        bool was = false;
        auto it = node->prev_decorator_results_.find(dec.get());
        if (it != node->prev_decorator_results_.end()) {
            was = it->second;
        }

        if (now != was) {
            if (!now) {
                PropagateAbort(node, dec->abort_mode());
            } else {
                auto mode = dec->abort_mode();
                if (mode == AbortMode::kLowerPriority || mode == AbortMode::kBoth) {
                    PropagateAbort(node, AbortMode::kLowerPriority);
                }
            }
            node->prev_decorator_results_[dec.get()] = now;
        }

        if (!now) {
            return false;
        }
    }
    return true;
}

void BehaviorTreeEngine::PropagateAbort(Node* source, AbortMode mode) {
    if (mode == AbortMode::kNone) return;

    std::vector<Node*> running_nodes;
    CollectRunningNodes(root_.get(), running_nodes);

    std::vector<Node*> to_abort;

    if (mode == AbortMode::kSelf || mode == AbortMode::kBoth) {
        for (auto* node : running_nodes) {
            if (IsDescendantOf(node, source) || node == source) {
                to_abort.push_back(node);
            }
        }
    }

    if (mode == AbortMode::kLowerPriority || mode == AbortMode::kBoth) {
        Node* current = source;
        Node* parent = current->parent();
        while (parent) {
            auto* composite = dynamic_cast<Composite*>(parent);
            if (composite) {
                for (size_t i = 0; i < composite->children().size(); ++i) {
                    auto* child = composite->children()[i].get();
                    if (child == current || IsDescendantOf(current, child)) {
                        for (size_t j = i + 1; j < composite->children().size(); ++j) {
                            for (auto* node : running_nodes) {
                                if (IsDescendantOf(node, composite->children()[j].get())) {
                                    to_abort.push_back(node);
                                }
                            }
                        }
                        break;
                    }
                }
            }
            current = parent;
            parent = current->parent();
        }
    }

    std::sort(to_abort.begin(), to_abort.end());
    to_abort.erase(std::unique(to_abort.begin(), to_abort.end()), to_abort.end());

    for (auto* node : to_abort) {
        node->OnAborted();
    }
}

void BehaviorTreeEngine::HandleEvents() {
    event_queue_.Drain();
}

void BehaviorTreeEngine::ResetTree() {
    if (root_) {
        root_->Reset();
    }
}

void BehaviorTreeEngine::CollectRunningNodes(Node* node, std::vector<Node*>& out) {
    auto* composite = dynamic_cast<Composite*>(node);
    if (composite) {
        if (composite->is_mid_sequence()) {
            out.push_back(node);
        }
        for (auto& child : composite->children()) {
            CollectRunningNodes(child.get(), out);
        }
    }
}

bool BehaviorTreeEngine::IsDescendantOf(Node* node, Node* ancestor) const {
    while (node) {
        if (node == ancestor) return true;
        node = node->parent();
    }
    return false;
}
