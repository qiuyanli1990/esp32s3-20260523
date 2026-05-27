#include "afe_audio_processor.h"
#include "sdkconfig.h"
#if CONFIG_BOARD_TYPE_ESP_KORVO_2
#include "boards/esp32s3-korvo-2/config.h"
#endif
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>

#include <algorithm>

#define PROCESSOR_RUNNING 0x01

#define TAG "AfeAudioProcessor"

namespace {

constexpr bool kAfeNeedsModelList =
#if CONFIG_SR_NSN_NSNET2 || CONFIG_SR_VADN_VADNET1_MEDIUM
    true;
#else
    false;
#endif

#if CONFIG_BOARD_TYPE_ESP_KORVO_2 && AUDIO_AFE_USE_VC_LOW_COST
constexpr afe_type_t kAfeType = AFE_TYPE_VC;
constexpr afe_mode_t kAfeMode = AFE_MODE_LOW_COST;
constexpr aec_mode_t kAecMode = AEC_MODE_VOIP_LOW_COST;
constexpr const char* kAfeProfile = "korvo2 vc-voip-low-cost";
#elif CONFIG_BOARD_TYPE_ESP_KORVO_2 && AUDIO_AFE_USE_SR_LOW_COST
constexpr afe_type_t kAfeType = AFE_TYPE_SR;
constexpr afe_mode_t kAfeMode = AFE_MODE_LOW_COST;
constexpr aec_mode_t kAecMode = AEC_MODE_SR_LOW_COST;
constexpr const char* kAfeProfile = "korvo2 low-cost AEC";
#else
constexpr afe_type_t kAfeType = AFE_TYPE_VC;
constexpr afe_mode_t kAfeMode = AFE_MODE_HIGH_PERF;
constexpr aec_mode_t kAecMode = AEC_MODE_VOIP_HIGH_PERF;
constexpr const char* kAfeProfile = "vc-voip-high-perf";
#endif

#if CONFIG_BOARD_TYPE_ESP_KORVO_2
constexpr int kAfeSampleRate = 16000;
#endif
constexpr size_t kSoftwareReferenceStreamChunks = 16;
constexpr UBaseType_t kProcessorTaskPriority = 4;
constexpr BaseType_t kProcessorTaskCore = 1;
#if CONFIG_BOARD_TYPE_ESP_KORVO_2
#ifndef AUDIO_KORVO2_SOFTWARE_REF_DELAY_MS
#define AUDIO_KORVO2_SOFTWARE_REF_DELAY_MS 0
#endif
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
    near_end_channel_indices_ = codec_->GetNearEndChannelIndices();
    if (near_end_channel_indices_.empty()) {
        near_end_channel_indices_.push_back(0);
    }

    // Pre-allocate output buffer capacity
    output_buffer_.reserve(frame_samples_);

    std::string input_format = codec_->GetAfeInputFormat();
    int afe_total_ch_num = codec_->afe_input_channels();
    int afe_mic_num = codec_->near_end_input_channels();
    int ref_num = codec_->has_aec_reference() ? 1 : 0;
#if CONFIG_BOARD_TYPE_ESP_KORVO_2
    if (codec_->has_hardware_input_reference()) {
#ifdef AUDIO_KORVO2_AFE_INPUT_FORMAT
        input_format = AUDIO_KORVO2_AFE_INPUT_FORMAT;
#endif
#ifdef AUDIO_KORVO2_AFE_TOTAL_CH_NUM
        afe_total_ch_num = AUDIO_KORVO2_AFE_TOTAL_CH_NUM;
#endif
#ifdef AUDIO_KORVO2_AFE_MIC_NUM
        afe_mic_num = AUDIO_KORVO2_AFE_MIC_NUM;
#endif
#ifdef AUDIO_KORVO2_AFE_REF_NUM
        ref_num = AUDIO_KORVO2_AFE_REF_NUM;
#endif
    }
#endif
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
    
