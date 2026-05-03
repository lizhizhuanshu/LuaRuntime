cmake_minimum_required(VERSION 3.26)

project(lua_runtime LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)

# ── Dependencies ────────────────────────────────────────────────────────────

# Asio (header-only)
FetchContent_Declare(
    asio
    GIT_REPOSITORY https://github.com/chriskohlhoff/asio.git
    GIT_TAG        asio-1-38-0
)
FetchContent_MakeAvailable(asio)
add_library(asio_iface INTERFACE)
target_include_directories(asio_iface INTERFACE ${asio_SOURCE_DIR}/asio/include)
target_compile_definitions(asio_iface INTERFACE ASIO_STANDALONE ASIO_NO_DEPRECATED)

# Lua (static library from source)
FetchContent_Declare(
    lua
    GIT_REPOSITORY https://github.com/lua/lua.git
    GIT_TAG        v5.4.8
)
FetchContent_MakeAvailable(lua)
file(GLOB LUA_SOURCES ${lua_SOURCE_DIR}/*.c)
list(FILTER LUA_SOURCES EXCLUDE REGEX ".*ltests\\.c|.*onelua\\.c")
add_library(lua STATIC ${LUA_SOURCES})
target_include_directories(lua PUBLIC ${lua_SOURCE_DIR})
set_property(TARGET lua PROPERTY C_STANDARD 99)
if(UNIX)
    target_link_libraries(lua PRIVATE dl m)
endif()

# sol2 (header-only, uses fork with local modifications)
set(SOL2_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    sol2
    GIT_REPOSITORY https://github.com/lizhizhuanshu/sol2.git
    GIT_TAG        v3.5.0
)
FetchContent_MakeAvailable(sol2)

# spdlog (header-only)
set(SPDLOG_HEADER_ONLY ON CACHE BOOL "" FORCE)
FetchContent_Declare(
    spdlog
    GIT_REPOSITORY https://github.com/gabime/spdlog.git
    GIT_TAG        v1.17.0
)
FetchContent_MakeAvailable(spdlog)

# ── LuaRuntime library ─────────────────────────────────────────────────────

add_library(lua_runtime_lib STATIC
    src/lua_runtime.cc
    src/lua_runtime_factory.cc
)
target_include_directories(lua_runtime_lib PUBLIC src)
target_link_libraries(lua_runtime_lib PUBLIC
    sol2::sol2
    spdlog::spdlog_header_only
    lua
    asio_iface
)

# ── Tests ──────────────────────────────────────────────────────────────────

enable_testing()
FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        v1.14.0
)
FetchContent_MakeAvailable(googletest)

file(GLOB TEST_SOURCES tests/*.cc)
add_executable(lua_runtime_test ${TEST_SOURCES})
target_link_libraries(lua_runtime_test PRIVATE lua_runtime_lib GTest::gtest_main)
include(GoogleTest)
gtest_discover_tests(lua_runtime_test)
