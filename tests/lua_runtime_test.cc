#include "lua_runtime.h"
#include "lua_runtime_factory.h"
#include "lua_extension.h"

#include <gtest/gtest.h>

#include <asio.hpp>
#include <map>
#include <thread>

// --- Test CodeProvider (async via asio) ---

class TestCodeProvider : public CodeProvider {
public:
    asio::awaitable<std::optional<std::string>> LoadModule(const std::string& name) override {
        auto it = modules_.find(name);
        co_return it != modules_.end() ? std::optional(it->second) : std::nullopt;
    }
    asio::awaitable<std::optional<std::string>> LoadFile(const std::string& path) override {
        auto it = files_.find(path);
        co_return it != files_.end() ? std::optional(it->second) : std::nullopt;
    }
    void set_module(const std::string& name, const std::string& source) {
        modules_[name] = source;
    }
    void set_file(const std::string& path, const std::string& source) {
        files_[path] = source;
    }
private:
    std::map<std::string, std::string> modules_;
    std::map<std::string, std::string> files_;
};

class LuaRuntimeTest : public ::testing::Test {
protected:
    void SetUp() override {
        rt = LuaRuntimeFactory().Create();
    }

    LuaRuntime::Ptr rt;
};

// --- RunScript ---

TEST_F(LuaRuntimeTest, RunScriptReturnsOkOnSuccess) {
    EXPECT_EQ(rt->RunScript("print('hello')"), LUA_OK);
}

TEST_F(LuaRuntimeTest, RunScriptReturnsErrorOnSyntaxError) {
    EXPECT_NE(rt->RunScript("if true"), LUA_OK);
}

TEST_F(LuaRuntimeTest, RunScriptReturnsErrorOnRuntimeError) {
    EXPECT_NE(rt->RunScript("error('boom')"), LUA_OK);
}

TEST_F(LuaRuntimeTest, RunScriptCanReadGlobalSetFromC) {
    rt->lua()["x"] = 42;
    EXPECT_EQ(rt->RunScript("assert(x == 42)"), LUA_OK);
}

TEST_F(LuaRuntimeTest, RunScriptCanCallCFunction) {
    rt->lua().set_function("double_it", [](int v) { return v * 2; });
    EXPECT_EQ(rt->RunScript("assert(double_it(5) == 10)"), LUA_OK);
}

// --- FromLuaState ---

TEST_F(LuaRuntimeTest, FromLuaStateReturnsCorrectRuntime) {
    rt->lua()["get_rt"] = [this](sol::this_state s) -> int {
        auto ptr = LuaRuntime::FromLuaState(s);
        return ptr.get() == rt.get() ? 1 : 0;
    };
    EXPECT_EQ(rt->RunScript("assert(get_rt() == 1)"), LUA_OK);
}

// --- Async yield/resume: single ---

TEST_F(LuaRuntimeTest, AsyncNoArgs) {
    rt->lua().set_function("async_noop", [](lua_State* L) -> int {
        auto rt = LuaRuntime::FromLuaState(L);
        auto handle = rt->PreYield(L);
        std::thread([rt, handle]() { rt->Resume(handle); }).detach();
        return rt->Yield(L);
    });
    EXPECT_EQ(rt->RunScript("async_noop()"), LUA_OK);
}

