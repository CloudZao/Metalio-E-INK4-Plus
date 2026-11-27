#include <stddef.h>
#include <stdint.h>
#include "app_api.h"

#define TEST_COUNT 17 // 单次自动测试项数

/** @brief 提供独立 ELF 的本地内存复制。 */
void* memcpy(void* destination, const void* source, size_t length) {
    uint8_t* out = destination;
    const uint8_t* in = source;
    for (size_t index = 0; index < length; ++index) {
        out[index] = in[index];
    }
    return destination;
}

/** @brief 提供独立 ELF 的本地内存初始化。 */
void* memset(void* destination, int value, size_t length) {
    uint8_t* out = destination;
    for (size_t index = 0; index < length; ++index) {
        out[index] = (uint8_t)value;
    }
    return destination;
}

typedef struct {
    const char* name; // 中文测试名称
    const char* name_en; // 英文测试名称
    uint32_t protocol; // xz_app_net_protocol_t
    unsigned length; // 预期回显长度
} network_test_case_t;

typedef struct {
    int state; // 0 未运行，1 运行中，2 通过，3 失败
    int error; // 宿主结果码
    int code; // 底层数值错误码
    uint32_t elapsed; // 单调毫秒耗时
    uint32_t bytes; // 已校验字节数
    xz_app_net_diagnostics_t diagnostics; // DNS 地址或 Ping 统计
} network_test_result_t;

static const network_test_case_t cases[TEST_COUNT] = {
    {"HTTP GET", "HTTP GET", XZ_APP_NET_HTTP, sizeof("service-lab-network-test") - 1},
    {"HTTP POST", "HTTP POST", XZ_APP_NET_HTTP, 128},
    {"HTTP 404", "HTTP 404", XZ_APP_NET_HTTP, sizeof("service-lab-network-test") - 1},
    {"HTTP 分块", "HTTP chunked", XZ_APP_NET_HTTP, 9000},
    {"HTTP 超时", "HTTP timeout", XZ_APP_NET_HTTP, 0},
    {"HTTPS", "HTTPS", XZ_APP_NET_HTTP, sizeof("service-lab-network-test") - 1},
    {"WS 文本", "WS text", XZ_APP_NET_WEBSOCKET, 18},
    {"WS 二进制", "WS binary", XZ_APP_NET_WEBSOCKET, 1024},
    {"WS 空消息", "WS empty", XZ_APP_NET_WEBSOCKET, 0},
    {"WSS", "WSS", XZ_APP_NET_WEBSOCKET, 1024},
    {"TCP", "TCP", XZ_APP_NET_TCP, 3072},
    {"TLS", "TLS", XZ_APP_NET_TLS, 3072},
    {"UDP", "UDP", XZ_APP_NET_UDP, 128},
    {"TLS 拒绝证书", "TLS reject CA", XZ_APP_NET_TLS, 0},
    {"HTTP 取消", "HTTP cancel", XZ_APP_NET_HTTP, 0},
    {"DNS", "DNS", 0, 0},
    {"Ping", "Ping", 0, 0}
};

static const xz_app_api_t* api;
static int zh;
static void* host_input;
static void* port_input;
static void* status_label;
static void* results_label;
static void* previous_button;
static void* next_button;
static void* idle_timer_button;
static int idle_timer_enabled;
static unsigned result_page;
static void* run_buttons[9];
static char host[254];
static unsigned http_port = 8080;
static unsigned ports[4] = {9081, 9082, 9443, 9444};
static char ca_pem[8193];
static char profile[256];
static unsigned profile_length;
static unsigned ca_length;
static int prepared;
static int setup; // 1 读取端口，2 读取测试证书，3 进入应用准备网络
static int current = -1;
static int sequence;
static int last_case;
static int continuous;
static int waiting_next;
static uint32_t handle;
static uint64_t started;
static uint64_t next_start;
static unsigned received;
static int sent;
static int valid;
static unsigned loops;
static uint8_t buffer[4096];
static char address[512];
static char output[2048];
static unsigned output_length;
static network_test_result_t results[TEST_COUNT];

static void TextReset(void) {
    output_length = 0;
    output[0] = 0;
}

