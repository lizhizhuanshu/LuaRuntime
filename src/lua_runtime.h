#pragma once

extern "C" {
#include "lua.h"
}

#include <sol/sol.hpp>

#include <async_simple/Executor.h>
#include <async_simple/Promise.h>
#include <async_simple/coro/FutureAwaiter.h>
#include <async_simple/coro/Lazy.h>

#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <string>
#include <atomic>
#include <thread>
#include <unordered_map>
#include <variant>
#include <vector>

#include "code_provider.h"
#include "lua_extension.h"

using AsyncHandle = int64_t;
using LuaValue = std::variant<std::nullptr_t, bool, int64_t, double, std::string>;

struct ScriptResult {
    int status = LUA_ERRRUN;
    std::vector<LuaValue> values;
    std::string error;
};

class LuaRuntimeFactory;

class LuaRuntime : public std::enable_shared_from_this<LuaRuntime> {
public:
    using Ptr = std::shared_ptr<LuaRuntime>;

    ~LuaRuntime();

    async_simple::coro::Lazy<ScriptResult> RunScript(const std::string& script);
    async_simple::coro::Lazy<ScriptResult> RunFile(const std::string& filename);

    static Ptr FromLuaState(lua_State* L);

    AsyncHandle PreYield(lua_State* L);
    int Yield(lua_State* L);

    void Resume(AsyncHandle handle);
    void Resume(AsyncHandle handle, std::vector<LuaValue> args);

    void CallLuaFunction(int fn_ref, std::vector<LuaValue> args = {});

    sol::state& lua() { return *lua_; }

    CodeProvider* code_provider() const { return code_provider_.get(); }
    async_simple::Executor* executor() const { return executor_; }

    std::optional<lua_CFunction> find_c_module(const std::string& name) const {
        auto it = c_modules_.find(name);
        return it != c_modules_.end() ? std::optional(it->second) : std::nullopt;
    }

private:
    friend class LuaRuntimeFactory;

    LuaRuntime();

    static void Setup(sol::state& lua, const std::shared_ptr<CodeProvider>& code_provider,
                    const std::unordered_map<std::string, lua_CFunction>& c_modules,
                    async_simple::Executor* executor,
                    const std::vector<std::shared_ptr<LuaExtension>>& extensions);

    void Start();
    void Stop();
    void EventLoop();
    void WaitOrTimeout();

    void CancelTimer(AsyncHandle handle);

    // Lua thread pool
    lua_State* AcquireCo();
    void ReleaseCo(lua_State* co);

    struct PendingEntry {
        lua_State* co;
    };

    struct ResumeRequest {
        AsyncHandle handle;
        std::vector<LuaValue> args;
    };

    struct ResumeResult {
        lua_State* co = nullptr;
        int status = 0;
    };

    enum class TimerType { kSleep, kSetTimeout };

    struct TimerEntry {
        TimerType type;
        lua_State* co = nullptr;
        int fn_ref = LUA_NOREF;
        AsyncHandle handle = 0;
    };

    struct ScriptRequest {
        std::string chunk;
        std::string name;
        async_simple::Promise<ScriptResult> promise;
    };

    ResumeResult DoResume(AsyncHandle handle, std::vector<LuaValue> args);
    void ProcessExpiredTimers();
    bool DrainOneResume();
    bool DrainOneCallback();
    bool DrainOneScript();
    void MaybeRecycleCo(lua_State* co, int status, int nresults);
    std::vector<LuaValue> PeekValues(lua_State* L, int nresults);
    void PushValues(lua_State* L, const std::vector<LuaValue>& values);

    std::unique_ptr<sol::state> lua_;
    std::shared_ptr<CodeProvider> code_provider_;
    std::unordered_map<std::string, lua_CFunction> c_modules_;
    async_simple::Executor* executor_ = nullptr;
    std::vector<std::shared_ptr<LuaExtension>> extensions_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::queue<ResumeRequest> resume_queue_;
    std::unordered_map<AsyncHandle, PendingEntry> pending_;
    std::multimap<int64_t, TimerEntry> timer_queue_;
    AsyncHandle next_handle_ = 1;
    std::queue<std::pair<int, std::vector<LuaValue>>> callback_queue_;

    // Active threads and their registry refs
    std::unordered_map<lua_State*, int> active_co_refs_;
    // Thread pool: idle threads with their registry refs
    std::vector<std::pair<lua_State*, int>> co_pool_;

    // Event loop thread
    std::thread event_loop_thread_;
    std::atomic<bool> running_{false};
    std::queue<ScriptRequest> script_queue_;
    std::unordered_map<lua_State*, async_simple::Promise<ScriptResult>> script_promises_;
};
