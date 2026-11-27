/*
 * MCP Server Implementation
 * Reference: https://modelcontextprotocol.io/specification/2024-11-05
 */

#include <algorithm>
#include <cstring>
#include <utility>
#include <esp_log.h>
#include <esp_app_desc.h>
#include "mcp_server.h"

#define TAG "MCP"

void McpServer::Configure(const McpServerCallbacks& callbacks) {
    callbacks_ = callbacks;
}

McpServer::McpServer() {}

McpServer::~McpServer() {
    for (auto tool : tools_) {
        delete tool;
    }
    tools_.clear();
}

void McpServer::AddTool(McpTool* tool) {
    if (std::find(tools_.begin(), tools_.end(), tool) != tools_.end()) {
        return;
    }
    std::unique_ptr<McpTool> owner(tool);
    if (tool == nullptr) {
        return;
    }
    if (std::find_if(tools_.begin(), tools_.end(), [tool](const McpTool* t) { return t->name() == tool->name(); }) !=
        tools_.end()) {
        ESP_LOGW(TAG, "Tool %s already added", tool->name().c_str());
        return;
    }

    ESP_LOGI(TAG, "Add tool: %s%s", tool->name().c_str(), tool->user_only() ? " [user]" : "");
    tools_.push_back(tool);
    owner.release();
}

void McpServer::AddTool(const std::string& name, const std::string& description, const PropertyList& properties,
                        std::function<ReturnValue(const PropertyList&)> callback) {
    AddTool(new McpTool(name, description, properties, callback));
}

void McpServer::AddUserOnlyTool(const std::string& name, const std::string& description, const PropertyList& properties,
                                std::function<ReturnValue(const PropertyList&)> callback) {
    auto tool = new McpTool(name, description, properties, callback);
    tool->set_user_only(true);
    AddTool(tool);
}

void McpServer::LogMalformed(const char* stage, size_t bytes) {
    const uint32_t rejected = malformed_messages_.fetch_add(1, std::memory_order_relaxed) + 1;
    if (rejected == 1 || rejected % 64 == 0)
        ESP_LOGW(TAG, "operation=parse stage=%s bytes=%u rejected=%u", stage, static_cast<unsigned>(bytes),
                 static_cast<unsigned>(rejected));
}

void McpServer::ParseMessage(const std::string& message) {
    if (message.size() > 64 * 1024) {
        LogMalformed("oversized", message.size());
        return;
    }
    McpJson json(cJSON_Parse(message.c_str()), cJSON_Delete);
    if (!json) {
        LogMalformed("json", message.size());
        return;
    }
    ParseMessage(json.get());
}

void McpServer::ParseCapabilities(const cJSON* capabilities) {
    std::string url_str, token_str;
    auto vision = cJSON_GetObjectItem(capabilities, "vision");
    if (cJSON_IsObject(vision)) {
        auto url = cJSON_GetObjectItem(vision, "url");
        auto token = cJSON_GetObjectItem(vision, "token");
        if (cJSON_IsString(url)) {
            url_str = url->valuestring;
            if (cJSON_IsString(token)) {
                token_str = token->valuestring;
            }
        }
    }
    // 每次初始化替换配置，避免重连后沿用上个会话的地址或凭据。
    if (callbacks_.configure_camera) {
        callbacks_.configure_camera(url_str, token_str);
    }
}

