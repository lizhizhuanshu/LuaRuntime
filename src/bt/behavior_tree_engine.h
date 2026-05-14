#pragma once

#include <atomic>
#include <memory>
#include <string>

extern "C" {
#include "lua.h"
}

#include "blackboard.h"
#include "bt_event_queue.h"
#include "node.h"
#include "types.h"

class BehaviorTreeEngine : public std::enable_shared_from_this<BehaviorTreeEngine> {
public:
    using Ptr = std::shared_ptr<BehaviorTreeEngine>;

    BehaviorTreeEngine();
    ~BehaviorTreeEngine();

    // Non-copyable
    BehaviorTreeEngine(const BehaviorTreeEngine&) = delete;
    BehaviorTreeEngine& operator=(const BehaviorTreeEngine&) = delete;

    // Load tree from JSON string
    bool Load(const std::string& json);

    // Lifecycle (thread-safe)
    void Run();
    void Pause();
    void Resume();
    void Stop();

    // State queries (thread-safe)
    bool IsRunning() const { return running_.load(); }
    bool IsPaused() const { return paused_.load(); }
    std::string GetStatus() const;
    std::string GetCurrentNode() const;

    // Blackboard access (thread-safe)
    Blackboard& blackboard() { return blackboard_; }

    // Event injection (thread-safe)
    void Notify(const std::string& event_name, LuaValue data);

    // Initialize script nodes with lua_State and LuaContext (called on event loop thread)
    void InitScriptNodes(lua_State* L, LuaContext* ctx);

    // Tick once (called by BT event loop thread)
    // Returns the tree status after tick (kRunning if tree is still executing)
    NodeStatus TickOnce();

private:
    bool EvaluateDecorators(Node* node);
    void PropagateAbort(Node* source, AbortMode mode);
    void HandleEvents();
    void ResetTree();
    void InitScriptNodesRecursive(Node* node, lua_State* L, LuaContext* ctx);
    void CollectRunningNodes(Node* node, std::vector<Node*>& out);
    bool IsDescendantOf(Node* node, Node* ancestor) const;

    std::unique_ptr<Node> root_;
    Blackboard blackboard_;
    BtEventQueue event_queue_;

    std::atomic<bool> running_{false};
    std::atomic<bool> paused_{false};

    mutable std::mutex current_node_mutex_;
    std::string current_node_path_;
};
