#include "lua_runtime.h"
#include "lua_extension.h"
#include <chrono>
#include <cstring>
#include <memory>

#include <spdlog/spdlog.h>

namespace {
template<class... Ts> struct overloaded : Ts... { using Ts::operator()...; };
template<class... Ts> overloaded(Ts...) -> overloaded<Ts...>;

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

    // 3. CodeProvider (async via async_simple + yieldk)
    if (rt->code_provider()) {
        if (!rt->executor()) {
            return luaL_error(L, "module '%s': executor required for CodeProvider", name);
        }

        auto rt_shared = rt->shared_from_this();
        auto handle = rt->PreYield(L);
        auto* exec = rt->executor();
        std::string module_name(name);  // safe capture for coroutine

        [rt_shared, handle, exec, module_name = std::move(module_name)]() mutable -> async_simple::coro::Lazy<void> {
            auto source = co_await rt_shared->code_provider()->LoadModule(module_name);
            if (source.has_value()) {
                rt_shared->Resume(handle, {std::move(*source)});
            } else {
                rt_shared->Resume(handle, {LuaValue{nullptr}});
            }
        }().via(exec).detach();

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
    if (!rt->executor()) {
        lua_pushnil(L);
        lua_pushfstring(L, "cannot load relative file '%s': no executor", filename);
        return 2;
    }

    auto rt_shared = rt->shared_from_this();
    auto handle = rt->PreYield(L);
    auto* exec = rt->executor();
    std::string file_path(filename);  // safe capture for coroutine

    [rt_shared, handle, exec, file_path = std::move(file_path)]() mutable -> async_simple::coro::Lazy<void> {
        auto source = co_await rt_shared->code_provider()->LoadFile(file_path);
        if (source.has_value()) {
            rt_shared->Resume(handle, {std::move(*source)});
        } else {
            rt_shared->Resume(handle, {LuaValue{nullptr}});
        }
    }().via(exec).detach();

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
                       async_simple::Executor* executor,
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
    rt->executor_ = executor;
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

void LuaRuntime::Start() {
    running_ = true;
    event_loop_thread_ = std::thread(&LuaRuntime::EventLoop, this);
}

void LuaRuntime::Stop() {
    running_.store(false, std::memory_order_release);
    cv_.notify_all();
    if (event_loop_thread_.joinable()) {
        event_loop_thread_.join();
    }
}

LuaRuntime::~LuaRuntime() {
    Stop();

    lua_State* main_L = lua_->lua_state();
    for (auto& ext : extensions_) {
        ext->OnShutdown(main_L);
    }
    for (auto& [deadline, entry] : timer_queue_) {
        if (entry.fn_ref != LUA_NOREF) {
            luaL_unref(main_L, LUA_REGISTRYINDEX, entry.fn_ref);
        }
    }
    while (!task_queue_.empty()) {
        auto& req = task_queue_.front();
        std::visit([&main_L, &req](const auto& kind) {
            using T = std::decay_t<decltype(kind)>;
            if constexpr (std::is_same_v<T, CallRef>) {
                luaL_unref(main_L, LUA_REGISTRYINDEX, kind.fn_ref);
            }
            req.promise.setValue(ScriptResult{LUA_ERRRUN, {}, "runtime shutdown"});
        }, req.kind);
        task_queue_.pop();
    }
    while (!release_queue_.empty()) {
        for (int ref : release_queue_.front()) {
            luaL_unref(main_L, LUA_REGISTRYINDEX, ref);
        }
        release_queue_.pop();
    }
    for (auto& [co, promise] : script_promises_) {
        promise.setValue(ScriptResult{LUA_ERRRUN, {}, "runtime shutdown"});
    }
    for (auto& [co, ref] : active_co_refs_) {
        luaL_unref(main_L, LUA_REGISTRYINDEX, ref);
    }
    for (auto& [co, ref] : co_pool_) {
        luaL_unref(main_L, LUA_REGISTRYINDEX, ref);
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

async_simple::coro::Lazy<ScriptResult> LuaRuntime::RunScript(const std::string& script) {
    async_simple::Promise<ScriptResult> promise;
    auto future = promise.getFuture();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        task_queue_.push({LoadScript{script, "=script"}, std::move(promise)});
    }
    cv_.notify_one();
    co_return co_await std::move(future);
}

async_simple::coro::Lazy<ScriptResult> LuaRuntime::RunFile(const std::string& filename) {
    async_simple::Promise<ScriptResult> promise;
    auto future = promise.getFuture();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        task_queue_.push({LoadScript{"", filename}, std::move(promise)});
    }
    cv_.notify_one();
    co_return co_await std::move(future);
}

async_simple::coro::Lazy<ScriptResult> LuaRuntime::CallFunction(int fn_ref, std::vector<LuaValue> args) {
    async_simple::Promise<ScriptResult> promise;
    auto future = promise.getFuture();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        task_queue_.push({CallRef{fn_ref, std::move(args), false}, std::move(promise)});
    }
    cv_.notify_one();
    co_return co_await std::move(future);
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

lua_State* LuaRuntime::AcquireCo() {
    lua_State* main_L = lua_->lua_state();
    if (!co_pool_.empty()) {
        auto [co, ref] = co_pool_.back();
        co_pool_.pop_back();
        active_co_refs_[co] = ref;
        return co;
    }

    lua_State* co = lua_newthread(main_L);
    int ref = luaL_ref(main_L, LUA_REGISTRYINDEX);
    SetExtraspace(co, this);
    active_co_refs_[co] = ref;
    return co;
}

void LuaRuntime::ReleaseCo(lua_State* co) {
    std::lock_guard<std::mutex> lock(mutex_);
    lua_settop(co, 0);
    auto it = active_co_refs_.find(co);
    if (it != active_co_refs_.end()) {
        co_pool_.push_back({co, it->second});
        active_co_refs_.erase(it);
    }
}

void LuaRuntime::MaybeRecycleCo(lua_State* co, int status, int nresults) {
    std::string error_msg;
    if (status != LUA_OK && status != LUA_YIELD) {
        const char* err = lua_tostring(co, -1);
        error_msg = err ? err : "unknown error";
        spdlog::error("LuaRuntime: {}", error_msg);
        lua_pop(co, 1);
    }
    if (status != LUA_YIELD) {
        // Fulfill pending script promise if any
        auto it = script_promises_.find(co);
        if (it != script_promises_.end()) {
            ScriptResult result;
            result.status = status;
            if (status == LUA_OK) {
                result.values = PeekValues(co, nresults);
            } else {
                result.error = std::move(error_msg);
            }
            it->second.setValue(std::move(result));
            script_promises_.erase(it);
        }
        ReleaseCo(co);
    }
}

void LuaRuntime::CallLuaFunction(int fn_ref, std::vector<LuaValue> args) {
    async_simple::Promise<ScriptResult> promise;  // unused, fire-and-forget
    {
        std::lock_guard<std::mutex> lock(mutex_);
        task_queue_.push({CallRef{fn_ref, std::move(args), false}, std::move(promise)});
    }
    cv_.notify_one();
}

void LuaRuntime::ReleaseRefs(std::vector<int> fn_refs) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        release_queue_.push(std::move(fn_refs));
    }
    cv_.notify_one();
}