TEST_F(LuaRuntimeTest, AsyncReturnsIntValue) {
    rt->lua().set_function("async_value", [](lua_State* L) -> int {
        auto rt = LuaRuntime::FromLuaState(L);
        auto handle = rt->PreYield(L);
        std::thread([rt, handle]() {
            rt->Resume(handle, {static_cast<int64_t>(99)});
        }).detach();
        return rt->Yield(L);
    });
    EXPECT_EQ(rt->RunScript(R"(
        local v = async_value()
        assert(v == 99, "expected 99 got " .. tostring(v))
    )"), LUA_OK);
}

TEST_F(LuaRuntimeTest, AsyncReturnsDoubleValue) {
    rt->lua().set_function("async_double", [](lua_State* L) -> int {
        auto rt = LuaRuntime::FromLuaState(L);
        auto handle = rt->PreYield(L);
        std::thread([rt, handle]() {
            rt->Resume(handle, {3.14});
        }).detach();
        return rt->Yield(L);
    });
    EXPECT_EQ(rt->RunScript(R"(
        local v = async_double()
        assert(math.abs(v - 3.14) < 0.001)
    )"), LUA_OK);
}

TEST_F(LuaRuntimeTest, AsyncReturnsBoolValue) {
    rt->lua().set_function("async_bool", [](lua_State* L) -> int {
        auto rt = LuaRuntime::FromLuaState(L);
        auto handle = rt->PreYield(L);
        std::thread([rt, handle]() {
            rt->Resume(handle, {LuaValue{true}});
        }).detach();
        return rt->Yield(L);
    });
    EXPECT_EQ(rt->RunScript(R"(
        local v = async_bool()
        assert(v == true)
    )"), LUA_OK);
}

TEST_F(LuaRuntimeTest, AsyncReturnsStringValue) {
    rt->lua().set_function("async_str", [](lua_State* L) -> int {
        auto rt = LuaRuntime::FromLuaState(L);
        auto handle = rt->PreYield(L);
        std::thread([rt, handle]() {
            rt->Resume(handle, {std::string("hello from C++")});
        }).detach();
        return rt->Yield(L);
    });
    EXPECT_EQ(rt->RunScript(R"(
        local v = async_str()
        assert(v == "hello from C++")
    )"), LUA_OK);
}

TEST_F(LuaRuntimeTest, AsyncReturnsNilValue) {
    rt->lua().set_function("async_nil", [](lua_State* L) -> int {
        auto rt = LuaRuntime::FromLuaState(L);
        auto handle = rt->PreYield(L);
        std::thread([rt, handle]() {
            rt->Resume(handle, {LuaValue{nullptr}});
        }).detach();
        return rt->Yield(L);
    });
    EXPECT_EQ(rt->RunScript("local v = async_nil(); assert(v == nil)"), LUA_OK);
}

// --- Multiple sequential async calls in one script ---

TEST_F(LuaRuntimeTest, SequentialAsyncCalls) {
    rt->lua().set_function("async_add", [](lua_State* L) -> int {
        int a = static_cast<int>(luaL_checkinteger(L, 1));
        int b = static_cast<int>(luaL_checkinteger(L, 2));
        auto rt = LuaRuntime::FromLuaState(L);
        auto handle = rt->PreYield(L);
        std::thread([rt, handle, a, b]() {
            rt->Resume(handle, {static_cast<int64_t>(a + b)});
        }).detach();
        return rt->Yield(L);
    });

    EXPECT_EQ(rt->RunScript(R"(
        local a = async_add(1, 2)
        local b = async_add(a, 3)
        local c = async_add(b, 4)
        assert(c == 10, "expected 10 got " .. tostring(c))
    )"), LUA_OK);
}

// --- Concurrent async calls on separate LuaRuntime instances ---

TEST_F(LuaRuntimeTest, ConcurrentRuntimes) {
    auto make_rt = []() {
        auto r = LuaRuntimeFactory().Create();
        r->lua().set_function("async_id", [](lua_State* L) -> int {
            int id = static_cast<int>(luaL_checkinteger(L, 1));
            auto rt = LuaRuntime::FromLuaState(L);
            auto handle = rt->PreYield(L);
            std::thread([rt, handle, id]() {
                rt->Resume(handle, {static_cast<int64_t>(id)});
            }).detach();
            return rt->Yield(L);
        });
        return r;
    };

    auto rt1 = make_rt();
    auto rt2 = make_rt();

    int r1 = LUA_ERRRUN, r2 = LUA_ERRRUN;
    std::thread t1([&r1, rt1]() {
        r1 = rt1->RunScript(R"(
            local v = async_id(1)
            assert(v == 1)
        )");
    });

    std::thread t2([&r2, rt2]() {
        r2 = rt2->RunScript(R"(
            local v = async_id(2)
            assert(v == 2)
        )");
    });

    t1.join();
    t2.join();

    EXPECT_EQ(r1, LUA_OK);
    EXPECT_EQ(r2, LUA_OK);
}

// --- Built-in: now, sleep, setTimeout ---

TEST_F(LuaRuntimeTest, NowReturnsMilliseconds) {
    EXPECT_EQ(rt->RunScript(R"(
        local t = now()
        assert(type(t) == "number")
        assert(t > 0)
        -- sleep 50ms and check that time advanced
        sleep(50)
        assert(now() - t >= 50, "elapsed too short: " .. tostring(now() - t))
    )"), LUA_OK);
}

TEST_F(LuaRuntimeTest, SleepBlocksForDuration) {
    EXPECT_EQ(rt->RunScript(R"(
        local t = now()
        sleep(100)
        local elapsed = now() - t
        assert(elapsed >= 80, "slept too short: " .. tostring(elapsed) .. "ms")
    )"), LUA_OK);
}

TEST_F(LuaRuntimeTest, SetTimeoutCallsCallback) {
    EXPECT_EQ(rt->RunScript(R"(
        local done = false
        local t = now()
        setTimeout(80, function()
            done = true
        end)
        sleep(150)
        assert(done, "callback not called after timeout")
    )"), LUA_OK);
}

TEST_F(LuaRuntimeTest, SetTimeoutWithMultipleCallbacks) {
    // setTimeout is non-blocking and concurrent — callbacks fire by delay.
    EXPECT_EQ(rt->RunScript(R"(
        local order = {}
        setTimeout(100, function() order[#order + 1] = "a" end)
        setTimeout(50, function()  order[#order + 1] = "b" end)
        setTimeout(25, function()  order[#order + 1] = "c" end)
        sleep(200)
        assert(order[1] == "c", "expected c first, got " .. tostring(order[1]))
        assert(order[2] == "b", "expected b second, got " .. tostring(order[2]))
        assert(order[3] == "a", "expected a third, got " .. tostring(order[3]))
    )"), LUA_OK);
}

TEST_F(LuaRuntimeTest, SetTimeoutReturnsHandle) {
    EXPECT_EQ(rt->RunScript(R"(
        local t = setTimeout(100, function() end)
        assert(type(t) == "number", "expected number handle, got " .. type(t))
        assert(t > 0, "expected positive handle")
    )"), LUA_OK);
}

TEST_F(LuaRuntimeTest, ClearTimeoutPreventsCallback) {
    EXPECT_EQ(rt->RunScript(R"(
        local done = false
        local t = setTimeout(50, function() done = true end)
        clearTimeout(t)
        sleep(100)
        assert(done == false, "callback should not fire after clearTimeout")
    )"), LUA_OK);
}

TEST_F(LuaRuntimeTest, ClearTimeoutWithMultipleTimers) {
    EXPECT_EQ(rt->RunScript(R"(
        local results = {}
        local t1 = setTimeout(100, function() results[#results + 1] = "a" end)
        local t2 = setTimeout(50, function()  results[#results + 1] = "b" end)
        local t3 = setTimeout(25, function()  results[#results + 1] = "c" end)
        clearTimeout(t1)  -- cancel the 100ms one
        sleep(200)
        assert(#results == 2, "expected 2 callbacks, got " .. tostring(#results))
        assert(results[1] == "c", "expected c first")
        assert(results[2] == "b", "expected b second")
    )"), LUA_OK);
}

TEST_F(LuaRuntimeTest, ClearTimeoutOnFiredTimerIsHarmless) {
    EXPECT_EQ(rt->RunScript(R"(
        local done = false
        local t = setTimeout(50, function() done = true end)
        sleep(100)
        assert(done == true, "callback should have fired")
        clearTimeout(t)  -- no-op, should not crash
    )"), LUA_OK);
}

// --- Custom require and loadfile ---

class LuaRuntimeWithProviderTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto p = std::make_unique<TestCodeProvider>();
        provider = p.get();
        rt = LuaRuntimeFactory()
            .WithIoContext(io)
            .WithCodeProvider(std::move(p))
            .Create();
        io_thread = std::thread([this]() { io.run(); });
    }
    void TearDown() override {
        io.stop();
        if (io_thread.joinable()) io_thread.join();
        io.restart();
    }
    asio::io_context io;
    asio::executor_work_guard<asio::io_context::executor_type> work{io.get_executor()};
    std::thread io_thread;
    TestCodeProvider* provider = nullptr;
    LuaRuntime::Ptr rt;
};

TEST_F(LuaRuntimeWithProviderTest, RequireLuaModuleViaCodeProvider) {
    provider->set_module("greet", "return { hello = function() return 'hi' end }");

    EXPECT_EQ(rt->RunScript(R"(
        local greet = require("greet")
        assert(greet.hello() == "hi")
    )"), LUA_OK);
}

TEST_F(LuaRuntimeWithProviderTest, RequireModuleNotFound) {
    EXPECT_NE(rt->RunScript(R"(
        require("nonexistent_module")
    )"), LUA_OK);
}

TEST_F(LuaRuntimeWithProviderTest, RequireCaching) {
    provider->set_module("counter", R"(
        _G._load_count = (_G._load_count or 0) + 1
        return { count = _G._load_count }
    )");

    EXPECT_EQ(rt->RunScript(R"(
        local a = require("counter")
        local b = require("counter")
        assert(a.count == 1, "expected 1 got " .. tostring(a.count))
        assert(a == b, "expected same table on second require")
    )"), LUA_OK);
}

TEST_F(LuaRuntimeWithProviderTest, LoadfileRelativePath) {
    provider->set_file("test.lua", "return 42 + 1");

    EXPECT_EQ(rt->RunScript(R"(
        local fn = loadfile("test.lua")
        assert(type(fn) == "function")
        local result = fn()
        assert(result == 43)
    )"), LUA_OK);
}

TEST_F(LuaRuntimeWithProviderTest, LoadfileRelativeNotFound) {
    EXPECT_EQ(rt->RunScript(R"(
        local fn, err = loadfile("missing.lua")
        assert(fn == nil, "expected nil, got " .. tostring(fn))
        assert(err ~= nil, "expected error message")
    )"), LUA_OK);
}

TEST_F(LuaRuntimeWithProviderTest, LoadfileAbsolutePath) {
    EXPECT_EQ(rt->RunScript(R"(
        local fn, err = loadfile("/nonexistent/path.lua")
        assert(fn == nil, "expected nil for nonexistent file")
        assert(err ~= nil, "expected error message")
    )"), LUA_OK);
}

class LuaRuntimeRegisterTest : public ::testing::Test {
protected:
    static int luaopen_testmath(lua_State* L) {
        lua_newtable(L);
        lua_pushcfunction(L, [](lua_State* L) -> int {
            int a = static_cast<int>(luaL_checkinteger(L, 1));
            int b = static_cast<int>(luaL_checkinteger(L, 2));
            lua_pushinteger(L, a * b);
            return 1;
        });
        lua_setfield(L, -2, "mul");
        return 1;
    }

    void SetUp() override {
        rt = LuaRuntimeFactory()
            .Register("testmath", luaopen_testmath)
            .Create();
    }

    LuaRuntime::Ptr rt;
};

TEST_F(LuaRuntimeRegisterTest, RequireCModule) {
    EXPECT_EQ(rt->RunScript(R"(
        local m = require("testmath")
        assert(m.mul(3, 4) == 12)
    )"), LUA_OK);
}

TEST_F(LuaRuntimeRegisterTest, RequireCModuleCached) {
    EXPECT_EQ(rt->RunScript(R"(
        local a = require("testmath")
        local b = require("testmath")
        assert(a == b, "expected same table")
    )"), LUA_OK);
}

class LuaRuntimeFluentBuilderTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto provider = std::make_unique<TestCodeProvider>();
        provider->set_module("util", "return { answer = 42 }");
        provider->set_module("helper", "return { greet = function() return 'hello' end }");
        rt = LuaRuntimeFactory()
            .WithIoContext(io)
            .Register("util", [](lua_State* L) -> int {
                lua_newtable(L);
                lua_pushstring(L, "from_c");
                lua_setfield(L, -2, "source");
                return 1;
            })
            .WithCodeProvider(std::move(provider))
            .Create();
        io_thread = std::thread([this]() { io.run(); });
    }
    void TearDown() override {
        io.stop();
        if (io_thread.joinable()) io_thread.join();
        io.restart();
    }
    asio::io_context io;
    asio::executor_work_guard<asio::io_context::executor_type> work{io.get_executor()};
    std::thread io_thread;
    LuaRuntime::Ptr rt;
};

TEST_F(LuaRuntimeFluentBuilderTest, FluentChainingWorks) {
    EXPECT_EQ(rt->RunScript(R"(
        local u = require("util")
        assert(u.source == "from_c", "expected C module via preload")
        local h = require("helper")
        assert(h.greet() == "hello", "expected Lua module via CodeProvider")
    )"), LUA_OK);
}

// --- C++ side CallLuaFunction ---

TEST_F(LuaRuntimeTest, CallLuaFunctionFromCpp) {
    lua_State* main_L = rt->lua().lua_state();

    lua_pushlightuserdata(main_L, main_L);
    lua_pushcclosure(main_L, [](lua_State* L) -> int {
        luaL_checktype(L, 1, LUA_TFUNCTION);
        auto rt = LuaRuntime::FromLuaState(L);
        lua_pushvalue(L, 1);
        int fn_ref = luaL_ref(L, LUA_REGISTRYINDEX);

        std::thread([rt, fn_ref]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            rt->CallLuaFunction(fn_ref);
        }).detach();
        return 0;
    }, 1);
    lua_setglobal(main_L, "store_callback");

    EXPECT_EQ(rt->RunScript(R"(
        local done = false
        store_callback(function()
            done = true
        end)
        while not done do sleep(10) end
        assert(done)
    )"), LUA_OK);
}

TEST_F(LuaRuntimeTest, CallLuaFunctionWithArgs) {
    lua_State* main_L = rt->lua().lua_state();

    lua_pushlightuserdata(main_L, main_L);
    lua_pushcclosure(main_L, [](lua_State* L) -> int {
        luaL_checktype(L, 1, LUA_TFUNCTION);
        auto rt = LuaRuntime::FromLuaState(L);
        lua_pushvalue(L, 1);
        int fn_ref = luaL_ref(L, LUA_REGISTRYINDEX);

        std::thread([rt, fn_ref]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            rt->CallLuaFunction(fn_ref, {static_cast<int64_t>(42), std::string("hello")});
        }).detach();
        return 0;
    }, 1);
    lua_setglobal(main_L, "store_callback");

    EXPECT_EQ(rt->RunScript(R"(
        local got_a, got_b = nil, nil
        store_callback(function(a, b)
            got_a = a
            got_b = b
        end)
        while got_a == nil do sleep(10) end
        assert(got_a == 42, "expected 42 got " .. tostring(got_a))
        assert(got_b == "hello", "expected hello got " .. tostring(got_b))
    )"), LUA_OK);
}

// --- Factory: shared config across multiple runtimes ---

class LuaRuntimeFactoryTest : public ::testing::Test {
protected:
    static int luaopen_testmath(lua_State* L) {
        lua_newtable(L);
        lua_pushcfunction(L, [](lua_State* L) -> int {
            int a = static_cast<int>(luaL_checkinteger(L, 1));
            int b = static_cast<int>(luaL_checkinteger(L, 2));
            lua_pushinteger(L, a * b);
            return 1;
        });
        lua_setfield(L, -2, "mul");
        return 1;
    }

    void SetUp() override {
        auto p = std::make_unique<TestCodeProvider>();
        provider = p.get();
        provider->set_module("greet", "return { hello = function() return 'hi' end }");

        factory = std::make_unique<LuaRuntimeFactory>();
        factory->WithIoContext(io);
        factory->WithCodeProvider(std::move(p));
        factory->Register("testmath", luaopen_testmath);

        io_thread = std::thread([this]() { io.run(); });
    }
    void TearDown() override {
        io.stop();
        if (io_thread.joinable()) io_thread.join();
        io.restart();
    }

    asio::io_context io;
    asio::executor_work_guard<asio::io_context::executor_type> work{io.get_executor()};
    std::thread io_thread;
    TestCodeProvider* provider = nullptr;
    std::unique_ptr<LuaRuntimeFactory> factory;
};

TEST_F(LuaRuntimeFactoryTest, MultipleRuntimesShareCodeProvider) {
    auto rt1 = factory->Create();
    auto rt2 = factory->Create();

    EXPECT_EQ(rt1->RunScript("local g = require('greet'); assert(g.hello() == 'hi')"), LUA_OK);
    EXPECT_EQ(rt2->RunScript("local g = require('greet'); assert(g.hello() == 'hi')"), LUA_OK);
}

TEST_F(LuaRuntimeFactoryTest, MultipleRuntimesShareCModules) {
    auto rt1 = factory->Create();
    auto rt2 = factory->Create();

    EXPECT_EQ(rt1->RunScript("local m = require('testmath'); assert(m.mul(3, 4) == 12)"), LUA_OK);
    EXPECT_EQ(rt2->RunScript("local m = require('testmath'); assert(m.mul(5, 6) == 30)"), LUA_OK);
}

TEST_F(LuaRuntimeFactoryTest, MultipleRuntimesAreIndependent) {
    auto rt1 = factory->Create();
    auto rt2 = factory->Create();

    rt1->lua()["x"] = 100;
    rt2->lua()["x"] = 200;

    EXPECT_EQ(rt1->RunScript("assert(x == 100)"), LUA_OK);
    EXPECT_EQ(rt2->RunScript("assert(x == 200)"), LUA_OK);
}

// --- LuaExtension ---

class TestExtension : public LuaExtension {
public:
    void OnInit(lua_State* L) override {
        lua_pushinteger(L, 42);
        lua_setglobal(L, "magic_number");
        init_count++;
    }
    void OnShutdown(lua_State* L) override { shutdown_count++; }

    int init_count = 0;
    int shutdown_count = 0;
};

TEST_F(LuaRuntimeFactoryTest, ExtensionOnInitCalled) {
    auto ext = std::make_shared<TestExtension>();
    factory->RegisterExtension(ext);

    auto rt = factory->Create();
    EXPECT_EQ(ext->init_count, 1);
    EXPECT_EQ(rt->RunScript("assert(magic_number == 42)"), LUA_OK);
}

TEST_F(LuaRuntimeFactoryTest, ExtensionOnShutdownCalled) {
    auto ext = std::make_shared<TestExtension>();
    factory->RegisterExtension(ext);

    {
        auto rt = factory->Create();
        EXPECT_EQ(ext->shutdown_count, 0);
    }
    EXPECT_EQ(ext->shutdown_count, 1);
}

TEST_F(LuaRuntimeFactoryTest, ExtensionSharedAcrossRuntimes) {
    auto ext = std::make_shared<TestExtension>();
    factory->RegisterExtension(ext);

    auto rt1 = factory->Create();
    EXPECT_EQ(ext->init_count, 1);
    auto rt2 = factory->Create();
    EXPECT_EQ(ext->init_count, 2);

    EXPECT_EQ(rt1->RunScript("assert(magic_number == 42)"), LUA_OK);
    EXPECT_EQ(rt2->RunScript("assert(magic_number == 42)"), LUA_OK);
}
