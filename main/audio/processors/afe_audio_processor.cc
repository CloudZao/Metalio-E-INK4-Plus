#include "afe_audio_processor.h"
#include <esp_log.h>
#include <esp_heap_caps.h>

#include <freertos/FreeRTOS.h>
#include <freertos/idf_additions.h>
#include <freertos/task.h>

#define TAG "AfeAudioProcessor"

AfeAudioProcessor::AfeAudioProcessor()
    : afe_data_(nullptr) {
    event_group_ = xEventGroupCreate();
}

void AfeAudioProcessor::Initialize(AudioCodec* codec, int frame_duration_ms, srmodel_list_t* models_list) {
    if (afe_data_ != nullptr) {
        ESP_LOGW(TAG, "Initialize skipped: already initialized");
        return;
    }

    codec_ = codec;
    frame_samples_ = frame_duration_ms * 16000 / 1000;

    // Pre-allocate output buffer capacity
    output_buffer_.reserve(frame_samples_);

    int ref_num = codec_->input_reference() ? 1 : 0;

    std::string input_format;
    for (int i = 0; i < codec_->input_channels() - ref_num; i++) {
        input_format.push_back('M');
    }
    for (int i = 0; i < ref_num; i++) {
        input_format.push_back('R');
    }

    srmodel_list_t *models;
    if (models_list == nullptr) {
        models = esp_srmodel_init("model");
    } else {
        models = models_list;
    }

    char* ns_model_name = esp_srmodel_filter(models, ESP_NSNET_PREFIX, NULL);
    char* vad_model_name = esp_srmodel_filter(models, ESP_VADN_PREFIX, NULL);
    
    afe_config_t* afe_config = afe_config_init(input_format.c_str(), NULL, AFE_TYPE_VC, AFE_MODE_HIGH_PERF);
    afe_config->aec_mode = AEC_MODE_VOIP_HIGH_PERF;
    afe_config->vad_mode = VAD_MODE_0;
    afe_config->vad_min_noise_ms = 100;
    if (vad_model_name != nullptr) {
        afe_config->vad_model_name = vad_model_name;
    }

    if (ns_model_name != nullptr) {
        afe_config->ns_init = true;
        afe_config->ns_model_name = ns_model_name;
        afe_config->afe_ns_mode = AFE_NS_MODE_NET;
    } else {
        afe_config->ns_init = false;
    }

    afe_config->agc_init = false;
    afe_config->memory_alloc_mode = AFE_MEMORY_ALLOC_MORE_PSRAM;

#ifdef CONFIG_USE_DEVICE_AEC
    afe_config->aec_init = true;
    afe_config->vad_init = false;
#else
    afe_config->aec_init = false;
    afe_config->vad_init = true;
#endif

    afe_iface_ = esp_afe_handle_from_config(afe_config);
    afe_data_ = afe_iface_->create_from_config(afe_config);

    xEventGroupClearBits(event_group_, kRunning | kExit);
    BaseType_t ok = xTaskCreateWithCaps(
        [](void* arg) {
            auto this_ = (AfeAudioProcessor*)arg;
            this_->AudioProcessorTask();
            this_->communication_task_ = nullptr;
            vTaskDeleteWithCaps(NULL);
        },
        "audio_communication", 4096, this, 3, &communication_task_,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ok != pdPASS) {
        ESP_LOGE(TAG,
                 "Failed to create audio_communication (SPIRAM free=%u largest=%u)",
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)),
                 static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM)));
        communication_task_ = nullptr;
        if (afe_data_ != nullptr) {
            afe_iface_->destroy(afe_data_);
            afe_data_ = nullptr;
        }
        afe_iface_ = nullptr;
    }
}

void AfeAudioProcessor::Deinitialize() {
    if (afe_data_ == nullptr && communication_task_ == nullptr) {
        return;
    }

    xEventGroupClearBits(event_group_, kRunning);
    xEventGroupSetBits(event_group_, kExit);

    // 任务可能堵在 fetch；喂一帧静音尽量唤醒，再用短超时轮询退出
    if (afe_iface_ != nullptr && afe_data_ != nullptr) {
        const size_t feed = afe_iface_->get_feed_chunksize(afe_data_);
        if (feed > 0) {
            std::vector<int16_t> zeros(feed * (codec_ ? codec_->input_channels() : 1), 0);
            afe_iface_->feed(afe_data_, zeros.data());
        }
        afe_iface_->reset_buffer(afe_data_);
    }

    for (int i = 0; i < 50 && communication_task_ != nullptr; ++i) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    if (communication_task_ != nullptr) {
        ESP_LOGW(TAG, "audio_communication did not exit; continue destroy");
        communication_task_ = nullptr;
    }

    if (afe_data_ != nullptr && afe_iface_ != nullptr) {
        afe_iface_->destroy(afe_data_);
        afe_data_ = nullptr;
    }
    afe_iface_ = nullptr;
    is_speaking_ = false;
    output_buffer_.clear();
    output_buffer_.shrink_to_fit();
    xEventGroupClearBits(event_group_, kRunning | kExit);
    ESP_LOGI(TAG, "AFE deinitialized");
}