static void TextAdd(const char* value) {
    while (*value && output_length + 1 < sizeof(output)) {
        output[output_length++] = *value++;
    }
    output[output_length] = 0;
}

static void TextNumber(uint32_t value) {
    char digits[11];
    unsigned count = 0;
    do {
        digits[count++] = (char)('0' + value % 10);
        value /= 10;
    } while (value);
    while (count) {
        char part[2] = {digits[--count], 0};
        TextAdd(part);
    }
}

static void TextSigned(int value) {
    if (value < 0) {
        TextAdd("-");
        TextNumber(0u - (uint32_t)value);
    } else {
        TextNumber(value);
    }
}

static void SetStatus(const char* chinese, const char* english) {
    api->set_text(status_label, zh ? chinese : english);
}

static void RefreshResults(void) {
    TextReset();
    const unsigned end = result_page * 3 + 3 < TEST_COUNT ? result_page * 3 + 3 : TEST_COUNT;
    for (unsigned index = result_page * 3; index < end; ++index) {
        const network_test_result_t* result = &results[index];
        if (!result->state) {
            continue;
        }
        TextAdd(zh ? cases[index].name : cases[index].name_en);
        TextAdd(": ");
        TextAdd(result->state == 1 ? (zh ? "运行中" : "Running") :
                result->state == 2 ? (zh ? "通过" : "PASS") : (zh ? "失败" : "FAIL"));
        if (result->state > 1) {
            TextAdd("\n");
            if (index >= 15) {
                TextAdd(result->diagnostics.address);
                if (index == 16) {
                    TextAdd(" ");
                    TextNumber(result->diagnostics.received);
                    TextAdd("/");
                    TextNumber(result->diagnostics.sent);
                    TextAdd(" ");
                    TextNumber(result->diagnostics.average_rtt_ms);
                    TextAdd(" ms");
                }
                TextAdd("\n");
            } else {
                TextNumber(result->bytes);
                TextAdd(" B / ");
            }
            TextNumber(result->elapsed);
            TextAdd(" ms");
            if (result->error) {
                TextAdd("\n");
                TextSigned(result->error);
                TextAdd(" (");
                TextSigned(result->code);
                TextAdd(")");
            }
        }
        TextAdd("\n");
    }
    if (loops) {
        TextAdd(zh ? "持续 WS 已校验：" : "Continuous WS verified: ");
        TextNumber(loops);
        TextAdd("\n");
    }
    api->set_text(results_label, output);
    api->set_enabled(previous_button, result_page > 0);
    api->set_enabled(next_button, (result_page + 1) * 3 < TEST_COUNT);
}

static int ParsePort(const char* value, unsigned* result) {
    unsigned number = 0;
    if (!*value) {
        return 0;
    }
    while (*value) {
        if (*value < '0' || *value > '9' || number > 6553) {
            return 0;
        }
        number = number * 10 + (unsigned)(*value++ - '0');
    }
    if (!number || number > 65535) {
        return 0;
    }
    *result = number;
    return 1;
}

static int ReadHost(void) {
    if (api->get_text(host_input, host, sizeof(host)) <= 0) {
        return 0;
    }
    for (const char* p = host; *p; ++p) {
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
              (*p >= '0' && *p <= '9') || *p == '.' || *p == '-' || *p == '_')) {
            return 0;
        }
    }
    return 1;
}

static int ReadAddress(void) {
    char port[8];
    return ReadHost() && api->get_text(port_input, port, sizeof(port)) > 0 && ParsePort(port, &http_port);
}

static const char* MakeURL(const char* scheme, unsigned port, const char* path) {
    TextReset();
    TextAdd(scheme);
    TextAdd("://");
    TextAdd(host);
    TextAdd(":");
    TextNumber(port);
    TextAdd(path);
    if (output_length >= sizeof(address)) {
        return 0;
    }
    memcpy(address, output, output_length + 1);
    return address;
}

static void CloseConnection(void) {
    if (handle) {
        api->network_close(api->host_context, handle);
        handle = 0;
    }
}

