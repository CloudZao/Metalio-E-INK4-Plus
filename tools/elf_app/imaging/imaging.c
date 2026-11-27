#include "app_api.h"
#include "image_assets.h"

enum {
    ADJUST_CONTRAST,
    ADJUST_BRIGHTNESS,
    ADJUST_EXPOSURE,
    ADJUST_SHADOWS,
    ADJUST_HIGHLIGHTS,
    ADJUST_GAMMA,
    ADJUST_SHARPNESS,
    ADJUST_ROTATION,
    ADJUST_CROP,
    ADJUST_MIRROR_X,
    ADJUST_MIRROR_Y,
    ADJUST_INVERT,
    ADJUST_COUNT
};

static const xz_app_api_t* api;
static void* picture;
static void* status_label;
static void* adjustment_label;
static void* pause_button;
static void* sources[5];
static void* modes[4];
static int zh;
static unsigned source;
static xz_app_image_effects_t effects;
static unsigned crop_percent = 100;
static unsigned adjustment;
static unsigned default_mode;
static int paused;
static int last_result;
static int last_running = -1;
static char text[128];

static void Style(void* object, int primary) {
    const xz_app_style_t style = {25, XZ_APP_ALIGN_CENTER, 8, 1, (uint8_t)primary};
    if (object && api->set_style) {
        api->set_style(object, &style);
    }
}

static void NumberText(const char* prefix, int value, const char* suffix, unsigned decimals) {
    unsigned length = 0;
    while (*prefix && length < sizeof(text) - 1) {
        text[length++] = *prefix++;
    }
    if (value < 0) {
        text[length++] = '-';
        value = -value;
    }
    char digits[12];
    unsigned count = 0;
    const unsigned fraction = value % 100;
    if (decimals == 2) {
        value /= 100;
    }
    do {
        digits[count++] = '0' + value % 10;
        value /= 10;
    } while (value && count < sizeof(digits));
    while (count && length < sizeof(text) - 1) {
        text[length++] = digits[--count];
    }
    if (decimals == 2 && length + 3 < sizeof(text)) {
        text[length++] = '.';
        text[length++] = '0' + fraction / 10;
        text[length++] = '0' + fraction % 10;
    }
    while (*suffix && length < sizeof(text) - 1) {
        text[length++] = *suffix++;
    }
    text[length] = 0;
}

static void ShowResult(int result) {
    if (result == XZ_APP_OK) {
        last_result = result;
        api->set_text(status_label, source == 0 ? "" : (zh ? "测试图片已就绪" : "Test image ready"));
    } else {
        NumberText(zh ? "操作未完成，错误码 " : "Operation failed, code ", result, "", 0);
        api->set_text(status_label, text);
        last_result = result;
    }
}

static void SelectSource(void* context) {
    source = (unsigned)(uintptr_t)context;
    paused = 0;
    last_running = -1;
    api->camera_preview(api->host_context, picture, 0);
    int result;
    if (source == 0) {
        result = api->camera_preview(api->host_context, picture, 1);
    } else {
        result = api->image_load_asset(api->host_context, picture,
                                      image_assets[source - 1].offset, image_assets[source - 1].length);
    }
    for (unsigned i = 0; i < 5; ++i) {
        Style(sources[i], i == source);
    }
    api->set_text(pause_button, source == 0 ? (zh ? "暂停" : "Pause") : (zh ? "重载" : "Reload"));
    ShowResult(result);
}

static int32_t* ToneParameter(void) {
    switch (adjustment) {
        case ADJUST_CONTRAST:
            return &effects.contrast;
        case ADJUST_BRIGHTNESS:
            return &effects.brightness;
        case ADJUST_EXPOSURE:
            return &effects.exposure;
        case ADJUST_SHADOWS:
            return &effects.shadows;
        case ADJUST_HIGHLIGHTS:
            return &effects.highlights;
        case ADJUST_GAMMA:
            return &effects.gamma;
        case ADJUST_SHARPNESS:
            return &effects.sharpness;
        default:
            return 0;
    }
}

