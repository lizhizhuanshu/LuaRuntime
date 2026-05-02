#include "lua_runtime.h"
#include "lua_extension.h"
#include <chrono>
#include <cstring>
#include <memory>

#include <spdlog/spdlog.h>

namespace {
void SetExtraspace(lua_State* L, LuaRuntime* rt) {
    std::memcpy(lua_getextraspace(L), &rt, sizeof(rt));
}

LuaRuntime* GetExtraspace(lua_State* L) {
    LuaRuntime* rt = nullptr;
    std::memcpy(&rt, lua_getextraspace(L), sizeof(rt));
    return rt;
}

int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// Cache module result: non-nil values cached as-is, nil replaced with true (matches native Lua)
void CacheModuleResult(lua_State* L, const char* name, int result_idx) {
    if (result_idx < 0) result_idx = lua_absindex(L, result_idx);
    lua_getfield(L, LUA_REGISTRYINDEX, LUA_LOADED_TABLE);
    if (lua_isnil(L, result_idx)) {
        lua_pushboolean(L, 1);
    } else {
        lua_pushvalue(L, result_idx);
    }
    lua_setfield(L, -2, name);
    lua_pop(L, 1);
}

// --- custom_require continuation (called after yield resumes) ---

// Stack when called: [name] [source_string_or_nil]
int require_continuation(lua_State* L, int status, lua_KContext ctx) {
    const char* name = lua_tostring(L, 1);

    if (lua_isnil(L, 2)) {
        return luaL_error(L, "module '%s' not found via CodeProvider", name);
    }

    size_t len;
    const char* source = lua_tolstring(L, 2, &len);
    std::string chunkname = "@" + std::string(name);

    // Replace source with compiled chunk: [name] [chunk]
    lua_pop(L, 1);  // remove source_string
    int load_status = luaL_loadbuffer(L, source, len, chunkname.c_str());
    if (load_status != LUA_OK) {
        return lua_error(L);
    }

    // Call chunk(name), expect 1 return: [name] [module]
    lua_pushvalue(L, 1);  // push name as arg
    int call_status = lua_pcall(L, 1, 1, 0);
    if (call_status == LUA_YIELD) {
        return luaL_error(L, "module '%s' attempted to yield during loading", name);
    }
    if (call_status != LUA_OK) {
        return lua_error(L);  // error message already on stack
    }

    CacheModuleResult(L, name, 2);
    lua_remove(L, 1);  // remove name, leaving [result]
    return 1;
}

// --- custom_loadfile continuation ---

// Stack when called: [filename] [source_string_or_nil]
int loadfile_continuation(lua_State* L, int status, lua_KContext ctx) {
    const char* filename = lua_tostring(L, 1);

    if (lua_isnil(L, 2)) {
        lua_pop(L, 1);  // remove nil
        lua_pushnil(L);
        lua_pushfstring(L, "cannot find file '%s' via CodeProvider", filename);
        lua_remove(L, 1);  // remove filename
        return 2;
    }

    size_t len;
    const char* source = lua_tolstring(L, 2, &len);
    std::string chunkname = "@" + std::string(filename);

    lua_pop(L, 1);  // remove source_string
    int load_status = luaL_loadbuffer(L, source, len, chunkname.c_str());
    if (load_status != LUA_OK) {
        lua_pushnil(L);
        lua_insert(L, -2);
        lua_remove(L, 1);  // remove filename
        return 2;
    }
    lua_remove(L, 1);  // remove filename, leaving [chunk]
    return 1;
}

// --- Custom require ---

int custom_require(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);

    // 1. Check package.loaded cache
    lua_getfield(L, LUA_REGISTRYINDEX, LUA_LOADED_TABLE);
    lua_getfield(L, -1, name);
    if (lua_toboolean(L, -1)) {
        lua_remove(L, -2);
        return 1;
    }
    lua_pop(L, 2);

    auto* rt = GetExtraspace(L);

    // 2. Check C modules (synchronous)
    auto openf = rt->find_c_module(name);
    if (openf.has_value()) {
        lua_pushcfunction(L, *openf);
        lua_pushstring(L, name);
        int call_status = lua_pcall(L, 1, 1, 0);
        if (call_status == LUA_YIELD) {
            return luaL_error(L, "C module '%s' attempted to yield during loading", name);
        }
        if (call_status != LUA_OK) {
            return lua_error(L);  // error message already on stack
        }
        CacheModuleResult(L, name, -1);
        return 1;
    }