static int StartHTTP(const char* path) {
    xz_app_net_config_t config = {0};
    config.struct_size = sizeof(config);
    config.protocol = XZ_APP_NET_HTTP;
    config.receive_buffer_size = 4096;
    config.response_size_limit = sizeof(ca_pem) - 1;
    config.address = MakeURL("http", http_port, path);
    config.method = "GET";
    config.timeout_ms = 10000;
    started = api->monotonic_ms();
    return api->network_open(api->host_context, &config, &handle);
}

static void UpdateButtons(void) {
    const int idle = !handle && !waiting_next;
    for (unsigned index = 0; index < 9; ++index) {
        api->set_enabled(run_buttons[index], idle && (index == 0 || index >= 7 || prepared));
    }
}

static uint8_t ExpectedByte(unsigned index) {
    if (current == 0 || current == 2 || current == 5) {
        return (uint8_t)"service-lab-network-test"[index];
    }
    if (current == 6) {
        return (uint8_t)"ELF websocket echo"[index];
    }
    return (uint8_t)(index * 37u + 11u);
}

static void FinishTest(int passed, int error, int code) {
    network_test_result_t* result = &results[current];
    result->state = passed ? 2 : 3;
    result->error = error;
    result->code = code;
    result->bytes = received;
    result->elapsed = (uint32_t)(api->monotonic_ms() - started);
    CloseConnection();
    RefreshResults();
    if (sequence && current < last_case) {
        ++current;
        waiting_next = 1;
        next_start = api->monotonic_ms() + 300;
    } else {
        sequence = 0;
        continuous = 0;
        current = -1;
        SetStatus("测试已结束，可查看各项结果", "Tests finished. Review individual results.");
    }
    UpdateButtons();
}

static void StartCase(void) {
    xz_app_net_config_t config = {0};
    config.struct_size = sizeof(config);
    config.protocol = cases[current].protocol;
    config.receive_buffer_size = 8192;
    config.send_buffer_size = cases[current].length;
    config.response_size_limit = config.protocol == XZ_APP_NET_HTTP ? 1024u * 1024u : 0;
    config.timeout_ms = current == 4 ? 1000 : 10000;
    config.address = host;
    const char* path = current == 2 ? "/network-test/http?status=404" :
        current == 3 ? "/network-test/http?size=9000&chunked=1" :
        current == 4 ? "/network-test/http?delay=3000" :
        current == 14 ? "/network-test/http?delay=5000" : "/network-test/http";
    if (config.protocol == XZ_APP_NET_HTTP) {
        config.address = MakeURL(current == 5 ? "https" : "http", current == 5 ? ports[3] : http_port, path);
        config.method = current == 1 ? "POST" : "GET";
        if (current == 1) {
            for (unsigned index = 0; index < cases[current].length; ++index) {
                buffer[index] = ExpectedByte(index);
            }
            config.body = buffer;
            config.body_length = cases[current].length;
        }
    } else if (config.protocol == XZ_APP_NET_WEBSOCKET) {
        config.address = MakeURL(current == 9 ? "wss" : "ws", current == 9 ? ports[3] : http_port, "/network-test/ws");
    } else {
        config.port = config.protocol == XZ_APP_NET_TCP ? ports[0] : config.protocol == XZ_APP_NET_UDP ? ports[1] : ports[2];
    }
    if (current == 5 || current == 9 || current == 11) {
        config.ca_pem = ca_pem;
    }
    received = 0;
    sent = 0;
    valid = 1;
    result_page = (unsigned)current / 3;
    started = api->monotonic_ms();
    results[current] = (network_test_result_t){1, 0, 0, 0, 0, {0}};
    int result;
    if (current == 15) {
        result = api->network_resolve(api->host_context, host, 10000, &handle);
    } else if (current == 16) {
        xz_app_ping_config_t ping = {0};
        ping.struct_size = sizeof(ping);
        ping.address = host;
        ping.network_timeout_ms = 10000;
        ping.count = 4;
        ping.interval_ms = 250;
        ping.timeout_ms = 1000;
        ping.data_size = 32;
        ping.ttl = 64;
        result = api->network_ping(api->host_context, &ping, &handle);
    } else {
        result = api->network_open(api->host_context, &config, &handle);
    }
    if (result != XZ_APP_OK) {
        FinishTest(0, result, 0);
    } else {
        RefreshResults();
        UpdateButtons();
    }
}