static void UpdateControls(void) {
    static const char* const cn_adjustments[] = {"对比度 ", "亮度 ", "软件曝光 ", "阴影 ", "高光 ", "Gamma ",
                                                 "清晰度 ", "旋转 ", "中心裁剪 ", "水平镜像 ", "垂直镜像 ", "反相 "};
    static const char* const en_adjustments[] = {"Contrast ", "Brightness ", "Exposure ", "Shadows ", "Highlights ", "Gamma ",
                                                 "Detail ", "Rotation ", "Crop ", "Mirror X ", "Mirror Y ", "Invert "};
    const int32_t* parameter = ToneParameter();
    int value = 0;
    const char* suffix = "%";
    unsigned decimals = 0;
    if (parameter) {
        value = *parameter;
    }
    switch (adjustment) {
        case ADJUST_EXPOSURE:
            suffix = " EV";
            decimals = 2;
            break;
        case ADJUST_GAMMA:
            suffix = "";
            decimals = 2;
            break;
        case ADJUST_ROTATION:
            value = effects.rotation;
            suffix = zh ? " 度" : " deg";
            break;
        case ADJUST_CROP:
            value = crop_percent;
            break;
        case ADJUST_MIRROR_X:
            value = effects.mirror_x;
            break;
        case ADJUST_MIRROR_Y:
            value = effects.mirror_y;
            break;
        case ADJUST_INVERT:
            value = effects.invert;
            break;
    }
    NumberText(zh ? cn_adjustments[adjustment] : en_adjustments[adjustment], value, suffix, decimals);
    if (adjustment >= ADJUST_MIRROR_X) {
        static const char* const cn_flags[3][2] = {{"水平镜像：关", "水平镜像：开"},
                                                   {"垂直镜像：关", "垂直镜像：开"}, {"反相：关", "反相：开"}};
        static const char* const en_flags[3][2] = {{"Mirror X: off", "Mirror X: on"},
                                                   {"Mirror Y: off", "Mirror Y: on"}, {"Invert: off", "Invert: on"}};
        api->set_text(adjustment_label, zh ? cn_flags[adjustment - ADJUST_MIRROR_X][value] : en_flags[adjustment - ADJUST_MIRROR_X][value]);
    } else {
        api->set_text(adjustment_label, text);
    }
    for (unsigned i = 0; i < 4; ++i) {
        Style(modes[i], i == effects.mode);
    }
}

static int ApplyEffects(void) {
    const int result = api->image_set_effects(api->host_context, picture, &effects);
    if (result != XZ_APP_OK) {
        api->image_get_effects(api->host_context, picture, &effects);
        crop_percent = effects.crop_width / 100;
    }
    UpdateControls();
    return result;
}

static void SelectMode(void* context) {
    effects.mode = (unsigned)(uintptr_t)context;
    const int result = ApplyEffects();
    ShowResult(result);
}

static void AdjustValue(void* context) {
    const int increase = (uintptr_t)context != 0;
    int32_t* parameter = ToneParameter();
    if (parameter) {
        int minimum = -100;
        int maximum = 100;
        if (adjustment == ADJUST_GAMMA) {
            minimum = 25;
            maximum = 400;
        } else if (adjustment == ADJUST_EXPOSURE) {
            minimum = -200;
            maximum = 200;
        }
        const int value = *parameter + (increase ? 25 : -25);
        *parameter = value < minimum ? minimum : value > maximum ? maximum : value;
    } else if (adjustment == ADJUST_ROTATION) {
        effects.rotation = (effects.rotation + (increase ? 90 : 270)) % 360;
    } else if (adjustment == ADJUST_CROP) {
        const int value = (int)crop_percent + (increase ? 10 : -10);
        crop_percent = value < 20 ? 20 : value > 100 ? 100 : (unsigned)value;
        effects.crop_width = crop_percent * 100;
        effects.crop_height = effects.crop_width;
        effects.crop_x = (10000 - effects.crop_width) / 2;
        effects.crop_y = effects.crop_x;
    } else {
        switch (adjustment) {
            case ADJUST_MIRROR_X:
                effects.mirror_x = increase;
                break;
            case ADJUST_MIRROR_Y:
                effects.mirror_y = increase;
                break;
            case ADJUST_INVERT:
                effects.invert = increase;
                break;
        }
    }
    ShowResult(ApplyEffects());
}