    // 3. CodeProvider (async via asio + yieldk)
    if (rt->code_provider()) {
        if (!rt->io_context()) {
            return luaL_error(L, "module '%s': io_context required for CodeProvider", name);
        }

        auto rt_shared = rt->shared_from_this();
        auto handle = rt->PreYield(L);
        auto* exec = rt->io_context();
        std::string module_name(name);  // safe capture for coroutine

        asio::co_spawn(*exec,
            [rt_shared, handle, module_name = std::move(module_name)]() mutable -> asio::awaitable<void> {
                auto source = co_await rt_shared->code_provider()->LoadModule(module_name);
                if (source.has_value()) {
                    rt_shared->Resume(handle, {std::move(*source)});
                } else {
                    rt_shared->Resume(handle, {LuaValue{nullptr}});
                }
            },
            asio::detached);

        return lua_yieldk(L, 0, 0, require_continuation);
    }

    // 4. Not found
    return luaL_error(L, "module '%s' not found", name);
}

// --- Custom loadfile ---

int custom_loadfile(lua_State* L) {
    const char* filename = luaL_checkstring(L, 1);
    if (filename[0] == '\0') {
        lua_pushnil(L);
        lua_pushliteral(L, "empty filename");
        return 2;
    }

    if (filename[0] == '/') {
        int status = luaL_loadfile(L, filename);
        if (status != LUA_OK) {
            lua_pushnil(L);
            lua_insert(L, -2);
            return 2;
        }
        return 1;
    }

    auto* rt = GetExtraspace(L);
    if (!rt || !rt->code_provider()) {
        lua_pushnil(L);
        lua_pushfstring(L, "cannot load relative file '%s': no CodeProvider", filename);
        return 2;
    }
    if (!rt->io_context()) {
        lua_pushnil(L);
        lua_pushfstring(L, "cannot load relative file '%s': no io_context", filename);
        return 2;
    }

    auto rt_shared = rt->shared_from_this();
    auto handle = rt->PreYield(L);
    auto* exec = rt->io_context();
    std::string file_path(filename);  // safe capture for coroutine

    asio::co_spawn(*exec,
        [rt_shared, handle, file_path = std::move(file_path)]() mutable -> asio::awaitable<void> {
            auto source = co_await rt_shared->code_provider()->LoadFile(file_path);
            if (source.has_value()) {
                rt_shared->Resume(handle, {std::move(*source)});
            } else {
                rt_shared->Resume(handle, {LuaValue{nullptr}});
            }
        },
        asio::detached);

    return lua_yieldk(L, 0, 0, loadfile_continuation);
}
}  // namespace

LuaRuntime::LuaRuntime() : lua_(std::make_unique<sol::state>()) {
    SetExtraspace(lua_->lua_state(), this);
    lua_->open_libraries(sol::lib::base, sol::lib::string, sol::lib::table,
                         sol::lib::math, sol::lib::package);
}

