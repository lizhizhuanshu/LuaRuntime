#pragma once

#include <asio.hpp>

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "lua_runtime.h"
#include "lua_extension.h"

class LuaRuntimeFactory {
public:
    LuaRuntimeFactory& Register(const std::string& name, lua_CFunction openf);
    LuaRuntimeFactory& WithCodeProvider(std::shared_ptr<CodeProvider> provider);
    LuaRuntimeFactory& WithIoContext(asio::io_context& ctx);
    LuaRuntimeFactory& RegisterExtension(std::shared_ptr<LuaExtension> extension);
    LuaRuntime::Ptr Create();

private:
    std::unordered_map<std::string, lua_CFunction> c_modules_;
    std::shared_ptr<CodeProvider> code_provider_;
    asio::io_context* io_context_ = nullptr;
    std::vector<std::shared_ptr<LuaExtension>> extensions_;
};