static int ParseProfile(void) {
    static const char* keys[4] = {"tcp=", "udp=", "tls=", "https="};
    char* cursor = profile;
    unsigned parsed[4];
    for (unsigned index = 0; index < 4; ++index) {
        const char* key = keys[index];
        while (*key) {
            if (*cursor++ != *key++) {
                return 0;
            }
        }
        char* value = cursor;
        while (*cursor && *cursor != '\n') {
            ++cursor;
        }
        if (*cursor != '\n') {
            return 0;
        }
        *cursor++ = 0;
        if (!ParsePort(value, &parsed[index])) {
            return 0;
        }
    }
    memcpy(ports, parsed, sizeof(ports));
    return *cursor == 0;
}

static void FailSetup(void) {
    CloseConnection();
    setup = 0;
    prepared = 0;
    ca_pem[0] = 0;
    SetStatus("连接失败，请检查服务、地址和端口", "Cannot prepare. Check service, host and port.");
    UpdateButtons();
}

static void PollSetup(const xz_app_net_status_t* status) {
    if (setup == 3) {
        if (status->state == XZ_APP_NET_COMPLETE) {
            CloseConnection();
            setup = 0;
            SetStatus("网络已就绪，请填写主机和 HTTP 端口", "Network ready. Enter host and HTTP port.");
            UpdateButtons();
        } else if (status->state == XZ_APP_NET_ERROR || status->state == XZ_APP_NET_CLOSED) {
            FailSetup();
        }
        return;
    }
    uint32_t flags;
    while (1) {
        const int length = api->network_read(api->host_context, handle, buffer, sizeof(buffer), &flags);
        if (length == XZ_APP_ERR_NOT_READY) {
            break;
        }
        if (length < 0) {
            FailSetup();
            return;
        }
        char* destination = setup == 1 ? profile : ca_pem;
        unsigned* used = setup == 1 ? &profile_length : &ca_length;
        const unsigned capacity = setup == 1 ? sizeof(profile) : sizeof(ca_pem);
        if ((unsigned)length >= capacity - *used) {
            FailSetup();
            return;
        }
        memcpy(destination + *used, buffer, (unsigned)length);
        *used += (unsigned)length;
        destination[*used] = 0;
    }
    if (status->state == XZ_APP_NET_COMPLETE) {
        CloseConnection();
        if (status->http_status != 200) {
            FailSetup();
        } else if (setup == 1) {
            if (!ParseProfile()) {
                FailSetup();
                return;
            }
            setup = 2;
            if (StartHTTP("/network-test/ca.pem") != XZ_APP_OK) {
                FailSetup();
            }
        } else {
            setup = 0;
            prepared = ca_length > 64;
            SetStatus(prepared ? "服务已就绪，测试证书仅供本应用使用" : "证书无效，请重试",
                      prepared ? "Ready. Test CA is scoped to this app." : "Invalid certificate. Retry preparation.");
            UpdateButtons();
        }
    } else if (status->state == XZ_APP_NET_ERROR || status->state == XZ_APP_NET_CLOSED) {
        FailSetup();
    }
}

