#include "assistant_boot_photo.h"

#include "a2ui_image.h"
#include "a2ui_img_cache.h"
#include "api_endpoints.h"
#include "api_http.h"
#include "assistant_screen.h"
#include "assistant_screen_priv.h"
#include "assets/lang_config.h"
#include "board.h"
#include "camera.h"
#include "image_util.h"
#include "reader_types.h"
#include "screen_common.h"
#include "usb_uvc_still.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <new>
#include <string>
#include <vector>

#include <cJSON.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/idf_additions.h>
#include <freertos/task.h>


static constexpr const char* TAG = "AsstBootPhoto";
/** JPEG→L8 + 可选 A2I1 cache；栈放 SPIRAM，避免内部 RAM 碎片导致偶发 create 失败 */
static constexpr uint32_t kWorkerStack = 16 * 1024;
static constexpr UBaseType_t kWorkerPrio = 5;
static constexpr BaseType_t kWorkerCore = 0;
/** 对话区默认 Image 框；解码时再按此上限缩放。 */
static constexpr int kMaxImgW = kDefaultImgW;
static constexpr int kMaxImgH = kDefaultImgH;

static std::atomic<bool> s_busy{false};
static std::atomic<uint32_t> s_shot_seq{0};

struct AssistantBootInsertMsg {
    char* json = nullptr;
};

/** 失败路径自动清 busy；成功投递 LVGL 后 dismiss，由 OnInsertAsync 清。 */
struct AssistantBootBusyGuard {
    bool active = true;
    ~AssistantBootBusyGuard() {
        if (active) {
            s_busy.store(false, std::memory_order_release);
        }
    }
    void Dismiss() { active = false; }
};

struct AssistantBootJpegOwner {
    uint8_t* data = nullptr;
    size_t len = 0;
    ~AssistantBootJpegOwner() {
        if (data != nullptr) {
            free(data);
            data = nullptr;
            len = 0;
        }
    }
};

// 上传原始 JPEG 到 claw vision/upload（multipart file=）。
static bool UploadOriginalJpeg(const uint8_t* jpeg, size_t jpeg_len) {
    if (jpeg == nullptr || jpeg_len == 0) {
        return false;
    }
    auto network = Board::GetInstance().GetNetwork();
    if (network == nullptr) {
        ESP_LOGW(TAG, "upload skip: no network");
        return false;
    }
    auto http = network->CreateHttp(3);
    if (http == nullptr) {
        ESP_LOGW(TAG, "upload skip: CreateHttp failed");
        return false;
    }

    const std::string url = api::VisionUploadUrl();
    if (url.empty()) {
        ESP_LOGI(TAG, "skip vision upload: cloud endpoints blank");
        return false;
    }
    const std::string boundary = "----ESP32_ASST_CAM_BOUNDARY";

    api::ApplyCommonHeaders(http.get());
    http->SetHeader("Content-Type", "multipart/form-data; boundary=" + boundary);
    http->SetHeader("Transfer-Encoding", "chunked");
    http->SetTimeout(30000);

    api::LogHttpBinaryRequest(TAG, "POST", url, jpeg_len, "vision/upload");
    ESP_LOGI(TAG, "upload original JPEG %u bytes -> vision/upload Device-Id=%s",
             (unsigned)jpeg_len, api::DeviceId().c_str());

    if (!http->Open("POST", url)) {
        ESP_LOGW(TAG, "upload open failed err=0x%x", http->GetLastError());
        return false;
    }

    {
        std::string file_header;
        file_header += "--" + boundary + "\r\n";
        file_header += "Content-Disposition: form-data; name=\"file\"; filename=\"camera.jpg\"\r\n";
        file_header += "Content-Type: image/jpeg\r\n";
        file_header += "\r\n";
        if (http->Write(file_header.c_str(), file_header.size()) < 0) {
            ESP_LOGW(TAG, "upload write header failed");
            http->Close();
            return false;
        }
    }

    if (http->Write(reinterpret_cast<const char*>(jpeg), jpeg_len) < 0) {
        ESP_LOGW(TAG, "upload write body failed");
        http->Close();
        return false;
    }

    {
        std::string footer;
        footer += "\r\n--" + boundary + "--\r\n";
        if (http->Write(footer.c_str(), footer.size()) < 0) {
            ESP_LOGW(TAG, "upload write footer failed");
            http->Close();
            return false;
        }
    }
    http->Write("", 0);

    const int status = http->GetStatusCode();
    std::string resp = http->ReadAll();
    http->Close();
    api::LogHttpResponse(TAG, status, resp);

    if (status < 200 || status >= 300) {
        ESP_LOGW(TAG, "upload failed status=%d", status);
        return false;
    }
    ESP_LOGI(TAG, "upload ok status=%d", status);
    return true;
}

