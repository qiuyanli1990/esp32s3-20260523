#ifndef REALTIME_AUDIO_PORT_H
#define REALTIME_AUDIO_PORT_H

#include <cstdint>
#include <string>
#include <vector>

#include "audio_codec.h"
#include "realtime_audio_config.h"

class RealtimeAudioPort {
public:
    virtual ~RealtimeAudioPort() = default;

    virtual int input_sample_rate() const = 0;
    virtual int output_sample_rate() const = 0;
    virtual int input_channels() const = 0;
    virtual int raw_capture_channel_index() const = 0;
    virtual bool has_hardware_input_reference() const = 0;
    virtual std::string GetAfeInputFormat() const = 0;
    virtual RealtimeAudioConfig GetRealtimeAudioConfig() const { return {}; }

    virtual bool input_enabled() const = 0;
    virtual bool output_enabled() const = 0;
    virtual void EnableInput(bool enable) = 0;
    virtual void EnableOutput(bool enable) = 0;

    virtual bool InputData(std::vector<int16_t>& data) = 0;
    virtual void OutputData(std::vector<int16_t>& data) = 0;
};

class AudioCodecRealtimeAudioPort : public RealtimeAudioPort {
public:
    AudioCodecRealtimeAudioPort() = default;
    explicit AudioCodecRealtimeAudioPort(AudioCodec* codec) : codec_(codec) {}

    void SetCodec(AudioCodec* codec) { codec_ = codec; }
    AudioCodec* codec() const { return codec_; }

    int input_sample_rate() const override { return codec_ != nullptr ? codec_->input_sample_rate() : 0; }
    int output_sample_rate() const override { return codec_ != nullptr ? codec_->output_sample_rate() : 0; }
    int input_channels() const override { return codec_ != nullptr ? codec_->input_channels() : 1; }
    int raw_capture_channel_index() const override { return codec_ != nullptr ? codec_->raw_capture_channel_index() : 0; }
    bool has_hardware_input_reference() const override {
        return codec_ != nullptr && codec_->has_hardware_input_reference();
    }
    std::string GetAfeInputFormat() const override { return codec_ != nullptr ? codec_->GetAfeInputFormat() : "M"; }
    RealtimeAudioConfig GetRealtimeAudioConfig() const override {
        return codec_ != nullptr ? codec_->GetRealtimeAudioConfig() : RealtimeAudioConfig{};
    }

    bool input_enabled() const override { return codec_ != nullptr && codec_->input_enabled(); }
    bool output_enabled() const override { return codec_ != nullptr && codec_->output_enabled(); }
    void EnableInput(bool enable) override {
        if (codec_ != nullptr) {
            codec_->EnableInput(enable);
        }
    }
    void EnableOutput(bool enable) override {
        if (codec_ != nullptr) {
            codec_->EnableOutput(enable);
        }
    }

    bool InputData(std::vector<int16_t>& data) override {
        return codec_ != nullptr && codec_->InputData(data);
    }
    void OutputData(std::vector<int16_t>& data) override {
        if (codec_ != nullptr) {
            codec_->OutputData(data);
        }
    }

private:
    AudioCodec* codec_ = nullptr;
};

#endif  // REALTIME_AUDIO_PORT_H