void LuaRuntime::Setup(sol::state& lua, const std::shared_ptr<CodeProvider>& code_provider,
                       const std::unordered_map<std::string, lua_CFunction>& c_modules,
                       asio::io_context* io_context,
                       const std::vector<std::shared_ptr<LuaExtension>>& extensions) {
    // Built-in functions
    lua.set_function("now", []() { return NowMs(); });
    lua.set_function("sleep", [](lua_State* L) -> int {
        int ms = static_cast<int>(luaL_checkinteger(L, 1));
        auto rt = LuaRuntime::FromLuaState(L);
        {
            std::lock_guard<std::mutex> lock(rt->mutex_);
            rt->timer_queue_.emplace(
                NowMs() + ms,
                LuaRuntime::TimerEntry{LuaRuntime::TimerType::kSleep, L, LUA_NOREF});
        }
        return rt->Yield(L);
    });
    lua.set_function("setTimeout", [](lua_State* L) -> int {
        int ms = static_cast<int>(luaL_checkinteger(L, 1));
        luaL_checktype(L, 2, LUA_TFUNCTION);
        auto rt = LuaRuntime::FromLuaState(L);

        lua_State* main_L = rt->lua().lua_state();
        lua_pushvalue(L, 2);
        lua_xmove(L, main_L, 1);
        int fn_ref = luaL_ref(main_L, LUA_REGISTRYINDEX);

        AsyncHandle handle;
        {
            std::lock_guard<std::mutex> lock(rt->mutex_);
            handle = rt->next_handle_++;
            rt->timer_queue_.emplace(
                NowMs() + ms,
                LuaRuntime::TimerEntry{LuaRuntime::TimerType::kSetTimeout, nullptr, fn_ref, handle});
        }
        lua_pushinteger(L, static_cast<lua_Integer>(handle));
        return 1;
    });
    lua.set_function("clearTimeout", [](lua_State* L) -> int {
        auto rt = LuaRuntime::FromLuaState(L);
        auto handle = static_cast<AsyncHandle>(luaL_checkinteger(L, 1));
        rt->CancelTimer(handle);
        return 0;
    });
    // Config
    auto* rt = GetExtraspace(lua.lua_state());
    rt->code_provider_ = code_provider;
    rt->io_context_ = io_context;
    rt->c_modules_ = c_modules;
    rt->extensions_ = extensions;

    // Extensions
    lua_State* main_L = lua.lua_state();
    for (size_t i = 0; i < rt->extensions_.size(); ++i) {
        try {
            rt->extensions_[i]->OnInit(main_L);
        } catch (...) {
            // Rollback: shutdown already-initialized extensions
            for (size_t j = 0; j < i; ++j) {
                try { rt->extensions_[j]->OnShutdown(main_L); } catch (...) {}
            }
            throw;
        }
    }

    // Override require/loadfile
    lua.clear_package_loaders();
    if (rt->code_provider_) {
        lua.set_function("require", custom_require);
        lua.set_function("loadfile", custom_loadfile);
    } else if (!rt->c_modules_.empty()) {
        lua.set_function("require", custom_require);
    }
}

LuaRuntime::~LuaRuntime() {
    lua_State* main_L = lua_->lua_state();
    for (auto& ext : extensions_) {
        ext->OnShutdown(main_L);
    }
    for (auto& [handle, entry] : pending_) {
        luaL_unref(main_L, LUA_REGISTRYINDEX, entry.registry_ref);
    }
    for (auto& [deadline, entry] : timer_queue_) {
        if (entry.fn_ref != LUA_NOREF) {
            luaL_unref(main_L, LUA_REGISTRYINDEX, entry.fn_ref);
        }
    }
    for (auto& [co, ref] : active_callback_co_map_) {
        luaL_unref(main_L, LUA_REGISTRYINDEX, ref);
    }
    while (!callback_queue_.empty()) {
        luaL_unref(main_L, LUA_REGISTRYINDEX, callback_queue_.front().first);
        callback_queue_.pop();
    }
}

LuaRuntime::Ptr LuaRuntime::FromLuaState(lua_State* L) {
    auto* rt = GetExtraspace(L);
    if (!rt) {
        spdlog::error("LuaRuntime::FromLuaState: no runtime found");
        throw std::runtime_error("LuaRuntime::FromLuaState: no runtime found");
    }
    return rt->shared_from_this();
}

void LuaRuntime::CancelTimer(AsyncHandle handle) {
    lua_State* main_L = lua_->lua_state();
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = timer_queue_.begin(); it != timer_queue_.end(); ++it) {
        if (it->second.handle == handle) {
            if (it->second.fn_ref != LUA_NOREF) {
                luaL_unref(main_L, LUA_REGISTRYINDEX, it->second.fn_ref);
            }
            timer_queue_.erase(it);
            return;
        }
    }
}

void LuaRuntime::MaybeRecycleCallbackCo(lua_State* co, int status) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = active_callback_co_map_.find(co);
    if (it == active_callback_co_map_.end()) return;
    int co_ref = it->second;
    active_callback_co_map_.erase(it);
    if (status != LUA_YIELD) {
        lua_State* main_L = lua_->lua_state();
        luaL_unref(main_L, LUA_REGISTRYINDEX, co_ref);
    }
}

void LuaRuntime::CallLuaFunction(int fn_ref, std::vector<LuaValue> args) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        callback_queue_.push({fn_ref, std::move(args)});
    }
    cv_.notify_one();
}

