#include "agora/agora_audio_bridge.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include <esp_heap_caps.h>
#include <esp_log.h>

namespace {

constexpr char kTag[] = "AgoraAudio";
constexpr uint32_t kCaptureTaskStackSize = 4096;

int16_t GetChannelPeakAbs(const std::vector<int16_t>& raw, int input_channels, int channel_index) {
    int16_t peak = 0;
    if (input_channels <= 0 || channel_index < 0 || channel_index >= input_channels) {
        return peak;
    }
    for (size_t i = channel_index; i < raw.size(); i += static_cast<size_t>(input_channels)) {
        const int sample = std::abs(static_cast<int>(raw[i]));
        if (sample > peak) {
            peak = static_cast<int16_t>(sample);
        }
    }
    return peak;
}

esp_ae_rate_cvt_cfg_t MakeRateCfg(int src_rate, int dest_rate) {
    return esp_ae_rate_cvt_cfg_t{
        .src_rate = static_cast<uint32_t>(src_rate),
        .dest_rate = static_cast<uint32_t>(dest_rate),
        .channel = 1,
        .bits_per_sample = ESP_AE_BIT16,
        .complexity = 2,
        .perf_type = ESP_AE_RATE_CVT_PERF_TYPE_SPEED,
    };
}

}  // namespace

AgoraAudioBridge::~AgoraAudioBridge() {
    Stop();
    if (capture_task_buffer_ != nullptr) {
        heap_caps_free(capture_task_buffer_);
    }
    if (capture_task_stack_ != nullptr) {
        heap_caps_free(capture_task_stack_);
    }
}

void AgoraAudioBridge::CaptureTaskEntry(void* arg) {
    auto* self = static_cast<AgoraAudioBridge*>(arg);
    self->CaptureLoop();
    self->capture_task_handle_ = nullptr;
    vTaskDelete(nullptr);
}