    afe_config_t* afe_config = afe_config_init(input_format.c_str(), models, kAfeType, kAfeMode);
    if (afe_config == nullptr) {
        ESP_LOGE(TAG, "Failed to allocate AFE config");
        return false;
    }
    afe_config->aec_mode = kAecMode;
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

#if CONFIG_BOARD_TYPE_ESP_KORVO_2
    const bool korvo_dual_mic_hw_ref =
        codec_->has_hardware_input_reference() && afe_mic_num > 1;
    afe_config->se_init = korvo_dual_mic_hw_ref;
    if (!korvo_dual_mic_hw_ref) {
        afe_config->ns_init = false;
    }
    afe_config->agc_init = false;
    afe_config->vad_init = false;
    afe_config->wakenet_init = false;
    afe_config->fixed_first_channel = true;
    afe_config->afe_ringbuf_size = 25;
    afe_config->pcm_config.total_ch_num = afe_total_ch_num;
    afe_config->pcm_config.mic_num = afe_mic_num;
    afe_config->pcm_config.ref_num = ref_num;
    afe_config->pcm_config.sample_rate = 16000;
    afe_config = afe_config_check(afe_config);
    if (afe_config == nullptr) {
        ESP_LOGE(TAG, "Failed to validate Korvo AFE config");
        return false;
    }
#endif

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
    input_buffer_.reserve(feed_size * codec_->input_channels() * 4);
    size_t reference_stream_bytes = 0;
    if (codec_->using_software_input_reference()) {
        reference_stream_bytes = feed_size * sizeof(int16_t) * kSoftwareReferenceStreamChunks;
        if (reference_stream_ == nullptr) {
            reference_stream_ = xStreamBufferCreateWithCaps(
                reference_stream_bytes,
                sizeof(int16_t),
                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (reference_stream_ == nullptr) {
                ESP_LOGE(TAG, "Failed to create software reference stream");
                afe_iface_->destroy(afe_data_);
                afe_data_ = nullptr;
                afe_iface_ = nullptr;
                return false;
            }
        } else {
            xStreamBufferReset(reference_stream_);
        }
        software_reference_chunk_.resize(feed_size);
        software_afe_input_.resize(feed_size * codec_->afe_input_channels());
    }
    ESP_LOGI(TAG,
        "AFE initialized profile=%s feed=%u fetch=%u out_frame_ms=%d input_format=%s total_ch=%d mic=%d ref=%d hw_ref=%d sw_ref=%d ref_stream=%u",
        kAfeProfile,
        static_cast<unsigned>(feed_size),
        static_cast<unsigned>(fetch_size),
        frame_duration_ms,
        input_format.c_str(),
        afe_total_ch_num,
        afe_mic_num,
        ref_num,
        codec_->has_hardware_input_reference(),
        codec_->using_software_input_reference(),
        static_cast<unsigned>(reference_stream_bytes));

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
    }, "audio_communication", task_stack_depth, this, kProcessorTaskPriority,
        processor_task_stack_, processor_task_buffer_, kProcessorTaskCore);
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
    ESP_LOGI(TAG, "AFE processor task created: priority=%u core=%d",
        static_cast<unsigned>(kProcessorTaskPriority),
        static_cast<int>(kProcessorTaskCore));
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
    if (reference_stream_ != nullptr) {
        vStreamBufferDelete(reference_stream_);
        reference_stream_ = nullptr;
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
    FeedReference(data.data(), data.size());
}

