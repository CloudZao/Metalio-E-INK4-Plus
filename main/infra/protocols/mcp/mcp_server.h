#ifndef MCP_SERVER_H
#define MCP_SERVER_H

#include <memory>
#include <new>
#include <type_traits>
#include <utility>
#include <string>
#include <atomic>
#include <cstdint>
#include <cstddef>
#include <vector>
#include <functional>
#include <variant>
#include <optional>
#include <stdexcept>
#include <mbedtls/base64.h>
#include <cJSON.h>

using McpJson = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>;

static inline cJSON* McpRequireJson(cJSON* value) {
    if (value == nullptr) {
        throw std::bad_alloc();
    }
    return value;
}

static inline McpJson McpOwnJson(cJSON* value) {
    return McpJson(McpRequireJson(value), cJSON_Delete);
}

static inline void McpAttachObject(cJSON* parent, const char* key, McpJson child) {
    if (!cJSON_AddItemToObject(parent, key, child.get())) {
        throw std::bad_alloc();
    }
    child.release();
}

static inline void McpAttachArray(cJSON* parent, McpJson child) {
    if (!cJSON_AddItemToArray(parent, child.get())) {
        throw std::bad_alloc();
    }
    child.release();
}

static inline std::string McpPrintJson(const cJSON* value) {
    std::unique_ptr<char, decltype(&cJSON_free)> text(cJSON_PrintUnformatted(value), cJSON_free);
    if (!text) {
        throw std::bad_alloc();
    }
    return std::string(text.get());
}

class ImageContent {
private:
    std::string encoded_data_;
    std::string mime_type_;

    static std::string Base64Encode(const std::string& data) {
        if (data.empty()) {
            return {};
        }
        size_t capacity = 0;
        const auto* input = reinterpret_cast<const unsigned char*>(data.data());
        const int size_result = mbedtls_base64_encode(nullptr, 0, &capacity, input, data.size());
        if (size_result != MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL) {
            throw std::runtime_error("Base64 size calculation failed");
        }
        std::string result(capacity, '\0');
        size_t length = 0;
        if (mbedtls_base64_encode(reinterpret_cast<unsigned char*>(result.data()), result.size(), &length, input,
                                  data.size()) != 0) {
            throw std::runtime_error("Base64 encoding failed");
        }
        result.resize(length);
        return result;
    }

public:
    /** @brief 编码图片字节及媒体类型，供工具结果序列化 */
    ImageContent(const std::string& mime_type, const std::string& data) {
        mime_type_ = mime_type;
        encoded_data_ = Base64Encode(data);
    }

    /** @brief 序列化当前对象；分配失败抛出 bad_alloc，由分发层处理 */
    std::string to_json() const {
        auto json = McpOwnJson(cJSON_CreateObject());
        McpRequireJson(cJSON_AddStringToObject(json.get(), "type", "image"));
        McpRequireJson(cJSON_AddStringToObject(json.get(), "mimeType", mime_type_.c_str()));
        McpRequireJson(cJSON_AddStringToObject(json.get(), "data", encoded_data_.c_str()));
        return McpPrintJson(json.get());
    }
};

// 工具返回的 cJSON 或图片指针由 McpTool::Call 接管并释放。
using ReturnValue = std::variant<bool, int, std::string, cJSON*, ImageContent*>;

enum PropertyType {
    kPropertyTypeBoolean,  // 布尔参数
    kPropertyTypeInteger,  // 整数参数
    kPropertyTypeString    // 字符串参数
};

class Property {
private:
    std::string name_;
    PropertyType type_;
    std::variant<bool, int, std::string> value_;
    bool has_default_value_;
    std::optional<int> min_value_;  // 整数最小值
    std::optional<int> max_value_;  // 整数最大值

public:
    /** @brief 声明无默认值的必填工具参数 */
    Property(const std::string& name, PropertyType type) : name_(name), type_(type), has_default_value_(false) {}