void McpServer::ParseMessage(const cJSON* json) try {
    // Check JSONRPC version
    auto version = cJSON_GetObjectItem(json, "jsonrpc");
    if (version == nullptr || !cJSON_IsString(version) || strcmp(version->valuestring, "2.0") != 0) {
        LogMalformed("jsonrpc_version");
        return;
    }

    // Check method
    auto method = cJSON_GetObjectItem(json, "method");
    if (method == nullptr || !cJSON_IsString(method)) {
        ESP_LOGE(TAG, "Missing method");
        return;
    }

    auto method_str = std::string(method->valuestring);
    if (method_str.find("notifications") == 0) {
        return;
    }

    // Check params
    auto params = cJSON_GetObjectItem(json, "params");
    if (params != nullptr && !cJSON_IsObject(params)) {
        LogMalformed("params");
        return;
    }

    auto id = cJSON_GetObjectItem(json, "id");
    if (!cJSON_IsNumber(id) || id->valuedouble != id->valueint) {
        LogMalformed("id");
        return;
    }
    auto id_int = id->valueint;

    if (method_str == "initialize") {
        ParseCapabilities(cJSON_GetObjectItem(params, "capabilities"));
        auto app_desc = esp_app_get_description();
        std::string message =
            "{\"protocolVersion\":\"2024-11-05\",\"capabilities\":{\"tools\":{}},\"serverInfo\":{\"name\":\"" BOARD_NAME
            "\",\"version\":\"";
        message += app_desc->version;
        message += "\"}}";
        ReplyResult(id_int, message);
    } else if (method_str == "tools/list") {
        std::string cursor_str = "";
        bool list_user_only_tools = false;
        if (params != nullptr) {
            auto cursor = cJSON_GetObjectItem(params, "cursor");
            if (cJSON_IsString(cursor)) {
                cursor_str = std::string(cursor->valuestring);
            }
            auto with_user_tools = cJSON_GetObjectItem(params, "withUserTools");
            if (cJSON_IsBool(with_user_tools)) {
                list_user_only_tools = with_user_tools->valueint == 1;
            }
        }
        GetToolsList(id_int, cursor_str, list_user_only_tools);
    } else if (method_str == "tools/call") {
        if (!cJSON_IsObject(params)) {
            ESP_LOGE(TAG, "tools/call: Missing params");
            ReplyError(id_int, "Missing params");
            return;
        }
        auto tool_name = cJSON_GetObjectItem(params, "name");
        if (!cJSON_IsString(tool_name)) {
            ESP_LOGE(TAG, "tools/call: Missing name");
            ReplyError(id_int, "Missing name");
            return;
        }
        auto tool_arguments = cJSON_GetObjectItem(params, "arguments");
        if (tool_arguments != nullptr && !cJSON_IsObject(tool_arguments)) {
            ESP_LOGE(TAG, "tools/call: Invalid arguments");
            ReplyError(id_int, "Invalid arguments");
            return;
        }
        DoToolCall(id_int, std::string(tool_name->valuestring), tool_arguments);
    } else {
        ESP_LOGW(TAG, "operation=dispatch result=unknown_method");
        ReplyError(id_int, "Method not implemented: " + method_str);
    }
} catch (const std::bad_alloc&) {
    ESP_LOGE(TAG, "operation=dispatch result=no_memory");
}

void McpServer::ReplyResult(int id, const std::string& result) {
    std::string payload = "{\"jsonrpc\":\"2.0\",\"id\":";
    payload += std::to_string(id) + ",\"result\":";
    payload += result;
    payload += "}";
    if (callbacks_.send) {
        callbacks_.send(payload);
    } else
        ESP_LOGE(TAG, "reply failed: message transport not configured");
}

void McpServer::ReplyError(int id, const std::string& message) {
    auto root = McpOwnJson(cJSON_CreateObject());
    McpRequireJson(cJSON_AddStringToObject(root.get(), "jsonrpc", "2.0"));
    McpRequireJson(cJSON_AddNumberToObject(root.get(), "id", id));
    auto error = McpOwnJson(cJSON_CreateObject());
    McpRequireJson(cJSON_AddStringToObject(error.get(), "message", message.c_str()));
    McpAttachObject(root.get(), "error", std::move(error));
    if (callbacks_.send) {
        callbacks_.send(McpPrintJson(root.get()));
    } else {
        ESP_LOGE(TAG, "reply failed: message transport not configured");
    }
}