AsyncHandle LuaRuntime::PreYield(lua_State* co) {
    lua_State* main_L = lua_->lua_state();
    lua_pushthread(co);
    lua_xmove(co, main_L, 1);
    int ref = luaL_ref(main_L, LUA_REGISTRYINDEX);

    std::lock_guard<std::mutex> lock(mutex_);
    auto handle = next_handle_++;
    pending_[handle] = {co, ref};
    return handle;
}

int LuaRuntime::Yield(lua_State* L) {
    return lua_yield(L, 0);
}

void LuaRuntime::Resume(AsyncHandle handle) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        resume_queue_.push({handle, {}});
    }
    cv_.notify_one();
}

void LuaRuntime::Resume(AsyncHandle handle, std::vector<LuaValue> args) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        resume_queue_.push({handle, std::move(args)});
    }
    cv_.notify_one();
}

int LuaRuntime::RunScript(const std::string& script) {
    return RunInCoroutine(script, "=script");
}

int LuaRuntime::RunFile(const std::string& filename) {
    return RunInCoroutine("", filename);
}

void LuaRuntime::ProcessExpiredTimers(lua_State* main_co, int& main_status) {
    std::vector<TimerEntry> expired;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        while (!timer_queue_.empty() && timer_queue_.begin()->first <= NowMs()) {
            expired.push_back(std::move(timer_queue_.begin()->second));
            timer_queue_.erase(timer_queue_.begin());
        }
    }

    lua_State* main_L = lua_->lua_state();
    for (auto& entry : expired) {
        if (entry.type == TimerType::kSleep) {
            int nresults = 0;
            int status = lua_resume(entry.co, main_L, 0, &nresults);
            if (entry.co == main_co) {
                main_status = status;
            }
            MaybeRecycleCallbackCo(entry.co, status);
            if (status != LUA_OK && status != LUA_YIELD) {
                spdlog::error("LuaRuntime: {}",
                              lua_tostring(entry.co, -1) ? lua_tostring(entry.co, -1)
                                                          : "unknown error");
                lua_pop(entry.co, 1);
            }
        } else {
            lua_rawgeti(main_L, LUA_REGISTRYINDEX, entry.fn_ref);
            luaL_unref(main_L, LUA_REGISTRYINDEX, entry.fn_ref);
            if (lua_pcall(main_L, 0, 0, 0) != LUA_OK) {
                spdlog::error("setTimeout: {}",
                              lua_tostring(main_L, -1) ? lua_tostring(main_L, -1)
                                                       : "unknown error");
                lua_pop(main_L, 1);
            }
        }
    }
}