    /** @brief 声明带默认值的可选工具参数 */
    template <typename T>
    Property(const std::string& name, PropertyType type, const T& default_value)
        : name_(name), type_(type), has_default_value_(true) {
        value_ = default_value;
    }

    /** @brief 声明具有整数上下限的必填参数，其他参数类型会被拒绝 */
    Property(const std::string& name, PropertyType type, int min_value, int max_value)
        : name_(name), type_(type), has_default_value_(false), min_value_(min_value), max_value_(max_value) {
        if (type != kPropertyTypeInteger) {
            throw std::invalid_argument("Range limits only apply to integer properties");
        }
    }

    /** @brief 声明具有默认值及整数上下限的可选参数，默认值须在范围内 */
    Property(const std::string& name, PropertyType type, int default_value, int min_value, int max_value)
        : name_(name), type_(type), has_default_value_(true), min_value_(min_value), max_value_(max_value) {
        if (type != kPropertyTypeInteger) {
            throw std::invalid_argument("Range limits only apply to integer properties");
        }
        if (default_value < min_value || default_value > max_value) {
            throw std::invalid_argument("Default value must be within the specified range");
        }
        value_ = default_value;
    }

    inline const std::string& name() const {
        return name_;
    }
    inline PropertyType type() const {
        return type_;
    }
    inline bool has_default_value() const {
        return has_default_value_;
    }
    inline bool has_range() const {
        return min_value_.has_value() && max_value_.has_value();
    }
    inline int min_value() const {
        return min_value_.value_or(0);
    }
    inline int max_value() const {
        return max_value_.value_or(0);
    }

    template <typename T>
    inline T value() const {
        return std::get<T>(value_);
    }

    template <typename T>
    inline void set_value(const T& value) {
        // 添加对设置的整数值进行范围检查
        if constexpr (std::is_same_v<T, int>) {
            if (min_value_.has_value() && value < min_value_.value()) {
                throw std::invalid_argument("Value is below minimum allowed: " + std::to_string(min_value_.value()));
            }
            if (max_value_.has_value() && value > max_value_.value()) {
                throw std::invalid_argument("Value exceeds maximum allowed: " + std::to_string(max_value_.value()));
            }
        }
        value_ = value;
    }

    /** @brief 序列化当前对象；分配失败抛出 bad_alloc，由分发层处理 */
    std::string to_json() const {
        auto json = McpOwnJson(cJSON_CreateObject());
        if (type_ == kPropertyTypeBoolean) {
            McpRequireJson(cJSON_AddStringToObject(json.get(), "type", "boolean"));
            if (has_default_value_) {
                McpRequireJson(cJSON_AddBoolToObject(json.get(), "default", value<bool>()));
            }
        } else if (type_ == kPropertyTypeInteger) {
            McpRequireJson(cJSON_AddStringToObject(json.get(), "type", "integer"));
            if (has_default_value_) {
                McpRequireJson(cJSON_AddNumberToObject(json.get(), "default", value<int>()));
            }
            if (min_value_) {
                McpRequireJson(cJSON_AddNumberToObject(json.get(), "minimum", *min_value_));
            }
            if (max_value_) {
                McpRequireJson(cJSON_AddNumberToObject(json.get(), "maximum", *max_value_));
            }
        } else if (type_ == kPropertyTypeString) {
            McpRequireJson(cJSON_AddStringToObject(json.get(), "type", "string"));
            if (has_default_value_) {
                McpRequireJson(cJSON_AddStringToObject(json.get(), "default", value<std::string>().c_str()));
            }
        }
        return McpPrintJson(json.get());
    }
};

class PropertyList {
private:
    std::vector<Property> properties_;

public:
    PropertyList() = default;
    PropertyList(const std::vector<Property>& properties) : properties_(properties) {}
    void AddProperty(const Property& property) {
        properties_.push_back(property);
    }