bool AgoraAudioBridge::Start(AudioCodec* codec, AudioService* audio_service, AgoraRtcSession* session,
    int rtc_sample_rate, int pcm_frame_bytes, bool use_processed_input) {
    if (running_) {
        return true;
    }
    if (codec == nullptr || session == nullptr) {
        return false;
    }

    codec_ = codec;
    audio_service_ = audio_service;
    session_ = session;
    rtc_sample_rate_ = rtc_sample_rate;
    pcm_frame_bytes_ = pcm_frame_bytes;
    pcm_samples_per_frame_ = pcm_frame_bytes_ / static_cast<int>(sizeof(int16_t));
    use_processed_input_ = use_processed_input;
    input_sample_rate_ = use_processed_input_ ? 16000 : codec_->input_sample_rate();
    input_passthrough_ = input_sample_rate_ == rtc_sample_rate_;
    output_passthrough_ = codec_->output_sample_rate() == rtc_sample_rate_;
    raw_input_samples_per_frame_ = input_passthrough_
        ? pcm_samples_per_frame_
        : (pcm_samples_per_frame_ * input_sample_rate_ + rtc_sample_rate_ - 1) / rtc_sample_rate_;
    running_ = true;
    outgoing_frame_count_ = 0;
    incoming_frame_count_ = 0;

    ESP_LOGI(kTag,
        "Audio bridge start: in=%dHz out=%dHz rtc=%dHz input_passthrough=%s output_passthrough=%s use_processed_input=%s",
        input_sample_rate_, codec_->output_sample_rate(), rtc_sample_rate_,
        input_passthrough_ ? "true" : "false",
        output_passthrough_ ? "true" : "false",
        use_processed_input_ ? "true" : "false");

    if (!use_processed_input_ && !codec_->input_enabled()) {
        codec_->EnableInput(true);
    }
    if (!codec_->output_enabled()) {
        codec_->EnableOutput(true);
    }
    if (use_processed_input_ && audio_service_ != nullptr) {
        audio_service_->SetProcessedPcmCallback([this](std::vector<int16_t>&& data) {
            SendOutgoingPcm(std::move(data));
        });
    }

    if (!use_processed_input_ && capture_task_stack_ == nullptr) {
        capture_task_stack_ = static_cast<StackType_t*>(
            heap_caps_malloc(kCaptureTaskStackSize * sizeof(StackType_t), MALLOC_CAP_SPIRAM));
    }
    if (!use_processed_input_ && capture_task_buffer_ == nullptr) {
        capture_task_buffer_ = static_cast<StaticTask_t*>(heap_caps_malloc(sizeof(StaticTask_t), MALLOC_CAP_INTERNAL));
    }
    if (!use_processed_input_ && (capture_task_stack_ == nullptr || capture_task_buffer_ == nullptr)) {
        ESP_LOGE(kTag, "Failed to allocate capture task memory, free internal=%u free psram=%u",
            static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
            static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
        running_ = false;
        if (use_processed_input_ && audio_service_ != nullptr) {
            audio_service_->SetProcessedPcmCallback(nullptr);
        }
        return false;
    }

    if (!use_processed_input_) {
        capture_task_handle_ = xTaskCreateStatic(CaptureTaskEntry, "agora_audio_in", kCaptureTaskStackSize,
            this, 5, capture_task_stack_, capture_task_buffer_);
    }
    if (!use_processed_input_ && capture_task_handle_ == nullptr) {
        ESP_LOGE(kTag, "Failed to create capture task, free internal=%u free psram=%u",
            static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
            static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
        running_ = false;
        if (use_processed_input_ && audio_service_ != nullptr) {
            audio_service_->SetProcessedPcmCallback(nullptr);
        }
        return false;
    }
    if (!use_processed_input_) {
        const int capture_channel = codec_->raw_capture_channel_index();
        ESP_LOGI(kTag, "Raw uplink capture: channels=%d selected=%d (%s)",
            codec_->input_channels(), capture_channel,
            codec_->has_hardware_input_reference() ? "configured microphone channel after reference" : "default capture channel");
    }
    return true;
}

void AgoraAudioBridge::Stop() {
    if (!running_) {
        return;
    }

    running_ = false;
    if (use_processed_input_ && audio_service_ != nullptr) {
        audio_service_->SetProcessedPcmCallback(nullptr);
    }

    while (capture_task_handle_ != nullptr) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    if (!use_processed_input_ && codec_ != nullptr && codec_->input_enabled()) {
        codec_->EnableInput(false);
    }

    if (output_resampler_ != nullptr) {
        esp_ae_rate_cvt_close(output_resampler_);
        output_resampler_ = nullptr;
    }
    if (input_resampler_ != nullptr) {
        esp_ae_rate_cvt_close(input_resampler_);
        input_resampler_ = nullptr;
    }
    {
        std::lock_guard<std::mutex> lock(outgoing_mutex_);
        outgoing_buffer_.clear();
    }
}

void AgoraAudioBridge::PushIncomingPcm(const void* data, size_t len) {
    if (!running_ || data == nullptr || len == 0) {
        return;
    }
    ++incoming_frame_count_;

    std::vector<int16_t> pcm(len / sizeof(int16_t));
    memcpy(pcm.data(), data, pcm.size() * sizeof(int16_t));
    if (!codec_->output_enabled()) {
        codec_->EnableOutput(true);
    }

    std::vector<int16_t> output = std::move(pcm);
    {
        std::lock_guard<std::mutex> lock(playback_mutex_);
        if (!output_passthrough_ && EnsureOutputResampler()) {
            uint32_t max_output = 0;
            esp_ae_rate_cvt_get_max_out_sample_num(output_resampler_, output.size(), &max_output);
            std::vector<int16_t> resampled(max_output);
            uint32_t actual_output = max_output;
            esp_ae_rate_cvt_process(output_resampler_, reinterpret_cast<esp_ae_sample_t*>(output.data()),
                output.size(), reinterpret_cast<esp_ae_sample_t*>(resampled.data()), &actual_output);
            resampled.resize(actual_output);
            output = std::move(resampled);
        }
    }
    codec_->OutputData(output);
}

void AgoraAudioBridge::SendOutgoingPcm(std::vector<int16_t>&& mono) {
    if (!running_ || session_ == nullptr || !session_->is_started()) {
        return;
    }

    std::vector<int16_t> samples = std::move(mono);
    if (!input_passthrough_) {
        if (!EnsureInputResampler()) {
            return;
        }

        uint32_t max_output = 0;
        esp_ae_rate_cvt_get_max_out_sample_num(input_resampler_, samples.size(), &max_output);
        std::vector<int16_t> resampled(max_output);
        uint32_t actual_output = max_output;
        esp_ae_rate_cvt_process(input_resampler_, reinterpret_cast<esp_ae_sample_t*>(samples.data()),
            samples.size(), reinterpret_cast<esp_ae_sample_t*>(resampled.data()), &actual_output);
        resampled.resize(actual_output);
        samples = std::move(resampled);
    }

    {
        std::lock_guard<std::mutex> lock(outgoing_mutex_);
        outgoing_buffer_.insert(outgoing_buffer_.end(), samples.begin(), samples.end());
        if (session_->is_in_channel()) {
            FlushOutgoingFramesLocked();
        }
    }
}

void AgoraAudioBridge::FlushOutgoingFramesLocked() {
    while (running_ && session_ != nullptr && session_->is_in_channel() &&
           outgoing_buffer_.size() >= static_cast<size_t>(pcm_samples_per_frame_)) {
        if (!session_->SendAudio(outgoing_buffer_.data(), pcm_samples_per_frame_ * sizeof(int16_t))) {
            break;
        }
        ++outgoing_frame_count_;
        outgoing_buffer_.erase(outgoing_buffer_.begin(),
            outgoing_buffer_.begin() + pcm_samples_per_frame_);
    }
}

void AgoraAudioBridge::CaptureLoop() {
    uint32_t raw_diag_counter = 0;
    while (running_) {
        if (session_ == nullptr || !session_->is_in_channel()) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        std::vector<int16_t> mono;
        if (!codec_->input_enabled()) {
            codec_->EnableInput(true);
        }

        std::vector<int16_t> raw(codec_->input_channels() * raw_input_samples_per_frame_);
        if (!codec_->InputData(raw)) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        if (codec_->input_channels() > 1 && (++raw_diag_counter % 50) == 0) {
            const int capture_channel = codec_->raw_capture_channel_index();
            const int16_t ch0_peak = GetChannelPeakAbs(raw, codec_->input_channels(), 0);
            const int16_t ch1_peak = GetChannelPeakAbs(raw, codec_->input_channels(), 1);
            ESP_LOGI(kTag, "Raw capture level: selected=%d ch0_peak=%d ch1_peak=%d",
                capture_channel, static_cast<int>(ch0_peak), static_cast<int>(ch1_peak));
        }

        if (codec_->input_channels() > 1) {
            const int capture_channel = codec_->raw_capture_channel_index();
            mono.resize(raw_input_samples_per_frame_);
            for (int i = 0, j = capture_channel; i < raw_input_samples_per_frame_ && j < static_cast<int>(raw.size()); ++i, j += codec_->input_channels()) {
                mono[i] = raw[j];
            }
        } else {
            mono = std::move(raw);
        }

        SendOutgoingPcm(std::move(mono));
    }
}

bool AgoraAudioBridge::EnsureInputResampler() {
    if (input_passthrough_) {
        return true;
    }
    if (input_resampler_ != nullptr) {
        return true;
    }
    auto cfg = MakeRateCfg(input_sample_rate_, rtc_sample_rate_);
    return esp_ae_rate_cvt_open(&cfg, &input_resampler_) == ESP_OK && input_resampler_ != nullptr;
}

bool AgoraAudioBridge::EnsureOutputResampler() {
    if (output_passthrough_) {
        return true;
    }
    if (output_resampler_ != nullptr) {
        return true;
    }
    auto cfg = MakeRateCfg(rtc_sample_rate_, codec_->output_sample_rate());
    return esp_ae_rate_cvt_open(&cfg, &output_resampler_) == ESP_OK && output_resampler_ != nullptr;
}