int LuaRuntime::RunInCoroutine(const std::string& chunk, const std::string& name) {
    lua_State* main_L = lua_->lua_state();
    lua_newthread(main_L);
    int thread_ref = luaL_ref(main_L, LUA_REGISTRYINDEX);

    lua_rawgeti(main_L, LUA_REGISTRYINDEX, thread_ref);
    lua_State* co = lua_tothread(main_L, -1);
    lua_pop(main_L, 1);
    SetExtraspace(co, this);

    int load_result;
    if (chunk.empty()) {
        load_result = luaL_loadfile(co, name.c_str());
    } else {
        load_result = luaL_loadbuffer(co, chunk.c_str(), chunk.size(), name.c_str());
    }
    if (load_result != LUA_OK) {
        spdlog::error("LuaRuntime: {}", lua_tostring(co, -1));
        lua_pop(co, 1);
        luaL_unref(main_L, LUA_REGISTRYINDEX, thread_ref);
        return load_result;
    }

    int nresults = 0;
    int main_status = lua_resume(co, main_L, 0, &nresults);

    while (true) {
        // 1. Process expired timers
        ProcessExpiredTimers(co, main_status);

        // 2. Drain external resume queue
        while (true) {
            ResumeRequest req;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (resume_queue_.empty()) break;
                req = std::move(resume_queue_.front());
                resume_queue_.pop();
            }
            auto result = DoResume(req.handle, std::move(req.args));
            if (result.co == co) {
                main_status = result.status;
            }
        }

        // 3. Drain callback queue
        while (true) {
            std::pair<int, std::vector<LuaValue>> cb;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (callback_queue_.empty()) break;
                cb = std::move(callback_queue_.front());
                callback_queue_.pop();
            }
            lua_newthread(main_L);
            int co_ref = luaL_ref(main_L, LUA_REGISTRYINDEX);

            lua_rawgeti(main_L, LUA_REGISTRYINDEX, co_ref);
            lua_State* cb_co = lua_tothread(main_L, -1);
            lua_pop(main_L, 1);
            SetExtraspace(cb_co, this);

            lua_rawgeti(cb_co, LUA_REGISTRYINDEX, cb.first);
            luaL_unref(main_L, LUA_REGISTRYINDEX, cb.first);
            PushValues(cb_co, cb.second);

            {
                std::lock_guard<std::mutex> lock(mutex_);
                active_callback_co_map_[cb_co] = co_ref;
            }

            int nresults = 0;
            int status = lua_resume(cb_co, main_L, static_cast<int>(cb.second.size()), &nresults);

            MaybeRecycleCallbackCo(cb_co, status);
            if (status != LUA_OK && status != LUA_YIELD) {
                spdlog::error("LuaRuntime callback: {}",
                              lua_tostring(cb_co, -1) ? lua_tostring(cb_co, -1) : "unknown error");
                lua_pop(cb_co, 1);
            }
        }

        // 4. Check exit: all done when main finished and nothing pending
        bool has_work;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            has_work = !pending_.empty() || !timer_queue_.empty()
                       || !callback_queue_.empty() || !active_callback_co_map_.empty();
        }
        if (main_status != LUA_YIELD && !has_work) break;

        // 5. Wait: use nearest timer as timeout, or block indefinitely
        {
            std::unique_lock<std::mutex> lock(mutex_);
            if (!timer_queue_.empty()) {
                int64_t deadline = timer_queue_.begin()->first;
                int64_t wait_ms = std::max<int64_t>(0, deadline - NowMs());
                cv_.wait_for(lock, std::chrono::milliseconds(wait_ms));
            } else {
                cv_.wait(lock, [this] {
                    return !resume_queue_.empty() || !callback_queue_.empty();
                });
            }
        }
    }

    luaL_unref(main_L, LUA_REGISTRYINDEX, thread_ref);

    if (main_status == LUA_OK) return LUA_OK;

    spdlog::error("LuaRuntime: {}",
                  lua_tostring(co, -1) ? lua_tostring(co, -1) : "unknown error");
    lua_pop(co, 1);
    return main_status;
}

LuaRuntime::ResumeResult LuaRuntime::DoResume(AsyncHandle handle, std::vector<LuaValue> args) {
    lua_State* co = nullptr;
    int ref = LUA_NOREF;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = pending_.find(handle);
        if (it == pending_.end()) {
            spdlog::error("LuaRuntime::Resume: invalid handle {}", handle);
            return {nullptr, LUA_ERRRUN};
        }
        co = it->second.co;
        ref = it->second.registry_ref;
        pending_.erase(it);
    }

    PushValues(co, args);

    lua_State* main_L = lua_->lua_state();
    int nresults = 0;
    int status = lua_resume(co, main_L, static_cast<int>(args.size()), &nresults);

    spdlog::debug("DoResume handle={}: lua_resume status={}", handle, status);

    MaybeRecycleCallbackCo(co, status);
    luaL_unref(main_L, LUA_REGISTRYINDEX, ref);

    if (status != LUA_OK && status != LUA_YIELD) {
        spdlog::error("LuaRuntime::Resume: {}",
                      lua_tostring(co, -1) ? lua_tostring(co, -1) : "unknown error");
        lua_pop(co, 1);
    }
    return {co, status};
}

void LuaRuntime::PushValues(lua_State* L, const std::vector<LuaValue>& values) {
    for (const auto& v : values) {
        std::visit(
            [L](const auto& val) {
                using T = std::decay_t<decltype(val)>;
                if constexpr (std::is_same_v<T, std::nullptr_t>) {
                    lua_pushnil(L);
                } else if constexpr (std::is_same_v<T, bool>) {
                    lua_pushboolean(L, val ? 1 : 0);
                } else if constexpr (std::is_same_v<T, int64_t>) {
                    lua_pushinteger(L, static_cast<lua_Integer>(val));
                } else if constexpr (std::is_same_v<T, double>) {
                    lua_pushnumber(L, static_cast<lua_Number>(val));
                } else if constexpr (std::is_same_v<T, std::string>) {
                    lua_pushstring(L, val.c_str());
                }
            },
            v);
    }
}