static void SelectAdjustment(void* context) {
    adjustment = (adjustment + ((uintptr_t)context ? 1 : ADJUST_COUNT - 1)) % ADJUST_COUNT;
    UpdateControls();
}

static void ResetEffects(void* context) {
    (void)context;
    effects.contrast = 0;
    effects.brightness = 0;
    effects.exposure = 0;
    effects.shadows = 0;
    effects.highlights = 0;
    effects.sharpness = 0;
    effects.gamma = 100;
    effects.threshold = -1;
    effects.rotation = 0;
    effects.mirror_x = 0;
    effects.mirror_y = 0;
    effects.invert = 0;
    effects.crop_x = 0;
    effects.crop_y = 0;
    effects.crop_width = 10000;
    effects.crop_height = 10000;
    crop_percent = 100;
    effects.mode = default_mode;
    adjustment = 0;
    ShowResult(ApplyEffects());
}

static void Refresh(void* context) {
    (void)context;
    ShowResult(api->request_full_refresh(api->host_context));
}

static void Pause(void* context) {
    (void)context;
    if (source != 0) {
        SelectSource((void*)(uintptr_t)source);
        return;
    }
    const int result = api->camera_preview(api->host_context, picture, paused);
    if (result == XZ_APP_OK) {
        paused = !paused;
        api->set_text(pause_button, paused ? (zh ? "继续" : "Resume") : (zh ? "暂停" : "Pause"));
    }
    ShowResult(result);
}

static void Tick(void* context) {
    (void)context;
    xz_app_image_status_t image;
    image.struct_size = sizeof(image);
    if (api->image_get_status(api->host_context, picture, &image) != XZ_APP_OK || image.loading) {
        return;
    }
    int result = image.result;
    int changed = 0;
    if (source == 0 && !paused) {
        xz_app_camera_status_t camera = {sizeof(camera)};
        if (api->camera_get_status(api->host_context, &camera) == XZ_APP_OK) {
            if (camera.result != XZ_APP_OK) {
                result = camera.result;
            }
            changed = (int)camera.running != last_running;
            last_running = camera.running;
        }
    }
    if (result != last_result || changed) {
        ShowResult(result);
    }
}

static void* Button(int x, int y, int width, const char* title, xz_app_click_t callback, unsigned value) {
    void* button = api->button(api->root, x, y, width, 52, title, callback, (void*)(uintptr_t)value);
    Style(button, 0);
    return button;
}