static void Poll(void* context) {
    (void)context;
    if (waiting_next && api->monotonic_ms() >= next_start) {
        waiting_next = 0;
        StartCase();
    }
    if (!handle) {
        return;
    }
    xz_app_net_status_t status = {0};
    status.struct_size = sizeof(status);
    const int result = api->network_get_status(api->host_context, handle, &status);
    if (result != XZ_APP_OK) {
        if (setup) {
            FailSetup();
        } else {
            sequence = 0;
            continuous = 0;
            FinishTest(0, result, 0);
            prepared = 0;
            SetStatus("连接已撤销，请重新读取服务后测试", "Connection revoked. Prepare the service again.");
            UpdateButtons();
        }
        return;
    }
    if (setup) {
        PollSetup(&status);
        return;
    }
    if (current >= 15) {
        xz_app_net_diagnostics_t* diagnostics = &results[current].diagnostics;
        diagnostics->struct_size = sizeof(*diagnostics);
        const int error = api->network_get_diagnostics(api->host_context, handle, diagnostics);
        if (error != XZ_APP_OK) {
            FinishTest(0, error, 0);
        } else if (status.state == XZ_APP_NET_COMPLETE) {
            const int passed = diagnostics->address[0] &&
                (current == 15 || (diagnostics->sent == 4 && diagnostics->received == 4));
            FinishTest(passed, 0, 0);
        } else if (status.state == XZ_APP_NET_ERROR || status.state == XZ_APP_NET_CLOSED) {
            FinishTest(0, status.result ? status.result : XZ_APP_ERR_IO, status.error_code);
        }
        return;
    }
    if (current == 14 && api->monotonic_ms() - started >= 400) {
        const uint32_t old_handle = handle;
        CloseConnection();
        status.struct_size = sizeof(status);
        const int revoked = api->network_get_status(api->host_context, old_handle, &status);
        FinishTest(revoked == XZ_APP_ERR_NOT_FOUND, revoked == XZ_APP_ERR_NOT_FOUND ? 0 : revoked, 0);
        return;
    }
    if (!sent && status.state == XZ_APP_NET_CONNECTED && cases[current].protocol != XZ_APP_NET_HTTP) {
        for (unsigned index = 0; index < cases[current].length; ++index) {
            buffer[index] = ExpectedByte(index);
        }
        const int error = api->network_send(api->host_context, handle, buffer, cases[current].length,
                                            current == 6 || current == 8 ? 0 : XZ_APP_NET_BINARY);
        if (error != XZ_APP_OK) {
            FinishTest(0, error, 0);
            return;
        }
        sent = 1;
    }
    int message_received = 0;
    for (unsigned count = 0; count < 32; ++count) {
        uint32_t flags = 0;
        const int length = api->network_read(api->host_context, handle, buffer, sizeof(buffer), &flags);
        if (length == XZ_APP_ERR_NOT_READY) {
            break;
        }
        if (length < 0) {
            FinishTest(0, length, 0);
            return;
        }
        if (cases[current].protocol == XZ_APP_NET_WEBSOCKET &&
            ((current == 6 || current == 8) ? flags != 0 : flags != XZ_APP_NET_BINARY)) {
            valid = 0;
        }
        for (int index = 0; index < length; ++index) {
            if (received >= cases[current].length || buffer[index] != ExpectedByte(received)) {
                valid = 0;
            }
            ++received;
        }
        message_received = 1;
    }
    if (current == 4 || current == 13) {
        if (status.state == XZ_APP_NET_ERROR) {
            const int expected = current == 4 ? XZ_APP_ERR_TIMEOUT : XZ_APP_ERR_IO;
            FinishTest(status.result == expected, status.result == expected ? 0 : status.result, status.error_code);
            return;
        }
    } else if (cases[current].protocol == XZ_APP_NET_HTTP && status.state == XZ_APP_NET_COMPLETE) {
        char content_type[96];
        const int header = api->network_get_header(api->host_context, handle, "content-type", content_type, sizeof(content_type));
        const unsigned expected_status = current == 2 ? 404 : 200;
        FinishTest(valid && received == cases[current].length && status.http_status == expected_status && header > 0, 0, 0);
        return;
    } else if (cases[current].protocol != XZ_APP_NET_HTTP && message_received && received >= cases[current].length) {
        if (continuous && valid && received == cases[current].length) {
            ++loops;
            received = 0;
            sent = 0;
            started = api->monotonic_ms();
            results[current].bytes = loops * cases[current].length;
            RefreshResults();
            return;
        }
        FinishTest(valid && received == cases[current].length, 0, 0);
        return;
    }
    if (status.state == XZ_APP_NET_ERROR || status.state == XZ_APP_NET_CLOSED) {
        FinishTest(0, status.result ? status.result : XZ_APP_ERR_IO, status.error_code);
    } else if (api->monotonic_ms() - started > 20000) {
        FinishTest(0, XZ_APP_ERR_TIMEOUT, 0);
    }
}

