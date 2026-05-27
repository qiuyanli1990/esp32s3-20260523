#ifndef REALTIME_AUDIO_SERVICE_H
#define REALTIME_AUDIO_SERVICE_H

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <vector>
#include <freertos/FreeRTOS.h>
#include <freertos/stream_buffer.h>
#include <freertos/task.h>

#include <esp_ae_rate_cvt.h>

#include "realtime_audio_port.h"
#include "realtime_audio_config.h"
#include "audio_service.h"

class RealtimeAudioService {
public:
    using TransportReadyCallback = std::function<bool()>;
    using SendAudioCallback = std::function<bool(const int16_t* data, size_t bytes)>;

    RealtimeAudioService() = default;
    ~RealtimeAudioService();

    bool Start(RealtimeAudioPort* audio_port, AudioService* audio_service,
        int rtc_sample_rate, int pcm_frame_bytes, bool use_processed_input,
        TransportReadyCallback transport_ready, SendAudioCallback send_audio);
    void Stop();
    void PushIncomingPcm(const void* data, size_t len);

    bool is_running() const { return running_; }

private:
    void SendOutgoingPcm(std::vector<int16_t>&& mono);
    void CaptureLoop();
    void PlaybackLoop();
    static void CaptureTaskEntry(void* arg);
    static void PlaybackTaskEntry(void* arg);
    bool EnsureInputResampler();
    bool EnsureOutputResampler();
    bool QueuePlayback(std::vector<int16_t>&& pcm);
    bool QueuePlaybackBytes(const void* data, size_t len);
    bool PopPlayback(std::vector<int16_t>& pcm);
    size_t PlaybackQueueSize();
    bool UsePlaybackStream() const;
    void ResetPlaybackStreamStats();
    void NotePlaybackUnderflow();
    StackType_t* AllocateTaskStack(uint32_t stack_size) const;
    void FlushOutgoingFramesLocked();
    bool TransportReady() const;

    RealtimeAudioPort* audio_port_ = nullptr;
    AudioService* audio_service_ = nullptr;
    TransportReadyCallback transport_ready_;
    SendAudioCallback send_audio_;
    RealtimeAudioConfig realtime_config_;
    int rtc_sample_rate_ = 16000;
    int pcm_frame_bytes_ = 640;
    int pcm_samples_per_frame_ = 320;
    int raw_input_samples_per_frame_ = 320;
    int input_sample_rate_ = 16000;
    bool use_processed_input_ = false;
    bool input_passthrough_ = false;
    bool output_passthrough_ = false;
    TaskHandle_t capture_task_handle_ = nullptr;
    TaskHandle_t playback_task_handle_ = nullptr;
    StaticTask_t* capture_task_buffer_ = nullptr;
    StaticTask_t* playback_task_buffer_ = nullptr;
    StackType_t* capture_task_stack_ = nullptr;
    StackType_t* playback_task_stack_ = nullptr;
    std::mutex playback_mutex_;
    std::mutex playback_queue_mutex_;
    std::deque<std::vector<int16_t>> playback_queue_;
    StreamBufferHandle_t playback_stream_ = nullptr;
    uint32_t stream_rx_frames_ = 0;
    uint32_t stream_rx_bytes_ = 0;
    uint32_t stream_rx_partial_frames_ = 0;
    uint32_t stream_rx_gap_over_30ms_ = 0;
    uint32_t stream_rx_max_gap_ms_ = 0;
    uint32_t stream_queue_overflows_ = 0;
    uint32_t stream_queue_partial_sends_ = 0;
    uint32_t stream_latency_trims_ = 0;
    uint32_t stream_latency_trim_bytes_ = 0;
    uint32_t stream_play_frames_ = 0;
    uint32_t stream_play_zero_reads_ = 0;
    uint32_t stream_play_partial_reads_ = 0;
    uint32_t stream_play_write_slow_ = 0;
    uint32_t stream_play_max_write_ms_ = 0;
    uint32_t stream_play_underflows_ = 0;
    int64_t stream_last_rx_us_ = 0;
    int64_t stream_last_rx_log_us_ = 0;
    int64_t stream_last_play_log_us_ = 0;
    std::mutex outgoing_mutex_;
    std::vector<int16_t> outgoing_buffer_;
    esp_ae_rate_cvt_handle_t input_resampler_ = nullptr;
    esp_ae_rate_cvt_handle_t output_resampler_ = nullptr;
    bool running_ = false;
};

#endif  // REALTIME_AUDIO_SERVICE_H
