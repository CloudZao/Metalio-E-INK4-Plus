#include "app_api.h"
#include "calculator_engine.h"

static const xz_app_api_t* api;
static Calculator calculator;
static void* result_label;
static void* expression_label;
static void* message_label;
static int chinese;
static int styled;

static void ApplyStyle(void* widget, unsigned size, unsigned align, unsigned radius, unsigned border, unsigned primary) {
    if (!styled || !widget) return;
    const xz_app_style_t style = {(uint16_t)size, (uint8_t)align, (uint8_t)radius, (uint8_t)border, (uint8_t)primary};
    api->set_style(widget, &style);
}
static void Show(void) {
    const char* hint = chinese ? "12 位有效数字 · 顺序计算" : "12-digit precision · Sequential";
    const char* result = calculator.input;
    if (calculator.error) {
        result = chinese ? "错误" : "Error";
        hint = calculator.error == CALC_DIV_ZERO ?
            (chinese ? "不能除以零，请重新输入" : "Cannot divide by zero. Enter a new number.") :
            (chinese ? "超出计算范围，请重新输入" : "Out of range. Enter a new number.");
    } else if (calculator.input_limit) {
        hint = chinese ? "最多输入 12 位数字" : "Up to 12 input digits";
    }
    ApplyStyle(result_label, calculator.error ? 30 : 36, XZ_APP_ALIGN_RIGHT, 0, 0, 0);
    api->set_text(result_label, result);
    api->set_text(expression_label, calculator.expression);
    api->set_text(message_label, hint);
}
static void Click(void* context) {
    CalcInput(&calculator, (char)(uintptr_t)context);
    Show();
}
int app_main(int argc, char** argv) {
    if (argc != 1 || !argv || !argv[0]) return -1;
    api = (const xz_app_api_t*)argv[0];
    if (api->abi_version != XZ_APP_ABI_VERSION || api->struct_size < offsetof(xz_app_api_t, asset_context) ||
        !api->label || !api->button || !api->set_text || api->width < 600 || api->height < 900) return -1;
    styled = api->struct_size >= offsetof(xz_app_api_t, set_style) + sizeof(api->set_style) && api->set_style;
    chinese = api->language && api->language[0] == 'z';
    CalcReset(&calculator);

    const int pad = 24, gap = 12, width = api->width - pad * 2;
    void* title = api->label(api->root, pad, 18, width - 200, 48, chinese ? "计算器" : "Calculator");
    ApplyStyle(title, 30, XZ_APP_ALIGN_LEFT, 0, 0, 0);
    void* mode = api->label(api->root, api->width - pad - 180, 22, 180, 40, chinese ? "标准" : "Standard");
    ApplyStyle(mode, 25, XZ_APP_ALIGN_RIGHT, 0, 0, 0);
    void* display = api->label(api->root, pad, 94, width, 232, "");
    ApplyStyle(display, 30, XZ_APP_ALIGN_RIGHT, 16, 1, 0);
    expression_label = api->label(api->root, pad + 20, 114, width - 40, 72, "");
    ApplyStyle(expression_label, 25, XZ_APP_ALIGN_RIGHT, 0, 0, 0);
    result_label = api->label(api->root, pad + 20, 218, width - 40, 72, "0");
    ApplyStyle(result_label, 36, XZ_APP_ALIGN_RIGHT, 0, 0, 0);

    if (!title || !mode || !display || !expression_label || !result_label) return -1;
    const int top = 366, key_width = (width - gap * 3) / 4;
    const int key_height = (api->height - top - 48 - gap * 4) / 5;
    static const char keys[] = {'C','N','%','/','7','8','9','*','4','5','6','-','1','2','3','+','<','0','.','='};
    static const char* const labels[] = {"AC","±","%","÷","7","8","9","×","4","5","6","−","1","2","3","+","","0",".","="};
    for (unsigned i = 0; i < 20; ++i) {
        const char key = keys[i];
        const int function = i < 4 || i % 4 == 3 || key == '<';
        const char* label = key == '<' ? (chinese ? "退格" : "Back") : labels[i];
        void* button = api->button(api->root, pad + (int)(i % 4) * (key_width + gap), top + (int)(i / 4) * (key_height + gap),
            key_width, key_height, label, Click, (void*)(uintptr_t)key);
        if (!button) return -1;
        ApplyStyle(button, key == '<' || key == '%' ? 30 : 36, XZ_APP_ALIGN_CENTER, 14, function ? 2 : 1, key == '=');
    }
    message_label = api->label(api->root, pad, api->height - 34, width, 32, "");
    ApplyStyle(message_label, 25, XZ_APP_ALIGN_CENTER, 0, 0, 0);
    if (!message_label) return -1;
    Show();
    return 0;
}
