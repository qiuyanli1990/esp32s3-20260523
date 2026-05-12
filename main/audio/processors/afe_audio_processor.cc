#include "afe_audio_processor.h"
#include "sdkconfig.h"
#include <esp_heap_caps.h>
#include <esp_log.h>

#define PROCESSOR_RUNNING 0x01

#define TAG "AfeAudioProcessor"

namespace {

constexpr bool kAfeNeedsModelList =
#if CONFIG_SR_NSN_NSNET2 || CONFIG_SR_VADN_VADNET1_MEDIUM
    true;
#else
    false;
#endif

}  // namespace

AfeAudioProcessor::AfeAudioProcessor()
    : afe_data_(nullptr) {
    event_group_ = xEventGroupCreate();
}

bool AfeAudioProcessor::Initialize(AudioCodec* codec, int frame_duration_ms, srmodel_list_t* models_list) {
    if (initialized_) {
        ESP_LOGI(TAG, "AFE audio processor already initialized");
        return true;
    }
    codec_ = codec;
    frame_samples_ = frame_duration_ms * 16000 / 1000;

    // Pre-allocate output buffer capacity
    output_buffer_.reserve(frame_samples_);

    const std::string input_format = codec_->GetAfeInputFormat();
    const int ref_num = codec_->has_aec_reference() ? 1 : 0;

    srmodel_list_t* models = models_list;
    if (models == nullptr && kAfeNeedsModelList) {
        models = esp_srmodel_init("model");
        if (models == nullptr) {
            ESP_LOGE(TAG, "Failed to load required AFE speech models");
            return false;
        }
    }
    if (models == nullptr) {
        ESP_LOGI(TAG, "AFE will use built-in WebRTC AEC/NS/VAD without packaged srmodels");
    }

    char* ns_model_name = models != nullptr ? esp_srmodel_filter(models, ESP_NSNET_PREFIX, NULL) : nullptr;
    char* vad_model_name = models != nullptr ? esp_srmodel_filter(models, ESP_VADN_PREFIX, NULL) : nullptr;

#if CONFIG_SR_NSN_NSNET2
    if (ns_model_name == nullptr) {
        ESP_LOGE(TAG, "NSNET2 is enabled but no NS model is available");
        return false;
    }
#endif

#if CONFIG_SR_VADN_VADNET1_MEDIUM
    if (vad_model_name == nullptr) {
        ESP_LOGE(TAG, "VADNET1 medium is enabled but no VAD model is available");
        return false;
    }
#endif
    
    afe_config_t* afe_config = afe_config_init(input_format.c_str(), models, AFE_TYPE_VC, AFE_MODE_HIGH_PERF);
    if (afe_config == nullptr) {
        ESP_LOGE(TAG, "Failed to allocate AFE config");
        return false;
    }
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

    afe_config->aec_init = codec_->has_aec_reference();
    afe_config->vad_init = true;

    afe_iface_ = esp_afe_handle_from_config(afe_config);
    if (afe_iface_ == nullptr) {
        ESP_LOGE(TAG, "Failed to create AFE interface");
        afe_config_free(afe_config);
        return false;
    }
    afe_data_ = afe_iface_->create_from_config(afe_config);
    afe_config_free(afe_config);
    if (afe_data_ == nullptr) {
        ESP_LOGE(TAG, "Failed to create AFE data handle");
        afe_iface_ = nullptr;
        return false;
    }

    const auto feed_size = afe_iface_->get_feed_chunksize(afe_data_);
    const auto fetch_size = afe_iface_->get_fetch_chunksize(afe_data_);
    ESP_LOGI(TAG, "AFE initialized, feed size: %u fetch size: %u input_format=%s ref=%d",
        static_cast<unsigned>(feed_size), static_cast<unsigned>(fetch_size), input_format.c_str(), ref_num);

    const size_t task_stack_depth = 8192;
    if (processor_task_stack_ == nullptr) {
        processor_task_stack_ = static_cast<StackType_t*>(
            heap_caps_malloc(task_stack_depth * sizeof(StackType_t), MALLOC_CAP_SPIRAM));
    }
    if (processor_task_buffer_ == nullptr) {
        processor_task_buffer_ = static_cast<StaticTask_t*>(heap_caps_malloc(sizeof(StaticTask_t), MALLOC_CAP_INTERNAL));
    }
    if (processor_task_stack_ == nullptr || processor_task_buffer_ == nullptr) {
        ESP_LOGE(TAG, "Failed to allocate AFE task memory, free internal=%u free psram=%u",
            static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
            static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
        if (processor_task_buffer_ != nullptr) {
            heap_caps_free(processor_task_buffer_);
            processor_task_buffer_ = nullptr;
        }
        if (processor_task_stack_ != nullptr) {
            heap_caps_free(processor_task_stack_);
            processor_task_stack_ = nullptr;
        }
        afe_iface_->destroy(afe_data_);
        afe_data_ = nullptr;
        afe_iface_ = nullptr;
        return false;
    }

    processor_task_handle_ = xTaskCreateStaticPinnedToCore([](void* arg) {
        auto this_ = (AfeAudioProcessor*)arg;
        this_->AudioProcessorTask();
        vTaskDelete(NULL);
    }, "audio_communication", task_stack_depth, this, 4, processor_task_stack_, processor_task_buffer_, 1);
    if (processor_task_handle_ == nullptr) {
        ESP_LOGE(TAG, "Failed to create AFE processor task, free internal=%u free psram=%u",
            static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
            static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
        afe_iface_->destroy(afe_data_);
        afe_data_ = nullptr;
        afe_iface_ = nullptr;
        return false;
    }
    initialized_ = true;
    ESP_LOGI(TAG, "AFE processor task created");
    return true;
}

AfeAudioProcessor::~AfeAudioProcessor() {
    if (afe_data_ != nullptr) {
        afe_iface_->destroy(afe_data_);
    }
    if (processor_task_buffer_ != nullptr) {
        heap_caps_free(processor_task_buffer_);
    }
    if (processor_task_stack_ != nullptr) {
        heap_caps_free(processor_task_stack_);
    }
    vEventGroupDelete(event_group_);
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

    std::lock_guard<std::mutex> lock(input_buffer_mutex_);
    // Check running state inside lock to avoid TOCTOU race with Stop()
    if (!IsRunning()) {
        return;
    }
    input_buffer_.insert(input_buffer_.end(), data.begin(), data.end());
    const size_t feed_size = afe_iface_->get_feed_chunksize(afe_data_);
    if (codec_->using_software_input_reference()) {
        FeedSoftwareReferenceFramesLocked(feed_size);
    } else {
        FeedHardwareInputFramesLocked(feed_size);
    }
}

void AfeAudioProcessor::FeedReference(std::vector<int16_t>&& data) {
    if (afe_data_ == nullptr || !codec_->using_software_input_reference() || data.empty()) {
        return;
    }

    std::lock_guard<std::mutex> lock(input_buffer_mutex_);
    if (!IsRunning()) {
        return;
    }

    reference_buffer_.insert(reference_buffer_.end(), data.begin(), data.end());
    const size_t feed_size = afe_iface_->get_feed_chunksize(afe_data_);
    const size_t max_reference_samples = feed_size * 20;
    if (reference_buffer_.size() > max_reference_samples) {
        reference_buffer_.erase(reference_buffer_.begin(),
            reference_buffer_.begin() + (reference_buffer_.size() - max_reference_samples));
    }
}

void AfeAudioProcessor::Start() {
    xEventGroupSetBits(event_group_, PROCESSOR_RUNNING);
}

void AfeAudioProcessor::Stop() {
    xEventGroupClearBits(event_group_, PROCESSOR_RUNNING);

    std::lock_guard<std::mutex> lock(input_buffer_mutex_);
    if (afe_data_ != nullptr) {
        afe_iface_->reset_buffer(afe_data_);
    }
    input_buffer_.clear();
    reference_buffer_.clear();
}

bool AfeAudioProcessor::IsRunning() {
    return xEventGroupGetBits(event_group_) & PROCESSOR_RUNNING;
}

bool AfeAudioProcessor::WantsSpeakerReference() const {
    return codec_ != nullptr && codec_->using_software_input_reference();
}

void AfeAudioProcessor::OnOutput(std::function<void(std::vector<int16_t>&& data)> callback) {
    output_callback_ = callback;
}

void AfeAudioProcessor::OnVadStateChange(std::function<void(bool speaking)> callback) {
    vad_state_change_callback_ = callback;
}

void AfeAudioProcessor::AudioProcessorTask() {
    if (afe_data_ == nullptr || afe_iface_ == nullptr) {
        ESP_LOGE(TAG, "AFE processor task started without initialized AFE handle");
        return;
    }
    auto fetch_size = afe_iface_->get_fetch_chunksize(afe_data_);
    auto feed_size = afe_iface_->get_feed_chunksize(afe_data_);
    ESP_LOGI(TAG, "Audio communication task started, feed size: %d fetch size: %d",
        feed_size, fetch_size);

    while (true) {
        xEventGroupWaitBits(event_group_, PROCESSOR_RUNNING, pdFALSE, pdTRUE, portMAX_DELAY);

        auto res = afe_iface_->fetch_with_delay(afe_data_, portMAX_DELAY);
        if ((xEventGroupGetBits(event_group_) & PROCESSOR_RUNNING) == 0) {
            continue;
        }
        if (res == nullptr || res->ret_value == ESP_FAIL) {
            if (res != nullptr) {
                ESP_LOGI(TAG, "Error code: %d", res->ret_value);
            }
            continue;
        }
        ++fetch_count_;

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
}

bool AfeAudioProcessor::EnableDeviceAec(bool enable) {
    if (afe_data_ == nullptr || afe_iface_ == nullptr) {
        ESP_LOGE(TAG, "AFE is not initialized");
        return false;
    }
    if (enable) {
        if (!codec_->has_aec_reference()) {
            ESP_LOGE(TAG, "Device AEC requires a reference signal");
            return false;
        }
        afe_iface_->disable_vad(afe_data_);
        afe_iface_->enable_aec(afe_data_);
    } else {
        afe_iface_->disable_aec(afe_data_);
        afe_iface_->enable_vad(afe_data_);
    }
    return true;
}

void AfeAudioProcessor::FeedHardwareInputFramesLocked(size_t feed_size) {
    const size_t chunk_size = feed_size * codec_->input_channels();
    while (input_buffer_.size() >= chunk_size) {
        afe_iface_->feed(afe_data_, input_buffer_.data());
        input_buffer_.erase(input_buffer_.begin(), input_buffer_.begin() + chunk_size);
    }
}

void AfeAudioProcessor::FeedSoftwareReferenceFramesLocked(size_t feed_size) {
    const int near_end_channels = codec_->near_end_input_channels();
    const int afe_channels = codec_->afe_input_channels();
    const size_t mic_chunk_size = feed_size * near_end_channels;
    while (input_buffer_.size() >= mic_chunk_size) {
        std::vector<int16_t> afe_input(feed_size * afe_channels, 0);
        for (size_t frame = 0; frame < feed_size; ++frame) {
            const size_t mic_base = frame * near_end_channels;
            const size_t afe_base = frame * afe_channels;

            if (codec_->reference_order() == AudioReferenceOrder::kReferenceBeforeNearEnd) {
                if (frame < reference_buffer_.size()) {
                    afe_input[afe_base] = reference_buffer_[frame];
                }
                for (int channel = 0; channel < near_end_channels; ++channel) {
                    afe_input[afe_base + 1 + channel] = input_buffer_[mic_base + channel];
                }
            } else {
                for (int channel = 0; channel < near_end_channels; ++channel) {
                    afe_input[afe_base + channel] = input_buffer_[mic_base + channel];
                }
                if (frame < reference_buffer_.size()) {
                    afe_input[afe_base + near_end_channels] = reference_buffer_[frame];
                }
            }
        }

        afe_iface_->feed(afe_data_, afe_input.data());
        input_buffer_.erase(input_buffer_.begin(), input_buffer_.begin() + mic_chunk_size);
        if (reference_buffer_.size() >= feed_size) {
            reference_buffer_.erase(reference_buffer_.begin(), reference_buffer_.begin() + feed_size);
        } else {
            reference_buffer_.clear();
        }
    }
}
