#pragma once

#include <asio/awaitable.hpp>
#include <optional>
#include <string>

class CodeProvider {
public:
    virtual ~CodeProvider() = default;
    virtual asio::awaitable<std::optional<std::string>> LoadModule(const std::string& module_name) = 0;
    virtual asio::awaitable<std::optional<std::string>> LoadFile(const std::string& path) = 0;
};