    const Property& operator[](const std::string& name) const {
        for (const auto& property : properties_) {
            if (property.name() == name) {
                return property;
            }
        }
        throw std::runtime_error("Property not found: " + name);
    }

    auto begin() {
        return properties_.begin();
    }
    auto end() {
        return properties_.end();
    }

    std::vector<std::string> GetRequired() const {
        std::vector<std::string> required;
        for (auto& property : properties_) {
            if (!property.has_default_value()) {
                required.push_back(property.name());
            }
        }
        return required;
    }

    /** @brief 序列化当前对象；分配失败抛出 bad_alloc，由分发层处理 */
    std::string to_json() const {
        auto json = McpOwnJson(cJSON_CreateObject());
        for (const auto& property : properties_) {
            auto property_json = McpOwnJson(cJSON_Parse(property.to_json().c_str()));
            McpAttachObject(json.get(), property.name().c_str(), std::move(property_json));
        }
        return McpPrintJson(json.get());
    }
};

class McpTool {
private:
    std::string name_;
    std::string description_;
    PropertyList properties_;
    std::function<ReturnValue(const PropertyList&)> callback_;
    bool user_only_ = false;

public:
    McpTool(const std::string& name, const std::string& description, const PropertyList& properties,
            std::function<ReturnValue(const PropertyList&)> callback)
        : name_(name), description_(description), properties_(properties), callback_(callback) {}

    void set_user_only(bool user_only) {
        user_only_ = user_only;
    }
    inline const std::string& name() const {
        return name_;
    }
    inline const std::string& description() const {
        return description_;
    }
    inline const PropertyList& properties() const {
        return properties_;
    }
    inline bool user_only() const {
        return user_only_;
    }

    /** @brief 序列化当前对象；分配失败抛出 bad_alloc，由分发层处理 */
    std::string to_json() const {
        const auto required = properties_.GetRequired();
        auto json = McpOwnJson(cJSON_CreateObject());
        McpRequireJson(cJSON_AddStringToObject(json.get(), "name", name_.c_str()));
        McpRequireJson(cJSON_AddStringToObject(json.get(), "description", description_.c_str()));
        auto input_schema = McpOwnJson(cJSON_CreateObject());
        McpRequireJson(cJSON_AddStringToObject(input_schema.get(), "type", "object"));
        McpAttachObject(input_schema.get(), "properties", McpOwnJson(cJSON_Parse(properties_.to_json().c_str())));
        if (!required.empty()) {
            auto required_array = McpOwnJson(cJSON_CreateArray());
            for (const auto& property : required) {
                McpAttachArray(required_array.get(), McpOwnJson(cJSON_CreateString(property.c_str())));
            }
            McpAttachObject(input_schema.get(), "required", std::move(required_array));
        }
        McpAttachObject(json.get(), "inputSchema", std::move(input_schema));
        if (user_only_) {
            auto annotations = McpOwnJson(cJSON_CreateObject());
            auto audience = McpOwnJson(cJSON_CreateArray());
            McpAttachArray(audience.get(), McpOwnJson(cJSON_CreateString("user")));
            McpAttachObject(annotations.get(), "audience", std::move(audience));
            McpAttachObject(json.get(), "annotations", std::move(annotations));
        }
        return McpPrintJson(json.get());
    }

