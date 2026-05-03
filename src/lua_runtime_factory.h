#pragma once

#include <async_simple/Executor.h>

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
    LuaRuntimeFactory& WithExecutor(async_simple::Executor& executor);
    LuaRuntimeFactory& RegisterExtension(std::shared_ptr<LuaExtension> extension);
    LuaRuntime::Ptr Create();

private:
    std::unordered_map<std::string, lua_CFunction> c_modules_;
    std::shared_ptr<CodeProvider> code_provider_;
    async_simple::Executor* executor_ = nullptr;
    std::vector<std::shared_ptr<LuaExtension>> extensions_;
};