static void Click(void* context) {
    const unsigned action = (unsigned)(uintptr_t)context;
    if (action == 13) {
        const int result = api->network_set_idle_timer(api->host_context, !idle_timer_enabled);
        if (result != XZ_APP_OK) {
            SetStatus("无法切换断网计时，请重试", "Cannot change idle timer. Retry.");
            return;
        }
        idle_timer_enabled = !idle_timer_enabled;
        api->set_text(idle_timer_button, zh ? (idle_timer_enabled ? "断网计时：开启" : "断网计时：关闭") :
                      (idle_timer_enabled ? "Idle timer: ON" : "Idle timer: OFF"));
        return;
    }
    if (action == 11 || action == 12) {
        if (action == 11 && result_page > 0) {
            --result_page;
        } else if (action == 12 && (result_page + 1) * 3 < TEST_COUNT) {
            ++result_page;
        }
        RefreshResults();
        return;
    }
    if (action == 8) {
        sequence = 0;
        continuous = 0;
        waiting_next = 0;
        setup = 0;
        if (current >= 0) {
            FinishTest(0, XZ_APP_ERR_CANCELLED, 0);
        } else {
            CloseConnection();
        }
        SetStatus("已停止，可重新测试", "Stopped. Ready to retry.");
        UpdateButtons();
        return;
    }
    if (handle || waiting_next) {
        return;
    }
    if (action == 9 || action == 10) {
        api->keyboard(api->host_context, api->root, action == 9 ? host_input : port_input);
        prepared = 0;
        UpdateButtons();
        return;
    }
    api->keyboard_close(api->host_context);
    if (action == 14 || action == 15) {
        if (!ReadHost()) {
            SetStatus("请输入有效主机或 IP", "Enter a valid host or IP.");
            return;
        }
        sequence = 0;
        continuous = 0;
        current = action == 14 ? 15 : 16;
        SetStatus("正在探测，可随时停止或退出", "Probing. Stop or leave at any time.");
        StartCase();
    } else if (action == 0) {
        if (!ReadAddress()) {
            SetStatus("请输入有效主机和端口", "Enter a valid host and port.");
            return;
        }
        prepared = 0;
        ca_length = 0;
        profile_length = 0;
        ca_pem[0] = 0;
        profile[0] = 0;
        setup = 1;
        SetStatus("正在读取服务和测试证书", "Reading services and test certificate…");
        if (StartHTTP("/network-test/profile") != XZ_APP_OK) {
            FailSetup();
        }
    } else if (prepared) {
        sequence = action != 4 && action != 6;
        last_case = action == 1 ? 5 : action == 2 ? 9 : action == 3 ? 11 : action == 4 ? 12 : TEST_COUNT - 1;
        continuous = action == 6;
        loops = 0;
        current = action == 1 ? 0 : action == 2 ? 6 : action == 3 ? 10 : action == 4 ? 12 : action == 6 ? 7 : 0;
        if (action == 5) {
            memset(results, 0, sizeof(results));
        }
        SetStatus("正在测试，可随时停止或退出", "Testing. Stop or leave at any time.");
        StartCase();
    }
    UpdateButtons();
}

static void Exit(void* context) {
    (void)context;
    sequence = 0;
    continuous = 0;
    CloseConnection();
}

static void* Label(void* parent, int x, int y, int width, int height, const char* value) {
    void* widget = api->label(parent, x, y, width, height, value);
    const xz_app_style_t style = {25, XZ_APP_ALIGN_LEFT, 0, 0, 0};
    if (widget) {
        api->set_style(widget, &style);
    }
    return widget;
}

static void* Button(int x, int y, int width, const char* chinese, const char* english, unsigned action) {
    void* widget = api->button(api->root, x, y, width, 64, zh ? chinese : english, Click, (void*)(uintptr_t)action);
    const xz_app_style_t style = {25, XZ_APP_ALIGN_CENTER, 10, 1, 0};
    if (widget) {
        api->set_style(widget, &style);
    }
    return widget;
}