    /** @brief 执行工具回调并接管其结果资源，生成 MCP 内容响应 */
    std::string Call(const PropertyList& properties) {
        ReturnValue return_value = callback_(properties);
        // 回调返回的裸指针立即接管，后续任一分配失败仍会回收。
        std::unique_ptr<ImageContent> image_content(
            std::holds_alternative<ImageContent*>(return_value) ? std::get<ImageContent*>(return_value) : nullptr);
        McpJson json_content(std::holds_alternative<cJSON*>(return_value) ? std::get<cJSON*>(return_value) : nullptr,
                             cJSON_Delete);
        auto result = McpOwnJson(cJSON_CreateObject());
        auto content = McpOwnJson(cJSON_CreateArray());
        auto item = McpOwnJson(cJSON_CreateObject());
        if (std::holds_alternative<ImageContent*>(return_value)) {
            if (!image_content) {
                throw std::runtime_error("Empty tool image result");
            }
            McpRequireJson(cJSON_AddStringToObject(item.get(), "type", "image"));
            McpRequireJson(cJSON_AddStringToObject(item.get(), "image", image_content->to_json().c_str()));
        } else {
            McpRequireJson(cJSON_AddStringToObject(item.get(), "type", "text"));
            if (std::holds_alternative<std::string>(return_value)) {
                McpRequireJson(
                    cJSON_AddStringToObject(item.get(), "text", std::get<std::string>(return_value).c_str()));
            } else if (std::holds_alternative<bool>(return_value)) {
                McpRequireJson(
                    cJSON_AddStringToObject(item.get(), "text", std::get<bool>(return_value) ? "true" : "false"));
            } else if (std::holds_alternative<int>(return_value)) {
                McpRequireJson(
                    cJSON_AddStringToObject(item.get(), "text", std::to_string(std::get<int>(return_value)).c_str()));
            } else if (std::holds_alternative<cJSON*>(return_value)) {
                if (!json_content) {
                    throw std::runtime_error("Empty tool JSON result");
                }
                McpRequireJson(cJSON_AddStringToObject(item.get(), "text", McpPrintJson(json_content.get()).c_str()));
            }
        }
        McpAttachArray(content.get(), std::move(item));
        McpAttachObject(result.get(), "content", std::move(content));
        McpRequireJson(cJSON_AddBoolToObject(result.get(), "isError", false));
        return McpPrintJson(result.get());
    }
};

struct McpServerCallbacks {
    std::function<void(const std::string&)> send;                                  // 向协议会话交付响应
    std::function<bool(std::function<void()>)> schedule;                           // 通过系统工作队列执行工具
    std::function<void(const std::string&, const std::string&)> configure_camera;  // 交付视觉能力配置
};

class McpServer {
public:
    /** @brief 获取系统唯一的 MCP 工具分发器 */
    static McpServer& GetInstance() {
        static McpServer instance;
        return instance;
    }

    /** @brief 在协议接收前绑定传输、调度和能力配置回调 */
    void Configure(const McpServerCallbacks& callbacks);

    /** @brief 启动协议前接管并注册工具；重复名称的对象会被释放 */
    void AddTool(McpTool* tool);

    /** @brief 根据名称、参数约束和执行回调注册工具 */
    void AddTool(const std::string& name, const std::string& description, const PropertyList& properties,
                 std::function<ReturnValue(const PropertyList&)> callback);

    /** @brief 注册仅向用户调用方公开的工具 */
    void AddUserOnlyTool(const std::string& name, const std::string& description, const PropertyList& properties,
                         std::function<ReturnValue(const PropertyList&)> callback);

    /** @brief 分发已解析请求；借用节点仅在调用期间有效 */
    void ParseMessage(const cJSON* json);

    /** @brief 解析并分发文本请求，异常输入不会交付给工具 */
    void ParseMessage(const std::string& message);

private:
    std::atomic<uint32_t> malformed_messages_{0};  // 异常消息日志限频计数
    void LogMalformed(const char* stage, size_t bytes = 0);
    McpServerCallbacks callbacks_;  // 系统初始化时绑定，运行期保持不变
    McpServer();
    ~McpServer();

    void ParseCapabilities(const cJSON* capabilities);

    void ReplyResult(int id, const std::string& result);
    void ReplyError(int id, const std::string& message);

    void GetToolsList(int id, const std::string& cursor, bool list_user_only_tools);
    void DoToolCall(int id, const std::string& tool_name, const cJSON* tool_arguments);

    std::vector<McpTool*> tools_;
};

#endif  // MCP_SERVER_H