AsyncHandle LuaRuntime::PreYield(lua_State* co) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto handle = next_handle_++;
    pending_[handle] = {co};
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

void LuaRuntime::ProcessExpiredTimers() {
    std::vector<TimerEntry> expired;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        while (!timer_queue_.empty() && timer_queue_.begin()->first <= NowMs()) {
            expired.push_back(std::move(timer_queue_.begin()->second));
            timer_queue_.erase(timer_queue_.begin());
        }
    }

    for (auto& entry : expired) {
        if (entry.type == TimerType::kSleep) {
            lua_State* main_L = lua_->lua_state();
            int nresults = 0;
            int status = lua_resume(entry.co, main_L, 0, &nresults);
            MaybeRecycleCo(entry.co, status, nresults);
        } else {
            async_simple::Promise<ScriptResult> promise;  // unused, fire-and-forget
            std::lock_guard<std::mutex> lock(mutex_);
            task_queue_.push({CallRef{entry.fn_ref, {}, true}, std::move(promise)});
        }
    }
}

bool LuaRuntime::DrainOneResume() {
    ResumeRequest req;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (resume_queue_.empty()) return false;
        req = std::move(resume_queue_.front());
        resume_queue_.pop();
    }
    DoResume(req.handle, std::move(req.args));
    return true;
}

bool LuaRuntime::DrainOneRelease() {
    std::vector<int> refs;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (release_queue_.empty()) return false;
        refs = std::move(release_queue_.front());
        release_queue_.pop();
    }
    lua_State* main_L = lua_->lua_state();
    for (int ref : refs) {
        luaL_unref(main_L, LUA_REGISTRYINDEX, ref);
    }
    return true;
}

