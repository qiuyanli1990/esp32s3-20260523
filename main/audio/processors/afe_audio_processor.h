#ifndef AFE_AUDIO_PROCESSOR_H
#define AFE_AUDIO_PROCESSOR_H

#include <esp_afe_sr_models.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/event_groups.h>
#include <freertos/stream_buffer.h>

#include <cstdint>
#include <string>
#include <vector>
#include <functional>
#include <mutex>

#include "audio_processor.h"
#include "audio_codec.h"

class AfeAudioProcessor : public AudioProcessor {
public:
    AfeAudioProcessor();
    ~AfeAudioProcessor();

    bool Initialize(AudioCodec* codec, int frame_duration_ms, srmodel_list_t* models_list) override;
    void Feed(std::vector<int16_t>&& data) override;
    void FeedReference(std::vector<int16_t>&& data) override;
    void FeedReference(const int16_t* data, size_t samples) override;
    void Start() override;
    void Stop() override;
    bool IsRunning() override;
    bool WantsSpeakerReference() const override;
    void OnOutput(std::function<void(std::vector<int16_t>&& data)> callback) override;
    void OnVadStateChange(std::function<void(bool speaking)> callback) override;
    size_t GetFeedSize() override;
    bool EnableDeviceAec(bool enable) override;

private:
    EventGroupHandle_t event_group_ = nullptr;
    TaskHandle_t processor_task_handle_ = nullptr;
    StaticTask_t* processor_task_buffer_ = nullptr;
    StackType_t* processor_task_stack_ = nullptr;
    const esp_afe_sr_iface_t* afe_iface_ = nullptr;
    esp_afe_sr_data_t* afe_data_ = nullptr;
    std::function<void(std::vector<int16_t>&& data)> output_callback_;
    std::function<void(bool speaking)> vad_state_change_callback_;
    AudioCodec* codec_ = nullptr;
    int frame_samples_ = 0;
    bool is_speaking_ = false;
    std::vector<int16_t> input_buffer_;
    StreamBufferHandle_t reference_stream_ = nullptr;
    std::vector<int> near_end_channel_indices_;
    std::vector<int16_t> software_reference_chunk_;
    std::vector<int16_t> software_afe_input_;
    std::mutex input_buffer_mutex_;
    std::vector<int16_t> output_buffer_;
    bool initialized_ = false;
    int64_t last_reference_feed_us_ = 0;
    int64_t software_reference_log_us_ = 0;
    uint32_t software_reference_wait_count_ = 0;
    uint32_t software_reference_zero_count_ = 0;

    void AudioProcessorTask();
    void FeedHardwareInputFramesLocked(size_t feed_size);
    void FeedSoftwareReferenceFramesLocked(size_t feed_size);
};

#endif 