int app_main(int argc, char** argv) {
    if (argc != 1 || !argv || !argv[0]) {
        return XZ_APP_ERR_INVALID;
    }
    api = (const xz_app_api_t*)argv[0];
    if (api->abi_version != XZ_APP_ABI_VERSION || !XZ_APP_HAS_FIELD(api, network_get_diagnostics) ||
        !(api->capabilities & XZ_APP_CAP_NETWORK) || !api->network_open || !api->network_send ||
        !(api->capabilities & XZ_APP_CAP_DNS) || !(api->capabilities & XZ_APP_CAP_PING) ||
        !api->network_get_status || !api->network_read || !api->network_get_header || !api->network_close ||
        !api->network_set_idle_timer || !api->network_prepare || !api->network_resolve || !api->network_ping ||
        !api->network_get_diagnostics ||
        !api->label || !api->button || !api->set_text || !api->set_style || !api->set_enabled ||
        !api->input || !api->get_text || !api->keyboard || !api->keyboard_close ||
        !api->monotonic_ms || !api->timer_start || !api->set_on_exit || api->width < 600 || api->height < 1000) {
        return XZ_APP_ERR_UNSUPPORTED;
    }
    zh = api->language && api->language[0] == 'z';
    const int width = api->width - 48;
    const int column = (width - 24) / 3;
    if (!Label(api->root, 24, 16, width, 48, zh ? "网络测试" : "Network tests")) {
        return XZ_APP_ERR_MEMORY;
    }
    host_input = api->input(api->root, 24, 76, width - 150, 58, "", 253, 0, 0);
    port_input = api->input(api->root, 24 + width - 134, 76, 134, 58, "8080", 5, 0, 0);
    if (!host_input || !port_input || !Button(24, 148, width - 150, "填写主机 / IP", "Enter host / IP", 9) ||
        !Button(24 + width - 134, 148, 134, "端口", "Port", 10)) {
        return XZ_APP_ERR_MEMORY;
    }
    const int probe_width = (width - 24) / 4;
    const int prepare_width = width - 24 - 2 * probe_width;
    run_buttons[0] = Button(24, 226, prepare_width, "读取服务与证书", "Prepare services", 0);
    run_buttons[7] = Button(36 + prepare_width, 226, probe_width, "DNS", "DNS", 14);
    run_buttons[8] = Button(48 + prepare_width + probe_width, 226, probe_width, "Ping", "Ping", 15);
    run_buttons[1] = Button(24, 304, column, "HTTP", "HTTP", 1);
    run_buttons[2] = Button(36 + column, 304, column, "WebSocket", "WebSocket", 2);
    run_buttons[3] = Button(48 + column * 2, 304, column, "TCP", "TCP", 3);
    run_buttons[4] = Button(24, 382, column, "UDP", "UDP", 4);
    run_buttons[5] = Button(36 + column, 382, column, "全部测试", "Run all", 5);
    run_buttons[6] = Button(48 + column * 2, 382, column, "持续 WS", "Repeat WS", 6);
    for (unsigned index = 0; index < 9; ++index) {
        if (!run_buttons[index]) {
            return XZ_APP_ERR_MEMORY;
        }
    }
    idle_timer_button = Button(36 + (width - 12) / 2, 460, (width - 12) / 2,
                               "断网计时：关闭", "Idle timer: OFF", 13);
    if (!Button(24, 460, (width - 12) / 2, "停止", "Stop", 8) || !idle_timer_button) {
        return XZ_APP_ERR_MEMORY;
    }
    status_label = Label(api->root, 24, 538, width, 90,
                         zh ? "填写 Service Lab 的主机和 HTTP 端口" : "Enter the Service Lab host and HTTP port.");
    results_label = Label(api->root, 24, 636, width, api->height - 720, "");
    previous_button = Button(24, api->height - 76, (width - 12) / 2, "上一页", "Previous", 11);
    next_button = Button(36 + (width - 12) / 2, api->height - 76, (width - 12) / 2, "下一页", "Next", 12);
    if (!status_label || !results_label || !previous_button || !next_button || api->set_on_exit(api->host_context, Exit, 0) != XZ_APP_OK ||
        !api->timer_start(api->host_context, 1000, 1, Poll, 0)) {
        return XZ_APP_ERR_MEMORY;
    }
    setup = 3;
    const int result = api->network_prepare(api->host_context, 10000, &handle);
    if (result == XZ_APP_OK) {
        SetStatus("正在连接网络，请稍候", "Connecting to network. Please wait.");
    } else {
        FailSetup();
    }
    UpdateButtons();
    RefreshResults();
    return XZ_APP_OK;
}
