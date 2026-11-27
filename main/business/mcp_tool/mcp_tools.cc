#include <new>
#include <utility>
#include "mcp_tools.h"
#include "sysmgmt/tasks/task_executor.h"
#include "infra/protocols/mcp/mcp_server.h"
#include "boards/common/board/board.h"
#include "boards/common/audio/audio_codec.h"
#include "boards/common/camera/camera.h"
#include "boards/common/display/backlight.h"
#include "business/conversation/vision/camera_vision.h"

static CameraVision s_vision;

void InitializeDeviceMcpTools(Board& board, const McpToolActions& actions,
                              std::function<void(const std::string&)> send) {
    auto& server = McpServer::GetInstance();
    auto& vision = s_vision;
    server.Configure({std::move(send),
                      [](std::function<void()> task) { return TaskExecutor::Schedule(std::move(task)); },
                      [](const std::string& url, const std::string& token) { s_vision.Configure(url, token); }});
    server.AddTool("self.get_device_status",
                   "Get the current device status, including audio, screen, battery and network.\n"
                   "查询当前设备状态，包括音频、屏幕、电池和网络。",
                   PropertyList(), [&board](const PropertyList&) -> ReturnValue {
                       std::string status = board.GetDeviceStatusJson();
                       if (status.empty()) {
                           throw std::bad_alloc();
                       }
                       return status;
                   });

    server.AddTool("self.audio_speaker.set_volume",
                   "Set speaker volume. Query device status first if the current volume is unknown.\n"
                   "设置扬声器音量；当前音量未知时先查询设备状态。",
                   PropertyList({Property("volume", kPropertyTypeInteger, 0, 100)}),
                   [&board](const PropertyList& properties) -> ReturnValue {
                       auto* codec = board.GetAudioCodec();
                       if (codec == nullptr) {
                           return false;
                       }
                       codec->SetOutputVolume(properties["volume"].value<int>());
                       return true;
                   });

    if (auto* backlight = board.GetBacklight()) {
        server.AddTool("self.screen.set_brightness", "Set screen brightness.\n设置屏幕亮度。",
                       PropertyList({Property("brightness", kPropertyTypeInteger, 0, 100)}),
                       [backlight](const PropertyList& properties) -> ReturnValue {
                           backlight->SetBrightness(static_cast<uint8_t>(properties["brightness"].value<int>()), true);
                           return true;
                       });
    }

    if (auto* camera = board.GetCamera()) {
        server.AddTool("self.camera.take_photo",
                       "Take a photo and explain it when asked to see or identify something.\n"
                       "拍照并识别画面，适用于查看或辨认眼前事物。",
                       PropertyList({Property("question", kPropertyTypeString)}),
                       [camera, &vision](const PropertyList& properties) -> ReturnValue {
                           return vision.TakePhoto(*camera, properties["question"].value<std::string>());
                       });
    }

    server.AddUserOnlyTool("self.get_system_info", "Get system information.\n获取系统信息。", PropertyList(),
                           [&board](const PropertyList&) -> ReturnValue {
                               std::string info = board.GetSystemInfoJson();
                               if (info.empty()) {
                                   throw std::bad_alloc();
                               }
                               return info;
                           });

    if (actions.reboot) {
        server.AddUserOnlyTool("self.reboot", "Reboot the system.\n重启设备。", PropertyList(),
                               [request = actions.reboot](const PropertyList&) -> ReturnValue { return request(); });
    }

    if (actions.upgrade) {
        server.AddUserOnlyTool("self.upgrade_firmware",
                               "Download and install firmware, then reboot.\n下载并安装固件，完成后重启。",
                               PropertyList({Property("url", kPropertyTypeString)}),
                               [request = actions.upgrade](const PropertyList& properties) -> ReturnValue {
                                   return request(properties["url"].value<std::string>());
                               });
    }
}