static bool PublishL8(const reader::RasterImage& raster, uint32_t seq, char* url_out, size_t url_cap) {
    if (url_out == nullptr || url_cap < 32 || raster.empty()) {
        return false;
    }
    snprintf(url_out, url_cap, "mem://assistant/boot_shot_%lu",
             static_cast<unsigned long>(seq));

    if (a2ui_image_put_local_l8(url_out, raster.width, raster.height, raster.pixels.data(),
                                raster.pixels.size()) != ESP_OK) {
        ESP_LOGE(TAG, "put_local_l8 failed");
        return false;
    }

    if (a2ui_img_cache_is_ready()) {
        std::vector<uint8_t> a2i1;
        if (reader::EncodeL8ToA2i1(raster, a2i1) && !a2i1.empty() &&
            a2i1.size() <= static_cast<size_t>(64 * 1024)) {
            const esp_err_t cerr = a2ui_img_cache_store(url_out, a2i1.data(), a2i1.size());
            if (cerr != ESP_OK) {
                ESP_LOGW(TAG, "cache store soft-fail: %s", esp_err_to_name(cerr));
            }
        } else {
            ESP_LOGW(TAG, "skip A2I1 cache encode");
        }
    }
    ESP_LOGI(TAG, "published %s L8 %ux%u", url_out, (unsigned)raster.width,
             (unsigned)raster.height);
    return true;
}

static char* BuildCapturingJson() {
    cJSON* root = cJSON_CreateObject();
    if (root == nullptr) {
        return nullptr;
    }
    cJSON* body = cJSON_AddObjectToObject(root, "updateComponents");
    cJSON* comps = body ? cJSON_AddArrayToObject(body, "components") : nullptr;
    if (body == nullptr || comps == nullptr) {
        cJSON_Delete(root);
        return nullptr;
    }
    cJSON_AddStringToObject(body, "root", "root");
    cJSON_AddStringToObject(body, "refresh", "partial");

    auto add_comp = [&](const char* id, const char* type) -> cJSON* {
        cJSON* c = cJSON_CreateObject();
        if (c == nullptr) {
            return nullptr;
        }
        cJSON_AddStringToObject(c, "id", id);
        cJSON_AddStringToObject(c, "component", type);
        cJSON_AddItemToArray(comps, c);
        return c;
    };

    cJSON* col = add_comp("root", "Column");
    cJSON* u_line = add_comp("u_line", "RichText");
    if (col == nullptr || u_line == nullptr) {
        cJSON_Delete(root);
        return nullptr;
    }
    cJSON_AddNumberToObject(col, "gap", kBootColGap);
    cJSON_AddNumberToObject(col, "padding", kBootColPadding);
    cJSON* children = cJSON_AddArrayToObject(col, "children");
    if (children == nullptr) {
        cJSON_Delete(root);
        return nullptr;
    }
    cJSON_AddItemToArray(children, cJSON_CreateString("u_line"));

    cJSON* spans = cJSON_AddArrayToObject(u_line, "spans");
    if (spans == nullptr) {
        cJSON_Delete(root);
        return nullptr;
    }
    cJSON* sp0 = cJSON_CreateObject();
    cJSON* sp1 = cJSON_CreateObject();
    if (sp0 == nullptr || sp1 == nullptr) {
        cJSON_Delete(sp0);
        cJSON_Delete(sp1);
        cJSON_Delete(root);
        return nullptr;
    }
    cJSON_AddStringToObject(sp0, "text", "用户：");
    cJSON_AddBoolToObject(sp0, "bold", true);
    cJSON_AddItemToArray(spans, sp0);
    const char* capt = Lang::Strings::SETTINGS_TEST_CAPTURING;
    cJSON_AddStringToObject(sp1, "text", capt != nullptr && capt[0] ? capt : "拍照中…");
    cJSON_AddBoolToObject(sp1, "bold", false);
    cJSON_AddItemToArray(spans, sp1);

    char* raw = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return raw;
}

