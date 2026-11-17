#include "app_util.h"
static void* result;
static uint8_t buffer[4096];
static void Check(void* context) {
    (void)context;
    const uint32_t total = app->asset_size;
    int ok = total == 16u * 1024u * 1024u;
    uint32_t tested = 0;
    for (uint32_t n = 0; ok && n < 17; ++n) {
        const uint32_t offset = (total - sizeof(buffer)) / 16 * n;
        if (app->read_asset(app->asset_context, offset, buffer, sizeof(buffer)) != sizeof(buffer)) { ok = 0; break; }
        for (uint32_t j = 0; j < sizeof(buffer); ++j) {
            uint32_t i = offset + j;
            uint8_t expected = (uint8_t)((i * 37u) ^ (i >> 8) ^ (i >> 16) ^ 0x5au);
            if (buffer[j] != expected) { ok = 0; break; }
        }
        ++tested;
    }
    ResetText(); Add(ok ? (zh ? "资源抽样校验通过" : "Resource sample check passed") : (zh ? "资源校验失败" : "Resource check failed"));
    Add("\n"); Number(tested); Add(zh ? " / 17 段" : " / 17 blocks");
    app->set_text(result, text);
}
int app_main(int argc, char** argv) {
    if (!AppInit(argc, argv) || app->struct_size < sizeof(*app) || !app->read_asset) return -1;
    Label(24, 24, app->width - 48, 60, zh ? "大包安装诊断" : "Large package diagnostics");
    ResetText(); Add(zh ? "资源大小：" : "Resource size: "); Number(app->asset_size / 1048576u); Add(" MiB");
    Label(24, 110, app->width - 48, 70, text);
    Label(24, 210, app->width - 48, 270, zh ? "安装时校验完整文件摘要。运行后可抽样读取资源的首部、中间和尾部，共 17 段。资源保留在 SD 卡，按需读取。" : "Installation verifies the full file hash. Check 17 resource blocks across the beginning, middle and end. Resources stay on SD and are read on demand.");
    result = Label(24, 520, app->width - 48, 150, zh ? "等待校验" : "Ready to check");
    Button(24, 760, app->width - 48, 90, zh ? "抽样校验资源" : "Check resource samples", Check, 0);
    return 0;
}