int app_main(int argc, char** argv) {
    if (argc != 1 || !argv || !argv[0]) {
        return XZ_APP_ERR_INVALID;
    }
    api = (const xz_app_api_t*)argv[0];
    if (api->abi_version != XZ_APP_ABI_VERSION || !XZ_APP_HAS_FIELD(api, image_get_effects) ||
        !(api->capabilities & XZ_APP_CAP_IMAGE) || !(api->capabilities & XZ_APP_CAP_CAMERA) ||
        !(api->capabilities & XZ_APP_CAP_FULL_REFRESH) || !api->image || !api->image_load_asset ||
        !(api->capabilities & XZ_APP_CAP_IMAGE_TONE) || !api->image_get_effects ||
        !api->image_set_effects || !api->image_get_status || !api->camera_preview || !api->camera_get_status ||
        !api->request_full_refresh || !api->timer_start || !api->label || !api->button || !api->set_text ||
        api->width < 480 || api->height < 600) {
        return XZ_APP_ERR_UNSUPPORTED;
    }
    zh = api->language && api->language[0] == 'z';
    const int pad = 16, gap = 8, width = api->width - pad * 2;
    if (!api->label(api->root, pad, 8, width, 40, zh ? "摄像与图片" : "Imaging")) {
        return XZ_APP_ERR_MEMORY;
    }
    static const char* const names[] = {"Cam", "JPEG", "PNG", "A2I1", "A2L8"};
    for (unsigned i = 0; i < 5; ++i) {
        const int w = (width - gap * 4) / 5;
        sources[i] = Button(pad + i * (w + gap), 56, w,
                             i == 0 && zh ? "摄像" : names[i], SelectSource, i);
        if (!sources[i]) {
            return XZ_APP_ERR_MEMORY;
        }
    }
    picture = api->image(api->host_context, api->root, pad, 120, width, api->height - 458);
    if (!picture) {
        return XZ_APP_ERR_MEMORY;
    }
    static const char* const cn_modes[] = {"16 灰", "8 灰", "抖点", "黑白"};
    static const char* const en_modes[] = {"16 gray", "8 gray", "Dither", "B/W"};
    for (unsigned i = 0; i < 4; ++i) {
        const int w = (width - gap * 3) / 4;
        modes[i] = Button(pad + i * (w + gap), api->height - 316, w,
                           zh ? cn_modes[i] : en_modes[i], SelectMode, i);
        if (!modes[i]) {
            return XZ_APP_ERR_MEMORY;
        }
    }
    const int action_width = (width - gap * 2) / 3;
    void* previous = Button(pad, api->height - 248, action_width,
                              zh ? "上一项" : "Previous", SelectAdjustment, 0);
    void* reset = Button(pad + action_width + gap, api->height - 248, action_width,
                           zh ? "恢复默认" : "Reset", ResetEffects, 0);
    void* next = Button(pad + (action_width + gap) * 2, api->height - 248, action_width,
                          zh ? "下一项" : "Next", SelectAdjustment, 1);
    void* decrease = Button(pad, api->height - 180, 64, "-", AdjustValue, 0);
    adjustment_label = api->label(api->root, pad + 72, api->height - 174, width - 144, 44, "");
    void* increase = Button(api->width - pad - 64, api->height - 180, 64, "+", AdjustValue, 1);
    void* refresh = Button(pad, api->height - 112, (width - gap) / 2,
                             zh ? "全刷" : "Refresh", Refresh, 0);
    pause_button = Button(pad + (width + gap) / 2, api->height - 112, (width - gap) / 2,
                           zh ? "暂停" : "Pause", Pause, 0);
    status_label = api->label(api->root, pad, api->height - 48, width, 44, "");
    if (!adjustment_label || !pause_button || !status_label || !decrease || !increase || !refresh ||
        !reset || !previous || !next) {
        return XZ_APP_ERR_MEMORY;
    }
    if (api->set_style) {
        const xz_app_style_t style = {25, XZ_APP_ALIGN_CENTER, 0, 0, 0};
        api->set_style(adjustment_label, &style);
    }
    xz_app_image_status_t status;
    status.struct_size = sizeof(status);
    if (api->image_get_status(api->host_context, picture, &status) != XZ_APP_OK || status.struct_size < sizeof(status)) {
        return XZ_APP_ERR_UNSUPPORTED;
    }
    effects.struct_size = sizeof(effects);
    if (api->image_get_effects(api->host_context, picture, &effects) != XZ_APP_OK || effects.struct_size < sizeof(effects)) {
        return XZ_APP_ERR_UNSUPPORTED;
    }
    default_mode = effects.mode;
    ApplyEffects();
    SelectSource(0);
    return api->timer_start(api->host_context, 1000, 1, Tick, 0) ? XZ_APP_OK : XZ_APP_ERR_MEMORY;
}