static char* BuildInsertJson(const char* image_url, int img_w, int img_h) {
    if (image_url == nullptr || image_url[0] == '\0') {
        return nullptr;
    }
    cJSON* root = cJSON_CreateObject();
    if (root == nullptr) {
        return nullptr;
    }
    cJSON* body = cJSON_AddObjectToObject(root, "updateComponents");
    cJSON* comps = body ? cJSON_AddArrayToObject(body, "components") : nullptr;
    if (body == nullptr || comps == nullptr) {
        cJSON_Delete(root);
        return nullptr;
    }
    cJSON_AddStringToObject(body, "root", "root");
    cJSON_AddStringToObject(body, "refresh", "partial");

    auto add_comp = [&](const char* id, const char* type) -> cJSON* {
        cJSON* c = cJSON_CreateObject();
        if (c == nullptr) {
            return nullptr;
        }
        cJSON_AddStringToObject(c, "id", id);
        cJSON_AddStringToObject(c, "component", type);
        cJSON_AddItemToArray(comps, c);
        return c;
    };

    // 文案已在开拍时发过「用户：拍照中」；拍完只补 Image，不再发「用户：拍照」
    cJSON* col = add_comp("root", "Column");
    cJSON* wrap = add_comp("img_wrap", "Column");
    cJSON* img = add_comp("shot", "Image");
    if (col == nullptr || wrap == nullptr || img == nullptr) {
        cJSON_Delete(root);
        return nullptr;
    }

    cJSON_AddNumberToObject(col, "gap", kBootColGap);
    cJSON_AddNumberToObject(col, "padding", kBootColPadding);
    cJSON* children = cJSON_AddArrayToObject(col, "children");
    if (children == nullptr) {
        cJSON_Delete(root);
        return nullptr;
    }
    cJSON_AddItemToArray(children, cJSON_CreateString("img_wrap"));

    cJSON_AddStringToObject(wrap, "align", "center");
    cJSON_AddNumberToObject(wrap, "gap", 0);
    cJSON_AddNumberToObject(wrap, "padding", 0);
    cJSON* wch = cJSON_AddArrayToObject(wrap, "children");
    if (wch == nullptr) {
        cJSON_Delete(root);
        return nullptr;
    }
    cJSON_AddItemToArray(wch, cJSON_CreateString("shot"));

    cJSON_AddStringToObject(img, "url", image_url);
    cJSON_AddNumberToObject(img, "width", img_w > 0 ? img_w : kMaxImgW);
    cJSON_AddNumberToObject(img, "height", img_h > 0 ? img_h : kMaxImgH);

    char* raw = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return raw;
}

static void OnCapturingAsync(void* user_data) {
    auto* msg = static_cast<AssistantBootInsertMsg*>(user_data);
    if (msg == nullptr) {
        return;
    }
    // 仅插「拍照中」文案，不清 busy（拍照 worker 仍在进行）
    if (AssistantScreen::IsActive() && msg->json != nullptr && msg->json[0] != '\0') {
        AssistantScreen::AddMessage("user", msg->json);
    }
    cJSON_free(msg->json);
    msg->json = nullptr;
    delete msg;
}

static void OnInsertAsync(void* user_data) {
    auto* msg = static_cast<AssistantBootInsertMsg*>(user_data);
    if (msg == nullptr) {
        s_busy.store(false, std::memory_order_release);
        return;
    }
    if (AssistantScreen::IsActive() && msg->json != nullptr && msg->json[0] != '\0') {
        AssistantScreen::AddMessage("user", msg->json);
    }
    cJSON_free(msg->json);
    msg->json = nullptr;
    delete msg;
    s_busy.store(false, std::memory_order_release);
}