AfeAudioProcessor::~AfeAudioProcessor() {
    Deinitialize();
    if (event_group_ != nullptr) {
        vEventGroupDelete(event_group_);
        event_group_ = nullptr;
    }
}

size_t AfeAudioProcessor::GetFeedSize() {
    if (afe_data_ == nullptr) {
        return 0;
    }
    return afe_iface_->get_feed_chunksize(afe_data_);
}

void AfeAudioProcessor::Feed(std::vector<int16_t>&& data) {
    if (afe_data_ == nullptr) {
        return;
    }
    afe_iface_->feed(afe_data_, data.data());
}

void AfeAudioProcessor::Start() {
    xEventGroupClearBits(event_group_, kExit);
    xEventGroupSetBits(event_group_, kRunning);
}

void AfeAudioProcessor::Stop() {
    xEventGroupClearBits(event_group_, kRunning);
    if (afe_data_ != nullptr) {
        afe_iface_->reset_buffer(afe_data_);
    }
}

bool AfeAudioProcessor::IsRunning() {
    return xEventGroupGetBits(event_group_) & kRunning;
}

void AfeAudioProcessor::OnOutput(std::function<void(std::vector<int16_t>&& data)> callback) {
    output_callback_ = callback;
}

void AfeAudioProcessor::OnVadStateChange(std::function<void(bool speaking)> callback) {
    vad_state_change_callback_ = callback;
}

void AfeAudioProcessor::AudioProcessorTask() {
    auto fetch_size = afe_iface_->get_fetch_chunksize(afe_data_);
    auto feed_size = afe_iface_->get_feed_chunksize(afe_data_);
    ESP_LOGI(TAG, "Audio communication task started, feed size: %d fetch size: %d",
        feed_size, fetch_size);

    while (true) {
        EventBits_t bits = xEventGroupWaitBits(event_group_, kRunning | kExit, pdFALSE, pdFALSE, portMAX_DELAY);
        if (bits & kExit) {
            break;
        }
        if ((bits & kRunning) == 0) {
            continue;
        }

        // 短超时：Deinitialize 设 kExit 后不必永远堵在 fetch
        auto res = afe_iface_->fetch_with_delay(afe_data_, pdMS_TO_TICKS(100));
        if (xEventGroupGetBits(event_group_) & kExit) {
            break;
        }
        if ((xEventGroupGetBits(event_group_) & kRunning) == 0) {
            continue;
        }
        if (res == nullptr || res->ret_value == ESP_FAIL) {
            if (res != nullptr) {
                ESP_LOGI(TAG, "Error code: %d", res->ret_value);
            }
            continue;
        }

        // VAD state change
        if (vad_state_change_callback_) {
            if (res->vad_state == VAD_SPEECH && !is_speaking_) {
                is_speaking_ = true;
                vad_state_change_callback_(true);
            } else if (res->vad_state == VAD_SILENCE && is_speaking_) {
                is_speaking_ = false;
                vad_state_change_callback_(false);
            }
        }

        if (output_callback_) {
            size_t samples = res->data_size / sizeof(int16_t);
            
            // Add data to buffer
            output_buffer_.insert(output_buffer_.end(), res->data, res->data + samples);
            
            // Output complete frames when buffer has enough data
            while (output_buffer_.size() >= frame_samples_) {
                if (output_buffer_.size() == frame_samples_) {
                    // If buffer size equals frame size, move the entire buffer
                    output_callback_(std::move(output_buffer_));
                    output_buffer_.clear();
                    output_buffer_.reserve(frame_samples_);
                } else {
                    // If buffer size exceeds frame size, copy one frame and remove it
                    output_callback_(std::vector<int16_t>(output_buffer_.begin(), output_buffer_.begin() + frame_samples_));
                    output_buffer_.erase(output_buffer_.begin(), output_buffer_.begin() + frame_samples_);
                }
            }
        }
    }
    ESP_LOGI(TAG, "Audio communication task exiting");
}

void AfeAudioProcessor::EnableDeviceAec(bool enable) {
    if (afe_data_ == nullptr || afe_iface_ == nullptr) {
        return;
    }
    if (enable) {
#if CONFIG_USE_DEVICE_AEC
        afe_iface_->disable_vad(afe_data_);
        afe_iface_->enable_aec(afe_data_);
#else
        ESP_LOGE(TAG, "Device AEC is not supported");
#endif
    } else {
        afe_iface_->disable_aec(afe_data_);
        afe_iface_->enable_vad(afe_data_);
    }
}
