#include "realtime_audio_service.h"

#include <algorithm>
#include <cstring>

#include <esp_timer.h>
#include <esp_heap_caps.h>
#include <esp_log.h>

#include "sdkconfig.h"

namespace {

constexpr char kTag[] = "RealtimeAudio";

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

RealtimeAudioService::~RealtimeAudioService() {
    Stop();
    if (capture_task_buffer_ != nullptr) {
        heap_caps_free(capture_task_buffer_);
    }
    if (capture_task_stack_ != nullptr) {
        heap_caps_free(capture_task_stack_);
    }
    if (playback_task_buffer_ != nullptr) {
        heap_caps_free(playback_task_buffer_);
    }
    if (playback_task_stack_ != nullptr) {
        heap_caps_free(playback_task_stack_);
    }
    if (playback_stream_ != nullptr) {
        vStreamBufferDelete(playback_stream_);
        playback_stream_ = nullptr;
    }
}

void RealtimeAudioService::CaptureTaskEntry(void* arg) {
    auto* self = static_cast<RealtimeAudioService*>(arg);
    self->CaptureLoop();
    self->capture_task_handle_ = nullptr;
    vTaskDelete(nullptr);
}

void RealtimeAudioService::PlaybackTaskEntry(void* arg) {
    auto* self = static_cast<RealtimeAudioService*>(arg);
    self->PlaybackLoop();
    self->playback_task_handle_ = nullptr;
    vTaskDelete(nullptr);
}

StackType_t* RealtimeAudioService::AllocateTaskStack(uint32_t stack_size) const {
    const uint32_t preferred_caps = realtime_config_.prefer_spiram_task_stack
        ? (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
        : (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    auto* stack = static_cast<StackType_t*>(
        heap_caps_malloc(stack_size * sizeof(StackType_t), preferred_caps));
    if (stack == nullptr && !realtime_config_.prefer_spiram_task_stack) {
        stack = static_cast<StackType_t*>(
            heap_caps_malloc(stack_size * sizeof(StackType_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    }
    return stack;
}

bool RealtimeAudioService::UsePlaybackStream() const {
    return output_passthrough_
        && realtime_config_.playback_buffer_mode == RealtimePlaybackBufferMode::kByteStream;
}

void RealtimeAudioService::ResetPlaybackStreamStats() {
    stream_rx_frames_ = 0;
    stream_rx_bytes_ = 0;
    stream_rx_partial_frames_ = 0;
    stream_rx_gap_over_30ms_ = 0;
    stream_rx_max_gap_ms_ = 0;
    stream_queue_overflows_ = 0;
    stream_queue_partial_sends_ = 0;
    stream_latency_trims_ = 0;
    stream_latency_trim_bytes_ = 0;
    stream_play_frames_ = 0;
    stream_play_zero_reads_ = 0;
    stream_play_partial_reads_ = 0;
    stream_play_write_slow_ = 0;
    stream_play_max_write_ms_ = 0;
    stream_play_underflows_ = 0;
    stream_last_rx_us_ = 0;
    stream_last_rx_log_us_ = 0;
    stream_last_play_log_us_ = 0;
}

void RealtimeAudioService::NotePlaybackUnderflow() {
    ++stream_play_underflows_;
}

bool RealtimeAudioService::Start(RealtimeAudioPort* audio_port, AudioService* audio_service,
    int rtc_sample_rate, int pcm_frame_bytes, bool use_processed_input,
    TransportReadyCallback transport_ready, SendAudioCallback send_audio) {
    if (running_) {
        return true;
    }
    if (audio_port == nullptr || !send_audio) {
        return false;
    }

    audio_port_ = audio_port;
    audio_service_ = audio_service;
    transport_ready_ = std::move(transport_ready);
    send_audio_ = std::move(send_audio);
    rtc_sample_rate_ = rtc_sample_rate;
    pcm_frame_bytes_ = pcm_frame_bytes;
    pcm_samples_per_frame_ = pcm_frame_bytes_ / static_cast<int>(sizeof(int16_t));
    use_processed_input_ = use_processed_input;
    realtime_config_ = audio_port_->GetRealtimeAudioConfig();
    if (realtime_config_.capture_task_stack_size == 0) {
        realtime_config_.capture_task_stack_size = RealtimeAudioConfig{}.capture_task_stack_size;
    }
    if (realtime_config_.playback_task_stack_size == 0) {
        realtime_config_.playback_task_stack_size = RealtimeAudioConfig{}.playback_task_stack_size;
    }
    if (realtime_config_.max_playback_queue_frames == 0) {
        realtime_config_.max_playback_queue_frames = 1;
    }
    if (realtime_config_.playback_prebuffer_frames > realtime_config_.max_playback_queue_frames) {
        realtime_config_.playback_prebuffer_frames = realtime_config_.max_playback_queue_frames;
    }
    if (realtime_config_.playback_latency_high_water_frames >
        realtime_config_.max_playback_queue_frames) {
        realtime_config_.playback_latency_high_water_frames =
            realtime_config_.max_playback_queue_frames;
    }
    if (realtime_config_.playback_latency_target_frames >
        realtime_config_.playback_latency_high_water_frames) {
        realtime_config_.playback_latency_target_frames =
            realtime_config_.playback_latency_high_water_frames;
    }
    input_sample_rate_ = use_processed_input_ ? 16000 : audio_port_->input_sample_rate();
    input_passthrough_ = input_sample_rate_ == rtc_sample_rate_;
    output_passthrough_ = audio_port_->output_sample_rate() == rtc_sample_rate_;
    raw_input_samples_per_frame_ = input_passthrough_
        ? pcm_samples_per_frame_
        : (pcm_samples_per_frame_ * input_sample_rate_ + rtc_sample_rate_ - 1) / rtc_sample_rate_;
    running_ = true;
    ResetPlaybackStreamStats();
    {
        std::lock_guard<std::mutex> lock(playback_queue_mutex_);
        playback_queue_.clear();
    }

    if (UsePlaybackStream()) {
        if (playback_stream_ == nullptr) {
            playback_stream_ = xStreamBufferCreateWithCaps(
                static_cast<size_t>(pcm_frame_bytes_) * realtime_config_.max_playback_queue_frames + 1,
                sizeof(int16_t),
                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (playback_stream_ == nullptr) {
                ESP_LOGE(kTag, "Failed to create playback stream, free internal=%u free psram=%u",
                    static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
                    static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
                running_ = false;
                return false;
            }
        } else {
            xStreamBufferReset(playback_stream_);
        }
    } else if (playback_stream_ != nullptr) {
        xStreamBufferReset(playback_stream_);
    }

    ESP_LOGI(kTag,
        "Realtime audio start: profile=%s in=%dHz out=%dHz rtc=%dHz input_passthrough=%s output_passthrough=%s use_processed_input=%s buffer=%s",
        realtime_config_.profile_name != nullptr ? realtime_config_.profile_name : "unknown",
        input_sample_rate_, audio_port_->output_sample_rate(), rtc_sample_rate_,
        input_passthrough_ ? "true" : "false",
        output_passthrough_ ? "true" : "false",
        use_processed_input_ ? "true" : "false",
        UsePlaybackStream() ? "byte-stream" : "frame-queue");

    if (!use_processed_input_ && !audio_port_->input_enabled()) {
        audio_port_->EnableInput(true);
    }
    if (!audio_port_->output_enabled()) {
        audio_port_->EnableOutput(true);
    }
    if (use_processed_input_ && audio_service_ != nullptr) {
        audio_service_->SetProcessedPcmCallback([this](std::vector<int16_t>&& data) {
            SendOutgoingPcm(std::move(data));
        });
    }

    if (playback_task_stack_ == nullptr) {
        playback_task_stack_ = AllocateTaskStack(realtime_config_.playback_task_stack_size);
    }
    if (playback_task_buffer_ == nullptr) {
        playback_task_buffer_ = static_cast<StaticTask_t*>(heap_caps_malloc(sizeof(StaticTask_t), MALLOC_CAP_INTERNAL));
    }
    if (playback_task_stack_ == nullptr || playback_task_buffer_ == nullptr) {
        ESP_LOGE(kTag, "Failed to allocate playback task memory, free internal=%u free psram=%u",
            static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
            static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
        running_ = false;
        if (use_processed_input_ && audio_service_ != nullptr) {
            audio_service_->SetProcessedPcmCallback(nullptr);
        }
        return false;
    }
    playback_task_handle_ = xTaskCreateStaticPinnedToCore(PlaybackTaskEntry, "agora_audio_out",
        realtime_config_.playback_task_stack_size, this, realtime_config_.playback_task_priority,
        playback_task_stack_, playback_task_buffer_, realtime_config_.playback_task_core);
    if (playback_task_handle_ == nullptr) {
        ESP_LOGE(kTag, "Failed to create playback task, free internal=%u free psram=%u",
            static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
            static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
        running_ = false;
        if (use_processed_input_ && audio_service_ != nullptr) {
            audio_service_->SetProcessedPcmCallback(nullptr);
        }
        return false;
    }

    bool needs_capture_task = !use_processed_input_;

    if (needs_capture_task && capture_task_stack_ == nullptr) {
        capture_task_stack_ = AllocateTaskStack(realtime_config_.capture_task_stack_size);
    }
    if (needs_capture_task && capture_task_buffer_ == nullptr) {
        capture_task_buffer_ = static_cast<StaticTask_t*>(heap_caps_malloc(sizeof(StaticTask_t), MALLOC_CAP_INTERNAL));
    }
    if (needs_capture_task && (capture_task_stack_ == nullptr || capture_task_buffer_ == nullptr)) {
        ESP_LOGE(kTag, "Failed to allocate capture task memory, free internal=%u free psram=%u",
            static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
            static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
        running_ = false;
        if (playback_task_handle_ != nullptr) {
            xTaskNotifyGive(playback_task_handle_);
        }
        while (playback_task_handle_ != nullptr) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        if (use_processed_input_ && audio_service_ != nullptr) {
            audio_service_->SetProcessedPcmCallback(nullptr);
        }
        return false;
    }
    ESP_LOGI(kTag,
        "Realtime playback task created: priority=%u core=%d queue=%u prebuffer=%u high_water=%u target=%u mode=%s",
        static_cast<unsigned>(realtime_config_.playback_task_priority),
        static_cast<int>(realtime_config_.playback_task_core),
        static_cast<unsigned>(realtime_config_.max_playback_queue_frames),
        static_cast<unsigned>(realtime_config_.playback_prebuffer_frames),
        static_cast<unsigned>(realtime_config_.playback_latency_high_water_frames),
        static_cast<unsigned>(realtime_config_.playback_latency_target_frames),
        UsePlaybackStream() ? "byte-stream" : "frame-queue");

    if (needs_capture_task) {
        if (realtime_config_.pin_capture_task) {
            capture_task_handle_ = xTaskCreateStaticPinnedToCore(CaptureTaskEntry, "agora_audio_in",
                realtime_config_.capture_task_stack_size, this, realtime_config_.capture_task_priority,
                capture_task_stack_, capture_task_buffer_, realtime_config_.capture_task_core);
        } else {
            capture_task_handle_ = xTaskCreateStatic(CaptureTaskEntry, "agora_audio_in",
                realtime_config_.capture_task_stack_size, this, realtime_config_.capture_task_priority,
                capture_task_stack_, capture_task_buffer_);
        }
    }
    if (needs_capture_task && capture_task_handle_ == nullptr) {
        ESP_LOGE(kTag, "Failed to create capture task, free internal=%u free psram=%u",
            static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
            static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
        running_ = false;
        if (playback_task_handle_ != nullptr) {
            xTaskNotifyGive(playback_task_handle_);
        }
        while (playback_task_handle_ != nullptr) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        if (use_processed_input_ && audio_service_ != nullptr) {
            audio_service_->SetProcessedPcmCallback(nullptr);
        }
        return false;
    }
    if (needs_capture_task) {
        const int capture_channel = audio_port_->raw_capture_channel_index();
        ESP_LOGI(kTag, "Raw uplink capture: channels=%d selected=%d format=%s (%s)",
            audio_port_->input_channels(), capture_channel,
            audio_port_->GetAfeInputFormat().c_str(),
            audio_port_->has_hardware_input_reference() ? "configured microphone channel after reference" : "default capture channel");
    }
    return true;
}

void RealtimeAudioService::Stop() {
    if (!running_) {
        transport_ready_ = nullptr;
        send_audio_ = nullptr;
        return;
    }

    running_ = false;
    if (use_processed_input_ && audio_service_ != nullptr) {
        audio_service_->SetProcessedPcmCallback(nullptr);
    }

    while (capture_task_handle_ != nullptr) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (playback_task_handle_ != nullptr) {
        xTaskNotifyGive(playback_task_handle_);
    }
    while (playback_task_handle_ != nullptr) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    bool should_disable_input = !use_processed_input_;
    if (should_disable_input && audio_port_ != nullptr && audio_port_->input_enabled()) {
        audio_port_->EnableInput(false);
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
    {
        std::lock_guard<std::mutex> lock(playback_queue_mutex_);
        playback_queue_.clear();
    }
    if (playback_stream_ != nullptr) {
        xStreamBufferReset(playback_stream_);
    }
    transport_ready_ = nullptr;
    send_audio_ = nullptr;
}

void RealtimeAudioService::PushIncomingPcm(const void* data, size_t len) {
    if (!running_ || data == nullptr || len == 0) {
        return;
    }

    if (!audio_port_->output_enabled()) {
        audio_port_->EnableOutput(true);
    }

    if (UsePlaybackStream()) {
        const int64_t now_us = esp_timer_get_time();
        if (stream_last_rx_us_ != 0) {
            const uint32_t gap_ms = static_cast<uint32_t>((now_us - stream_last_rx_us_) / 1000);
            if (gap_ms > 30) {
                ++stream_rx_gap_over_30ms_;
            }
            if (gap_ms > stream_rx_max_gap_ms_) {
                stream_rx_max_gap_ms_ = gap_ms;
            }
        }
        stream_last_rx_us_ = now_us;
        ++stream_rx_frames_;
        stream_rx_bytes_ += static_cast<uint32_t>(len);
        if (len != static_cast<size_t>(pcm_frame_bytes_)) {
            ++stream_rx_partial_frames_;
        }
        if (now_us - stream_last_rx_log_us_ >= 3000000) {
            stream_last_rx_log_us_ = now_us;
            const size_t stream_bytes = playback_stream_ != nullptr ? xStreamBufferBytesAvailable(playback_stream_) : 0;
            const size_t stream_space = playback_stream_ != nullptr ? xStreamBufferSpacesAvailable(playback_stream_) : 0;
            ESP_LOGI(kTag,
                "Realtime downlink stream stats: rx_frames=%lu rx_bytes=%lu partial_rx=%lu gap_over30=%lu max_gap_ms=%lu stream_bytes=%u stream_space=%u trims=%lu trim_bytes=%lu",
                static_cast<unsigned long>(stream_rx_frames_),
                static_cast<unsigned long>(stream_rx_bytes_),
                static_cast<unsigned long>(stream_rx_partial_frames_),
                static_cast<unsigned long>(stream_rx_gap_over_30ms_),
                static_cast<unsigned long>(stream_rx_max_gap_ms_),
                static_cast<unsigned>(stream_bytes),
                static_cast<unsigned>(stream_space),
                static_cast<unsigned long>(stream_latency_trims_),
                static_cast<unsigned long>(stream_latency_trim_bytes_));
        }
        QueuePlaybackBytes(data, len);
        return;
    }

    std::vector<int16_t> pcm(len / sizeof(int16_t));
    memcpy(pcm.data(), data, pcm.size() * sizeof(int16_t));
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
    QueuePlayback(std::move(output));
}

bool RealtimeAudioService::QueuePlayback(std::vector<int16_t>&& pcm) {
    if (pcm.empty()) {
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(playback_queue_mutex_);
        while (playback_queue_.size() >= realtime_config_.max_playback_queue_frames) {
            playback_queue_.pop_front();
        }
        playback_queue_.push_back(std::move(pcm));
    }
    if (playback_task_handle_ != nullptr) {
        xTaskNotifyGive(playback_task_handle_);
    }
    return true;
}

bool RealtimeAudioService::QueuePlaybackBytes(const void* data, size_t len) {
    if (playback_stream_ == nullptr || data == nullptr || len == 0) {
        return false;
    }

    const size_t frame_bytes = pcm_frame_bytes_ > 0 ? static_cast<size_t>(pcm_frame_bytes_) : len;
    const uint8_t* bytes = static_cast<const uint8_t*>(data);
    size_t offset = 0;
    size_t total_sent = 0;

    auto drop_oldest_bytes = [this](size_t bytes_to_drop) {
        uint8_t discard[320];
        size_t total_dropped = 0;
        while (bytes_to_drop > 0) {
            const size_t chunk = bytes_to_drop > sizeof(discard) ? sizeof(discard) : bytes_to_drop;
            const size_t dropped = xStreamBufferReceive(playback_stream_, discard, chunk, 0);
            if (dropped == 0) {
                break;
            }
            total_dropped += dropped;
            bytes_to_drop -= dropped;
        }
        return total_dropped;
    };

    while (offset < len) {
        const size_t chunk_len = std::min(frame_bytes, len - offset);
        size_t high_water_frames = realtime_config_.playback_latency_high_water_frames;
        size_t target_frames = realtime_config_.playback_latency_target_frames;
        const size_t high_water_bytes = high_water_frames * frame_bytes;
        const size_t target_bytes = target_frames * frame_bytes;
        if (high_water_bytes > 0 && target_bytes > 0) {
            size_t buffered = xStreamBufferBytesAvailable(playback_stream_);
            if (buffered + chunk_len > high_water_bytes) {
                size_t bytes_to_drop = buffered + chunk_len - target_bytes;
                bytes_to_drop = std::min(bytes_to_drop, buffered);
                bytes_to_drop = (bytes_to_drop / frame_bytes) * frame_bytes;
                if (bytes_to_drop > 0) {
                    const size_t dropped = drop_oldest_bytes(bytes_to_drop);
                    if (dropped > 0) {
                        ++stream_latency_trims_;
                        stream_latency_trim_bytes_ += static_cast<uint32_t>(dropped);
                    }
                }
            }
        }
        size_t space = xStreamBufferSpacesAvailable(playback_stream_);
        if (space < chunk_len) {
            const size_t bytes_needed = chunk_len - space;
            const size_t drop_bytes = ((bytes_needed + frame_bytes - 1) / frame_bytes) * frame_bytes;
            drop_oldest_bytes(drop_bytes);
            ++stream_queue_overflows_;
            space = xStreamBufferSpacesAvailable(playback_stream_);
        }

        if (space < chunk_len) {
            ++stream_queue_partial_sends_;
            break;
        }

        const size_t sent = xStreamBufferSend(playback_stream_, bytes + offset, chunk_len, 0);
        if (sent != chunk_len) {
            ++stream_queue_partial_sends_;
            break;
        }
        total_sent += sent;
        offset += sent;
    }

    if (playback_task_handle_ != nullptr) {
        xTaskNotifyGive(playback_task_handle_);
    }
    return total_sent > 0;
}

bool RealtimeAudioService::PopPlayback(std::vector<int16_t>& pcm) {
    std::lock_guard<std::mutex> lock(playback_queue_mutex_);
    if (playback_queue_.empty()) {
        return false;
    }
    pcm = std::move(playback_queue_.front());
    playback_queue_.pop_front();
    return true;
}

size_t RealtimeAudioService::PlaybackQueueSize() {
    std::lock_guard<std::mutex> lock(playback_queue_mutex_);
    return playback_queue_.size();
}

void RealtimeAudioService::SendOutgoingPcm(std::vector<int16_t>&& mono) {
    if (!running_ || !send_audio_ || !TransportReady()) {
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
        if (TransportReady()) {
            FlushOutgoingFramesLocked();
        }
    }
}

void RealtimeAudioService::FlushOutgoingFramesLocked() {
    while (running_ && TransportReady() &&
           outgoing_buffer_.size() >= static_cast<size_t>(pcm_samples_per_frame_)) {
        if (!send_audio_(outgoing_buffer_.data(), pcm_samples_per_frame_ * sizeof(int16_t))) {
            break;
        }
        outgoing_buffer_.erase(outgoing_buffer_.begin(),
            outgoing_buffer_.begin() + pcm_samples_per_frame_);
    }
}

bool RealtimeAudioService::TransportReady() const {
    return !transport_ready_ || transport_ready_();
}

void RealtimeAudioService::CaptureLoop() {
    while (running_) {
        if (!TransportReady()) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        std::vector<int16_t> mono;
        if (!audio_port_->input_enabled()) {
            audio_port_->EnableInput(true);
        }

        std::vector<int16_t> raw(audio_port_->input_channels() * raw_input_samples_per_frame_);
        if (!audio_port_->InputData(raw)) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        if (audio_port_->input_channels() > 1) {
            const int capture_channel = audio_port_->raw_capture_channel_index();
            mono.resize(raw_input_samples_per_frame_);
            for (int i = 0, j = capture_channel; i < raw_input_samples_per_frame_ && j < static_cast<int>(raw.size()); ++i, j += audio_port_->input_channels()) {
                mono[i] = raw[j];
            }
        } else {
            mono = std::move(raw);
        }

        SendOutgoingPcm(std::move(mono));
    }
}

void RealtimeAudioService::PlaybackLoop() {
    ESP_LOGI(kTag, "Realtime playback task started");
    if (UsePlaybackStream() && playback_stream_ != nullptr) {
        const size_t frame_bytes = static_cast<size_t>(pcm_frame_bytes_);
        const size_t frame_samples = frame_bytes / sizeof(int16_t);
        std::vector<int16_t> output(frame_samples);
        auto* output_bytes = reinterpret_cast<uint8_t*>(output.data());
        size_t filled = 0;
        bool primed = (realtime_config_.playback_prebuffer_frames == 0);
        while (running_) {
            const size_t prebuffer_frames = realtime_config_.playback_prebuffer_frames;
            const size_t prebuffer_bytes = frame_bytes * prebuffer_frames;
            if (!primed) {
                if (xStreamBufferBytesAvailable(playback_stream_) < prebuffer_bytes) {
                    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(20));
                    continue;
                }
                primed = true;
            }

            while (running_ && filled < frame_bytes) {
                const size_t got = xStreamBufferReceive(playback_stream_, output_bytes + filled,
                    frame_bytes - filled, pdMS_TO_TICKS(20));
                if (got == 0) {
                    ++stream_play_zero_reads_;
                    NotePlaybackUnderflow();
                    if (filled > 0) {
                        ++stream_play_partial_reads_;
                    }
                    break;
                }
                filled += got;
            }
            if (!running_ || filled < frame_bytes) {
                if (filled == 0 && prebuffer_bytes > 0) {
                    primed = false;
                }
                continue;
            }
            if (audio_port_ == nullptr) {
                filled = 0;
                continue;
            }
            if (!audio_port_->output_enabled()) {
                audio_port_->EnableOutput(true);
            }
            const int64_t write_start_us = esp_timer_get_time();
            audio_port_->OutputData(output);
            const uint32_t write_ms = static_cast<uint32_t>((esp_timer_get_time() - write_start_us) / 1000);
            filled = 0;
            ++stream_play_frames_;
            if (write_ms > 30) {
                ++stream_play_write_slow_;
            }
            if (write_ms > stream_play_max_write_ms_) {
                stream_play_max_write_ms_ = write_ms;
            }
            const int64_t now_us = esp_timer_get_time();
            if (now_us - stream_last_play_log_us_ >= 3000000) {
                stream_last_play_log_us_ = now_us;
                ESP_LOGI(kTag,
                    "Realtime playback stream stats: out_frames=%lu zero_reads=%lu partial_reads=%lu underflows=%lu queue_overflows=%lu partial_sends=%lu stream_bytes=%u prebuffer_frames=%u prebuffer_bytes=%u write_slow=%lu max_write_ms=%lu",
                    static_cast<unsigned long>(stream_play_frames_),
                    static_cast<unsigned long>(stream_play_zero_reads_),
                    static_cast<unsigned long>(stream_play_partial_reads_),
                    static_cast<unsigned long>(stream_play_underflows_),
                    static_cast<unsigned long>(stream_queue_overflows_),
                    static_cast<unsigned long>(stream_queue_partial_sends_),
                    static_cast<unsigned>(xStreamBufferBytesAvailable(playback_stream_)),
                    static_cast<unsigned>(prebuffer_frames),
                    static_cast<unsigned>(prebuffer_bytes),
                    static_cast<unsigned long>(stream_play_write_slow_),
                    static_cast<unsigned long>(stream_play_max_write_ms_));
            }
        }
        ESP_LOGI(kTag, "Realtime playback task stopped");
        return;
    }
    bool primed = false;
    while (running_) {
        if (!primed) {
            if (PlaybackQueueSize() < realtime_config_.playback_prebuffer_frames) {
                ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(20));
                continue;
            }
            primed = true;
        }

        std::vector<int16_t> output;
        if (!PopPlayback(output)) {
            primed = false;
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(20));
            continue;
        }
        if (audio_port_ == nullptr || output.empty()) {
            continue;
        }
        if (!audio_port_->output_enabled()) {
            audio_port_->EnableOutput(true);
        }
        audio_port_->OutputData(output);
    }
    ESP_LOGI(kTag, "Realtime playback task stopped");
}

bool RealtimeAudioService::EnsureInputResampler() {
    if (input_passthrough_) {
        return true;
    }
    if (input_resampler_ != nullptr) {
        return true;
    }
    auto cfg = MakeRateCfg(input_sample_rate_, rtc_sample_rate_);
    return esp_ae_rate_cvt_open(&cfg, &input_resampler_) == ESP_OK && input_resampler_ != nullptr;
}

bool RealtimeAudioService::EnsureOutputResampler() {
    if (output_passthrough_) {
        return true;
    }
    if (output_resampler_ != nullptr) {
        return true;
    }
    auto cfg = MakeRateCfg(rtc_sample_rate_, audio_port_->output_sample_rate());
    return esp_ae_rate_cvt_open(&cfg, &output_resampler_) == ESP_OK && output_resampler_ != nullptr;
}