bool LuaRuntime::DrainOneTask() {
    TaskRequest req;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (task_queue_.empty()) return false;
        req = std::move(task_queue_.front());
        task_queue_.pop();
    }

    lua_State* co = AcquireCo();
    lua_State* main_L = lua_->lua_state();
    int nargs = 0;

    std::visit(overloaded{
        [&](const LoadScript& s) {
            int load_result;
            if (s.chunk.empty()) {
                load_result = luaL_loadfile(co, s.name.c_str());
            } else {
                load_result = luaL_loadbuffer(co, s.chunk.c_str(), s.chunk.size(), s.name.c_str());
            }
            if (load_result != LUA_OK) {
                const char* err = lua_tostring(co, -1);
                spdlog::error("LuaRuntime: {}", err ? err : "unknown error");
                lua_pop(co, 1);
                ReleaseCo(co);
                ScriptResult result;
                result.status = load_result;
                result.error = err ? err : "unknown error";
                req.promise.setValue(std::move(result));
                co = nullptr;  // signal: no resume needed
            }
            // nargs stays 0 — loaded chunk takes no args
        },
        [&](const CallRef& c) {
            lua_rawgeti(co, LUA_REGISTRYINDEX, c.fn_ref);
            if (c.auto_unref) {
                luaL_unref(main_L, LUA_REGISTRYINDEX, c.fn_ref);
            }
            PushValues(co, c.args);
            nargs = static_cast<int>(c.args.size());
        }
    }, req.kind);

    if (!co) return true;  // load error, already handled

    int nresults = 0;
    int status = lua_resume(co, main_L, nargs, &nresults);

    script_promises_[co] = std::move(req.promise);

    if (status != LUA_YIELD) {
        MaybeRecycleCo(co, status, nresults);
    }
    return true;
}

void LuaRuntime::WaitOrTimeout() {
    std::unique_lock<std::mutex> lock(mutex_);
    if (!timer_queue_.empty()) {
        int64_t deadline = timer_queue_.begin()->first;
        int64_t wait_ms = std::max<int64_t>(0, deadline - NowMs());
        cv_.wait_for(lock, std::chrono::milliseconds(wait_ms), [this] {
            return !running_.load(std::memory_order_acquire) || !resume_queue_.empty() || !task_queue_.empty()
                   || !release_queue_.empty();
        });
    } else {
        cv_.wait(lock, [this] {
            return !running_.load(std::memory_order_acquire) || !resume_queue_.empty() || !task_queue_.empty()
                   || !release_queue_.empty();
        });
    }
}

void LuaRuntime::EventLoop() {
    while (running_.load(std::memory_order_acquire)) {
        ProcessExpiredTimers();
        while (DrainOneResume()) {}
        while (DrainOneTask()) {}
        while (DrainOneRelease()) {}
        WaitOrTimeout();
    }
}

LuaRuntime::ResumeResult LuaRuntime::DoResume(AsyncHandle handle, std::vector<LuaValue> args) {
    lua_State* co = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = pending_.find(handle);
        if (it == pending_.end()) {
            spdlog::error("LuaRuntime::Resume: invalid handle {}", handle);
            return {nullptr, LUA_ERRRUN};
        }
        co = it->second.co;
        pending_.erase(it);
    }
    PushValues(co, args);
    lua_State* main_L = lua_->lua_state();
    int nresults = 0;
    int status = lua_resume(co, main_L, static_cast<int>(args.size()), &nresults);
    spdlog::debug("DoResume handle={}: lua_resume status={}", handle, status);
    MaybeRecycleCo(co, status, nresults);
    return {co, status};
}

std::vector<LuaValue> LuaRuntime::PeekValues(lua_State* L, int nresults) {
    std::vector<LuaValue> result;
    result.reserve(nresults);
    for (int i = -nresults; i < 0; ++i) {
        int t = lua_type(L, i);
        if (t == LUA_TNIL) {
            result.push_back(nullptr);
        } else if (t == LUA_TBOOLEAN) {
            result.push_back(static_cast<bool>(lua_toboolean(L, i)));
        } else if (t == LUA_TNUMBER) {
            if (lua_isinteger(L, i)) {
                result.push_back(static_cast<int64_t>(lua_tointeger(L, i)));
            } else {
                result.push_back(static_cast<double>(lua_tonumber(L, i)));
            }
        } else if (t == LUA_TSTRING) {
            size_t len;
            const char* s = lua_tolstring(L, i, &len);
            result.push_back(std::string(s, len));
        } else {
            // table, function, userdata, thread — store as registry ref
            lua_pushvalue(L, i);
            int ref = luaL_ref(L, LUA_REGISTRYINDEX);
            result.push_back(LuaRef{ref, t});
        }
    }
    return result;
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
                } else if constexpr (std::is_same_v<T, LuaRef>) {
                    lua_rawgeti(L, LUA_REGISTRYINDEX, val.ref);
                }
            },
            v);
    }
}