static void PhotoWorker(void* /*arg*/) {
    {
        AssistantBootBusyGuard busy;

        Camera* camera = Board::GetInstance().GetCamera();
        if (camera == nullptr) {
            ESP_LOGW(TAG, "no Camera");
        } else {
            // 唯一供电路径：UvcStillCamera::Capture（OTG on → P0.0 低 → 拍 → StopSession → OTG off）
            AssistantBootJpegOwner jpeg;
            if (!camera->CaptureStillJpeg(&jpeg.data, &jpeg.len) || jpeg.data == nullptr ||
                jpeg.len == 0) {
                ESP_LOGW(TAG, "CaptureStillJpeg failed (no camera / busy / error)");
            } else {
                ESP_LOGI(TAG, "captured JPEG %u bytes", (unsigned)jpeg.len);

                // 先传原始图（软失败）；Capture 已关 USB/OTG，HTTP 不持会话
                if (!UploadOriginalJpeg(jpeg.data, jpeg.len)) {
                    ESP_LOGW(TAG, "vision upload soft-fail, continue local insert");
                }

                reader::RasterImage raster;
                if (!reader::DecodeImageToL8(jpeg.data, jpeg.len, kMaxImgW, kMaxImgH, raster) ||
                    raster.empty()) {
                    ESP_LOGE(TAG, "DecodeImageToL8 failed");
                } else {
                    free(jpeg.data);
                    jpeg.data = nullptr;
                    jpeg.len = 0;

                    const uint32_t seq = s_shot_seq.fetch_add(1, std::memory_order_relaxed) + 1;
                    char url[96];
                    if (PublishL8(raster, seq, url, sizeof(url))) {
                        const int img_w = raster.width;
                        const int img_h = raster.height;
                        raster.Reset();

                        char* json = BuildInsertJson(url, img_w, img_h);
                        if (json != nullptr) {
                            if (!AssistantScreen::IsActive()) {
                                cJSON_free(json);
                            } else {
                                auto* msg = new (std::nothrow) AssistantBootInsertMsg();
                                if (msg == nullptr) {
                                    cJSON_free(json);
                                } else {
                                    msg->json = json;
                                    if (!ScreenLvAsync(OnInsertAsync, msg)) {
                                        ESP_LOGW(TAG, "ScreenLvAsync failed");
                                        cJSON_free(msg->json);
                                        msg->json = nullptr;
                                        delete msg;
                                    } else {
                                        busy.Dismiss();
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }
    vTaskDeleteWithCaps(nullptr);
}


bool AssistantBootPhoto_OnDoubleClick() {
    if (!AssistantScreen::IsActive()) {
        return false;
    }
    Camera* camera = Board::GetInstance().GetCamera();
    if (camera == nullptr) {
        ESP_LOGW(TAG, "double-click ignored: no Camera object");
        return true;
    }
    // 与 MCP/产测共用 UsbUvcStill 忙锁；再查一次避免空跑 worker
    if (UsbUvcStill::IsBusy()) {
        ESP_LOGW(TAG, "double-click ignored: USB UVC busy");
        return true;
    }
    bool expected = false;
    if (!s_busy.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        ESP_LOGW(TAG, "double-click ignored: photo busy");
        return true;
    }

    ESP_LOGI(TAG, "boot double-click -> 拍照中文案 + CaptureStillJpeg");

    // 先发「用户：拍照中」，再起拍照 worker（其余逻辑不变）
    char* capturing_json = BuildCapturingJson();
    if (capturing_json != nullptr) {
        auto* msg = new (std::nothrow) AssistantBootInsertMsg();
        if (msg == nullptr) {
            cJSON_free(capturing_json);
        } else {
            msg->json = capturing_json;
            if (!ScreenLvAsync(OnCapturingAsync, msg)) {
                ESP_LOGW(TAG, "capturing ScreenLvAsync failed");
                cJSON_free(msg->json);
                msg->json = nullptr;
                delete msg;
            }
        }
    }

    // 与书封/壁纸预览一致：任务栈放 SPIRAM。内部 24KB 连续块常因碎片不够 → create failed。
    const BaseType_t ok = xTaskCreatePinnedToCoreWithCaps(
        PhotoWorker, "asst_cam", kWorkerStack, nullptr, kWorkerPrio, nullptr, kWorkerCore,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ok != pdPASS) {
        s_busy.store(false, std::memory_order_release);
        ESP_LOGE(TAG,
                 "worker create failed: need stack=%u SPIRAM; int free=%u largest=%u; "
                 "psram free=%u largest=%u",
                 static_cast<unsigned>(kWorkerStack),
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
                 static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)),
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)),
                 static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM)));
    }
    return true;
}
