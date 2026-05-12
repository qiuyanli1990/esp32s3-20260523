#ifndef AGORA_AUDIO_BRIDGE_H
#define AGORA_AUDIO_BRIDGE_H

#include <cstdint>
#include <mutex>
#include <vector>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <esp_ae_rate_cvt.h>

#include "agora_rtc_session.h"
#include "audio_codec.h"
#include "audio_service.h"

class Application;

class AgoraAudioBridge {
public:
    AgoraAudioBridge() = default;
    ~AgoraAudioBridge();

    bool Start(AudioCodec* codec, AudioService* audio_service, AgoraRtcSession* session,
        int rtc_sample_rate, int pcm_frame_bytes, bool use_processed_input);
    void Stop();
    void PushIncomingPcm(const void* data, size_t len);
    void PushProcessedPcm(std::vector<int16_t>&& data);

    bool is_running() const { return running_; }

private:
    void SendOutgoingPcm(std::vector<int16_t>&& mono);
    void CaptureLoop();
    static void CaptureTaskEntry(void* arg);
    bool EnsureInputResampler();
    bool EnsureOutputResampler();
    void FlushOutgoingFramesLocked();

    AudioCodec* codec_ = nullptr;
    AudioService* audio_service_ = nullptr;
    AgoraRtcSession* session_ = nullptr;
    int rtc_sample_rate_ = 16000;
    int pcm_frame_bytes_ = 640;
    int pcm_samples_per_frame_ = 320;
    int raw_input_samples_per_frame_ = 320;
    int input_sample_rate_ = 16000;
    bool use_processed_input_ = false;
    bool input_passthrough_ = false;
    bool output_passthrough_ = false;
    TaskHandle_t capture_task_handle_ = nullptr;
    StaticTask_t* capture_task_buffer_ = nullptr;
    StackType_t* capture_task_stack_ = nullptr;
    std::mutex playback_mutex_;
    std::mutex outgoing_mutex_;
    std::vector<int16_t> outgoing_buffer_;
    uint32_t outgoing_frame_count_ = 0;
    uint32_t incoming_frame_count_ = 0;
    esp_ae_rate_cvt_handle_t input_resampler_ = nullptr;
    esp_ae_rate_cvt_handle_t output_resampler_ = nullptr;
    bool running_ = false;
};

#endif  // AGORA_AUDIO_BRIDGE_H
