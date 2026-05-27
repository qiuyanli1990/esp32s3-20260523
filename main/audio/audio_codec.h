#ifndef _AUDIO_CODEC_H
#define _AUDIO_CODEC_H

#include "sdkconfig.h"

#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <driver/i2s_std.h>

#include <vector>
#include <string>
#include <functional>
#include <mutex>

#include "board.h"
#include "realtime_audio_config.h"

#define AUDIO_CODEC_DMA_DESC_NUM 6
#define AUDIO_CODEC_DMA_FRAME_NUM 240

enum class AudioReferenceOrder {
    kReferenceBeforeNearEnd,
    kReferenceAfterNearEnd,
};

class AudioCodec {
public:
    AudioCodec();
    virtual ~AudioCodec();
    
    virtual void SetOutputVolume(int volume);
    virtual void SetInputGain(float gain);
    virtual void EnableInput(bool enable);
    virtual void EnableOutput(bool enable);

    virtual void OutputData(std::vector<int16_t>& data);
    virtual bool InputData(std::vector<int16_t>& data);
    virtual void Start();

    inline bool duplex() const { return duplex_; }
    inline bool has_hardware_input_reference() const { return input_reference_; }
    inline int input_sample_rate() const { return input_sample_rate_; }
    inline int output_sample_rate() const { return output_sample_rate_; }
    inline int input_channels() const { return input_channels_; }
    inline int output_channels() const { return output_channels_; }
    inline int output_volume() const { return output_volume_; }
    inline float input_gain() const { return input_gain_; }
    inline bool input_enabled() const { return input_enabled_; }
    inline bool output_enabled() const { return output_enabled_; }
    inline bool software_reference_supported() const { return software_reference_supported_; }
    inline bool using_software_input_reference() const {
        return !input_reference_ && software_reference_supported_ && software_reference_enabled_;
    }
    inline bool has_aec_reference() const {
        return has_hardware_input_reference() || using_software_input_reference();
    }
    inline bool supports_device_aec() const {
        return duplex_ && (has_hardware_input_reference() || software_reference_supported_);
    }
    inline int near_end_input_channels() const {
        if (!afe_input_format_override_.empty()) {
            int channels = 0;
            for (char channel : afe_input_format_override_) {
                if (channel == 'M') {
                    ++channels;
                }
            }
            return channels > 0 ? channels : 1;
        }
        int channels = input_channels_ - (has_hardware_input_reference() ? 1 : 0);
        return channels > 0 ? channels : 1;
    }
    inline int afe_input_channels() const {
        if (using_software_input_reference()) {
            return near_end_input_channels() + 1;
        }
        if (!afe_input_format_override_.empty()) {
            return static_cast<int>(afe_input_format_override_.size());
        }
        return near_end_input_channels() + (has_aec_reference() ? 1 : 0);
    }
    inline AudioReferenceOrder reference_order() const { return reference_order_; }
    inline int raw_capture_channel_index() const {
        if (!afe_input_format_override_.empty()) {
            for (int i = 0; i < static_cast<int>(afe_input_format_override_.size()); ++i) {
                if (afe_input_format_override_[i] == 'M') {
                    return i;
                }
            }
        }
        if (input_channels_ <= 1) {
            return 0;
        }
        if (has_hardware_input_reference() && reference_order_ == AudioReferenceOrder::kReferenceBeforeNearEnd) {
            return 1;
        }
        return 0;
    }

    std::string GetAfeInputFormat() const;
    std::vector<int> GetNearEndChannelIndices() const;
    virtual RealtimeAudioConfig GetRealtimeAudioConfig() const { return {}; }
    void EnableSoftwareInputReference(bool enable);
    void SetOutputTap(std::function<void(const std::vector<int16_t>& data)> callback);
    void SetReferenceOrder(AudioReferenceOrder order) { reference_order_ = order; }
    void SetSoftwareReferenceSupported(bool supported) { software_reference_supported_ = supported; }
#if CONFIG_AUDIO_REFERENCE_PROBE
    virtual int GetReferenceProbeChannelCount() const;
    virtual std::string GetReferenceProbeChannelLabel(int channel) const;
    virtual int ReadReferenceProbeData(int16_t* dest, int frames, int* channel_count);
    int WriteReferenceProbeData(const int16_t* data, int samples);
#endif

protected:
    void SetAfeInputFormatOverride(const std::string& format) { afe_input_format_override_ = format; }

    i2s_chan_handle_t tx_handle_ = nullptr;
    i2s_chan_handle_t rx_handle_ = nullptr;

    bool duplex_ = false;
    bool input_reference_ = false;
    bool input_enabled_ = false;
    bool output_enabled_ = false;
    int input_sample_rate_ = 0;
    int output_sample_rate_ = 0;
    int input_channels_ = 1;
    int output_channels_ = 1;
    int output_volume_ = 70;
    float input_gain_ = 0.0;
    bool software_reference_supported_ = true;
    bool software_reference_enabled_ = false;
    AudioReferenceOrder reference_order_ = AudioReferenceOrder::kReferenceBeforeNearEnd;
    std::string afe_input_format_override_;
    std::mutex output_tap_mutex_;
    std::function<void(const std::vector<int16_t>& data)> output_tap_callback_;

    virtual int Read(int16_t* dest, int samples) = 0;
    virtual int Write(const int16_t* data, int samples) = 0;
};

#endif // _AUDIO_CODEC_H
