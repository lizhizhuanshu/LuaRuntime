#pragma once

#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

class Node;

class TreeParser {
public:
    static std::unique_ptr<Node> Parse(const std::string& json_str);

private:
    static std::unique_ptr<Node> ParseNode(const nlohmann::json& j, uint32_t& next_id);
    static std::vector<std::unique_ptr<Node>> ParseChildren(const nlohmann::json& j, uint32_t& next_id);
    static std::unique_ptr<Node> ParseComposite(const nlohmann::json& j, uint32_t& next_id);
    static std::unique_ptr<Node> ParseScriptLeaf(const nlohmann::json& j, uint32_t& next_id);
    static void ApplyDecorators(const nlohmann::json& j, Node* node);
};
