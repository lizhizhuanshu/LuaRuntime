#pragma once

#include "lua_context.h"

#include <sol/sol.hpp>

#include <async_simple/coro/FutureAwaiter.h>
#include <async_simple/coro/Lazy.h>

#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

class LuaRuntimeFactory;

class LuaRuntime : public std::enable_shared_from_this<LuaRuntime> {
public:
    using Ptr = std::shared_ptr<LuaRuntime>;

    ~LuaRuntime();

    async_simple::coro::Lazy<ScriptResult> RunScript(const std::string& script);
    async_simple::coro::Lazy<ScriptResult> RunFile(const std::string& filename);
    async_simple::coro::Lazy<ScriptResult> CallFunction(int fn_ref, std::vector<LuaValue> args = {});
    sol::state& lua() { return *lua_; }
    LuaContext& context() { return context_; }

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

    // Lua state ownership (destroyed after context_ so Shutdown can use main_L_)
    std::unique_ptr<sol::state> lua_;

    // Execution context (owns code_provider, extensions, config)
    LuaContext context_;

    // Event loop
    std::thread event_loop_thread_;
    std::atomic<bool> running_{false};
};
