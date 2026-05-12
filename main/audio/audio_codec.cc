#include "audio_codec.h"
#include "board.h"
#include "settings.h"

#include <esp_log.h>
#include <cstring>
#include <driver/i2s_common.h>

#define TAG "AudioCodec"

AudioCodec::AudioCodec() {
}

AudioCodec::~AudioCodec() {
}

void AudioCodec::OutputData(std::vector<int16_t>& data) {
    std::function<void(const std::vector<int16_t>& data)> callback;
    {
        std::lock_guard<std::mutex> lock(output_tap_mutex_);
        callback = output_tap_callback_;
    }
    if (callback) {
        callback(data);
    }
    Write(data.data(), data.size());
}

bool AudioCodec::InputData(std::vector<int16_t>& data) {
    int samples = Read(data.data(), data.size());
    if (samples > 0) {
        return true;
    }
    return false;
}

void AudioCodec::Start() {
    Settings settings("audio", false);
    output_volume_ = settings.GetInt("output_volume", output_volume_);
    if (output_volume_ <= 0) {
        ESP_LOGW(TAG, "Output volume value (%d) is too small, setting to default (10)", output_volume_);
        output_volume_ = 10;
    }

    ESP_LOGI(TAG, "Audio codec started");
}

std::string AudioCodec::GetAfeInputFormat() const {
    std::string input_format;
    const int near_end_channels = near_end_input_channels();
    const bool has_reference = has_aec_reference();

    if (has_reference && reference_order_ == AudioReferenceOrder::kReferenceBeforeNearEnd) {
        input_format.push_back('R');
    }
    for (int i = 0; i < near_end_channels; ++i) {
        input_format.push_back('M');
    }
    if (has_reference && reference_order_ == AudioReferenceOrder::kReferenceAfterNearEnd) {
        input_format.push_back('R');
    }
    return input_format;
}

void AudioCodec::EnableSoftwareInputReference(bool enable) {
    software_reference_enabled_ = enable && software_reference_supported_ && !input_reference_;
}

void AudioCodec::SetOutputTap(std::function<void(const std::vector<int16_t>& data)> callback) {
    std::lock_guard<std::mutex> lock(output_tap_mutex_);
    output_tap_callback_ = std::move(callback);
}

void AudioCodec::SetOutputVolume(int volume) {
    output_volume_ = volume;
    ESP_LOGI(TAG, "Set output volume to %d", output_volume_);
    
    Settings settings("audio", true);
    settings.SetInt("output_volume", output_volume_);
}

void AudioCodec::SetInputGain(float gain) {
    input_gain_ = gain;
    ESP_LOGI(TAG, "Set input gain to %.1f", input_gain_);
}

void AudioCodec::EnableInput(bool enable) {
    if (enable == input_enabled_) {
        return;
    }
    input_enabled_ = enable;
    ESP_LOGI(TAG, "Set input enable to %s", enable ? "true" : "false");
}

void AudioCodec::EnableOutput(bool enable) {
    if (enable == output_enabled_) {
        return;
    }
    output_enabled_ = enable;
    ESP_LOGI(TAG, "Set output enable to %s", enable ? "true" : "false");
}