void McpServer::GetToolsList(int id, const std::string& cursor, bool list_user_only_tools) {
    const int max_payload_size = 8000;
    std::string json = "{\"tools\":[";

    bool found_cursor = cursor.empty();
    auto it = tools_.begin();
    std::string next_cursor = "";

    while (it != tools_.end()) {
        // 如果我们还没有找到起始位置，继续搜索
        if (!found_cursor) {
            if ((*it)->name() == cursor) {
                found_cursor = true;
            } else {
                ++it;
                continue;
            }
        }

        if (!list_user_only_tools && (*it)->user_only()) {
            ++it;
            continue;
        }

        // 添加tool前检查大小
        std::string tool_json = (*it)->to_json() + ",";
        if (json.length() + tool_json.length() + 30 > max_payload_size) {
            // 如果添加这个tool会超出大小限制，设置next_cursor并退出循环
            next_cursor = (*it)->name();
            break;
        }

        json += tool_json;
        ++it;
    }

    if (json.back() == ',') {
        json.pop_back();
    }

    if (json.back() == '[' && !next_cursor.empty()) {
        // 如果没有添加任何tool，返回错误
        ESP_LOGE(TAG, "tools/list: Failed to add tool %s because of payload size limit", next_cursor.c_str());
        ReplyError(id, "Failed to add tool " + next_cursor + " because of payload size limit");
        return;
    }

    if (next_cursor.empty()) {
        json += "]}";
    } else {
        json += "],\"nextCursor\":\"" + next_cursor + "\"}";
    }

    ReplyResult(id, json);
}

void McpServer::DoToolCall(int id, const std::string& tool_name, const cJSON* tool_arguments) {
    auto tool_iter = std::find_if(tools_.begin(), tools_.end(),
                                  [&tool_name](const McpTool* tool) { return tool->name() == tool_name; });

    if (tool_iter == tools_.end()) {
        ESP_LOGW(TAG, "operation=dispatch result=unknown_tool id=%d", id);
        ReplyError(id, "Unknown tool: " + tool_name);
        return;
    }

    PropertyList arguments = (*tool_iter)->properties();
    try {
        for (auto& argument : arguments) {
            bool found = false;
            if (cJSON_IsObject(tool_arguments)) {
                auto value = cJSON_GetObjectItem(tool_arguments, argument.name().c_str());
                if (argument.type() == kPropertyTypeBoolean && cJSON_IsBool(value)) {
                    argument.set_value<bool>(value->valueint == 1);
                    found = true;
                } else if (argument.type() == kPropertyTypeInteger && cJSON_IsNumber(value) &&
                           value->valuedouble == value->valueint) {
                    argument.set_value<int>(value->valueint);
                    found = true;
                } else if (argument.type() == kPropertyTypeString && cJSON_IsString(value)) {
                    argument.set_value<std::string>(value->valuestring);
                    found = true;
                } else if (value != nullptr) {
                    ESP_LOGD(TAG, "tool call rejected id=%d stage=argument_type", id);
                    ReplyError(id, "Invalid argument: " + argument.name());
                    return;
                }
            }

            if (!argument.has_default_value() && !found) {
                ESP_LOGD(TAG, "tool call rejected id=%d stage=missing_argument", id);
                ReplyError(id, "Missing valid argument: " + argument.name());
                return;
            }
        }
    } catch (const std::bad_alloc&) {
        throw;
    } catch (const std::exception&) {
        ESP_LOGD(TAG, "tool call rejected id=%d stage=arguments", id);
        ReplyError(id, "工具参数无效，请检查请求。Invalid tool arguments.");
        return;
    }

    // 工具调用在语音工作任务中串行执行。
    if (!callbacks_.schedule || !callbacks_.schedule([this, id, tool_iter, arguments = std::move(arguments)]() {
            try {
                ReplyResult(id, (*tool_iter)->Call(arguments));
            } catch (const std::exception&) {
                ESP_LOGE(TAG, "tool call failed id=%d stage=execute", id);
                ReplyError(id, "工具执行失败，请重试。Tool execution failed. Please retry.");
            }
        })) {
        ReplyError(id, "Device is busy. Please retry.");
    }
}