void AfeAudioProcessor::FeedReference(const int16_t* data, size_t samples) {
    if (afe_data_ == nullptr || codec_ == nullptr || !codec_->using_software_input_reference()
        || data == nullptr || samples == 0) {
        return;
    }

    if (!IsRunning()) {
        return;
    }

    if (reference_stream_ == nullptr) {
        return;
    }

    const size_t bytes = samples * sizeof(int16_t);
    size_t space = xStreamBufferSpacesAvailable(reference_stream_);
    if (space < bytes) {
        size_t bytes_to_drop = bytes - space;
        bytes_to_drop = (bytes_to_drop + sizeof(int16_t) - 1) & ~(sizeof(int16_t) - 1);
        uint8_t discard[320];
        while (bytes_to_drop > 0) {
            const size_t chunk = bytes_to_drop > sizeof(discard) ? sizeof(discard) : bytes_to_drop;
            const size_t dropped = xStreamBufferReceive(reference_stream_, discard, chunk, 0);
            if (dropped == 0) {
                break;
            }
            bytes_to_drop -= dropped;
        }
    }

    xStreamBufferSend(reference_stream_, data, bytes, 0);
    last_reference_feed_us_ = esp_timer_get_time();
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
    if (reference_stream_ != nullptr) {
        xStreamBufferReset(reference_stream_);
    }
    last_reference_feed_us_ = 0;
    software_reference_log_us_ = 0;
    software_reference_wait_count_ = 0;
    software_reference_zero_count_ = 0;
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

#if CONFIG_BOARD_TYPE_ESP_KORVO_2 && AUDIO_AFE_USE_SR_LOW_COST
        auto res = afe_iface_->fetch(afe_data_);
#else
        auto res = afe_iface_->fetch_with_delay(afe_data_, portMAX_DELAY);
#endif
        if ((xEventGroupGetBits(event_group_) & PROCESSOR_RUNNING) == 0) {
            continue;
        }
        if (res == nullptr || res->ret_value == ESP_FAIL || res->data == nullptr || res->data_size <= 0) {
            if (res != nullptr) {
                ESP_LOGI(TAG, "Error code: %d", res->ret_value);
            }
#if CONFIG_BOARD_TYPE_ESP_KORVO_2 && AUDIO_AFE_USE_SR_LOW_COST
            vTaskDelay(pdMS_TO_TICKS(10));
#endif
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
    const int raw_input_channels = std::max(1, codec_->input_channels());
    const int near_end_channels = codec_->near_end_input_channels();
    const int afe_channels = codec_->afe_input_channels();
    const size_t input_chunk_size = feed_size * raw_input_channels;
    size_t reference_delay_samples = 0;
#if CONFIG_BOARD_TYPE_ESP_KORVO_2
    reference_delay_samples =
        static_cast<size_t>(kAfeSampleRate * AUDIO_KORVO2_SOFTWARE_REF_DELAY_MS / 1000);
#endif
    const size_t required_reference_samples = feed_size + reference_delay_samples;
    const size_t required_reference_bytes = required_reference_samples * sizeof(int16_t);
    const size_t feed_reference_bytes = feed_size * sizeof(int16_t);

    if (software_reference_chunk_.size() < feed_size) {
        software_reference_chunk_.resize(feed_size);
    }
    if (software_afe_input_.size() < feed_size * afe_channels) {
        software_afe_input_.resize(feed_size * afe_channels);
    }

    while (input_buffer_.size() >= input_chunk_size) {
        bool use_reference = false;
        size_t reference_buffer_size = reference_stream_ != nullptr
            ? xStreamBufferBytesAvailable(reference_stream_) / sizeof(int16_t)
            : 0;

        std::fill(software_reference_chunk_.begin(), software_reference_chunk_.begin() + feed_size, 0);
        size_t received = 0;
        if (reference_stream_ != nullptr
            && xStreamBufferBytesAvailable(reference_stream_) >= required_reference_bytes) {
            if (reference_delay_samples > 0) {
                uint8_t discard[320];
                size_t delay_bytes = reference_delay_samples * sizeof(int16_t);
                while (delay_bytes > 0) {
                    const size_t chunk = delay_bytes > sizeof(discard) ? sizeof(discard) : delay_bytes;
                    const size_t dropped = xStreamBufferReceive(reference_stream_, discard, chunk, 0);
                    if (dropped == 0) {
                        break;
                    }
                    delay_bytes -= dropped;
                }
            }

            received = xStreamBufferReceive(reference_stream_,
                software_reference_chunk_.data(), feed_reference_bytes, 0);
            use_reference = received == feed_reference_bytes;
            reference_buffer_size = xStreamBufferBytesAvailable(reference_stream_) / sizeof(int16_t);
        }

        if (received < feed_reference_bytes) {
            if (received > 0 || reference_buffer_size > 0) {
                ++software_reference_wait_count_;
            } else {
                ++software_reference_zero_count_;
            }
        }

        std::fill(software_afe_input_.begin(), software_afe_input_.begin() + feed_size * afe_channels, 0);
        for (size_t frame = 0; frame < feed_size; ++frame) {
            const size_t mic_base = frame * raw_input_channels;
            const size_t afe_base = frame * afe_channels;
            const int16_t reference_sample =
                use_reference ? software_reference_chunk_[frame] : 0;

            if (codec_->reference_order() == AudioReferenceOrder::kReferenceBeforeNearEnd) {
                software_afe_input_[afe_base] = reference_sample;
                for (int channel = 0; channel < near_end_channels; ++channel) {
                    int raw_channel = channel < static_cast<int>(near_end_channel_indices_.size())
                        ? near_end_channel_indices_[channel]
                        : channel;
                    software_afe_input_[afe_base + 1 + channel] =
                        raw_channel >= 0 && raw_channel < raw_input_channels
                            ? input_buffer_[mic_base + raw_channel]
                            : 0;
                }
            } else {
                for (int channel = 0; channel < near_end_channels; ++channel) {
                    int raw_channel = channel < static_cast<int>(near_end_channel_indices_.size())
                        ? near_end_channel_indices_[channel]
                        : channel;
                    software_afe_input_[afe_base + channel] =
                        raw_channel >= 0 && raw_channel < raw_input_channels
                            ? input_buffer_[mic_base + raw_channel]
                            : 0;
                }
                software_afe_input_[afe_base + near_end_channels] = reference_sample;
            }
        }

        afe_iface_->feed(afe_data_, software_afe_input_.data());
        input_buffer_.erase(input_buffer_.begin(), input_buffer_.begin() + input_chunk_size);

#if CONFIG_BOARD_TYPE_ESP_KORVO_2
        const int64_t after_feed_us = esp_timer_get_time();
        if (after_feed_us - software_reference_log_us_ >= 1000000) {
            software_reference_log_us_ = after_feed_us;
            ESP_LOGI(TAG,
                "AFE software ref feed: mic_buf=%u ref_buf=%u need=%u got=%u delay_ms=%d use_ref=%d wait=%lu zero=%lu",
                static_cast<unsigned>(input_buffer_.size()),
                static_cast<unsigned>(reference_buffer_size),
                static_cast<unsigned>(required_reference_samples),
                static_cast<unsigned>(received / sizeof(int16_t)),
                AUDIO_KORVO2_SOFTWARE_REF_DELAY_MS,
                use_reference ? 1 : 0,
                static_cast<unsigned long>(software_reference_wait_count_),
                static_cast<unsigned long>(software_reference_zero_count_));
        }
#else
        const int64_t after_feed_us = esp_timer_get_time();
        if (after_feed_us - software_reference_log_us_ >= 1000000) {
            software_reference_log_us_ = after_feed_us;
            ESP_LOGI(TAG,
                "AFE software ref feed: mic_buf=%u ref_buf=%u need=%u got=%u delay_ms=%d use_ref=%d wait=%lu zero=%lu",
                static_cast<unsigned>(input_buffer_.size()),
                static_cast<unsigned>(reference_buffer_size),
                static_cast<unsigned>(required_reference_samples),
                static_cast<unsigned>(received / sizeof(int16_t)),
                0,
                use_reference ? 1 : 0,
                static_cast<unsigned long>(software_reference_wait_count_),
                static_cast<unsigned long>(software_reference_zero_count_));
        }
#endif
    }
}
