#include "lua_runtime_factory.h"

LuaRuntimeFactory& LuaRuntimeFactory::Register(const std::string& name, lua_CFunction openf) {
    c_modules_[name] = openf;
    return *this;
}

LuaRuntimeFactory& LuaRuntimeFactory::WithCodeProvider(std::shared_ptr<CodeProvider> provider) {
    code_provider_ = std::move(provider);
    return *this;
}

LuaRuntimeFactory& LuaRuntimeFactory::WithExecutor(async_simple::Executor& executor) {
    executor_ = &executor;
    return *this;
}

LuaRuntimeFactory& LuaRuntimeFactory::RegisterExtension(std::shared_ptr<LuaExtension> extension) {
    extensions_.push_back(std::move(extension));
    return *this;
}

LuaRuntime::Ptr LuaRuntimeFactory::Create() {
    auto rt = std::shared_ptr<LuaRuntime>(new LuaRuntime());
    LuaRuntime::Setup(rt->lua(), code_provider_, c_modules_, executor_, extensions_);
    return rt;
}
