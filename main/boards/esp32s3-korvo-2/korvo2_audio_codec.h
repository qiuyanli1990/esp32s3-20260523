#ifndef _KORVO2_AUDIO_CODEC_H_
#define _KORVO2_AUDIO_CODEC_H_

#include "audio_codec.h"
#include "config.h"

#include <esp_codec_dev.h>
#include <esp_codec_dev_defaults.h>
#include <mutex>
#include <vector>

class Korvo2AudioCodec : public AudioCodec {
private:
    const audio_codec_data_if_t* data_if_ = nullptr;
    const audio_codec_ctrl_if_t* out_ctrl_if_ = nullptr;
    const audio_codec_if_t* out_codec_if_ = nullptr;
    const audio_codec_ctrl_if_t* in_ctrl_if_ = nullptr;
    const audio_codec_if_t* in_codec_if_ = nullptr;
    const audio_codec_gpio_if_t* gpio_if_ = nullptr;

    esp_codec_dev_handle_t output_dev_ = nullptr;
    esp_codec_dev_handle_t input_dev_ = nullptr;
    std::mutex data_if_mutex_;
    std::vector<int16_t> raw_input_buffer_;
    std::vector<int16_t> stereo_output_buffer_;

    uint16_t input_mic_channel_mask_ = 0;
    uint16_t input_reference_channel_mask_ = 0;
    bool hardware_input_reference_ = false;

#if AUDIO_KORVO2_REF_PROBE
    std::mutex ref_probe_mutex_;
    std::vector<int16_t> ref_probe_playback_buffer_;
    std::vector<int16_t> ref_probe_slot_buffers_[4];
    int64_t ref_probe_last_log_us_ = 0;
#endif

    void CreateDuplexChannels(gpio_num_t mclk, gpio_num_t bclk, gpio_num_t ws,
        gpio_num_t dout, gpio_num_t din);
#if AUDIO_KORVO2_REF_PROBE
    void AppendReferenceProbePlayback(const int16_t* data, int samples);
    void UpdateReferenceProbeCaptureAndLog(const std::vector<int16_t>& raw_input, int frames);
#endif

    virtual int Read(int16_t* dest, int samples) override;
    virtual int Write(const int16_t* data, int samples) override;

public:
    Korvo2AudioCodec(void* i2c_master_handle, int input_sample_rate, int output_sample_rate,
        gpio_num_t mclk, gpio_num_t bclk, gpio_num_t ws, gpio_num_t dout, gpio_num_t din,
        gpio_num_t pa_pin, uint8_t es8311_addr, uint8_t es7210_addr,
        bool input_reference, uint16_t input_mic_channel_mask, uint16_t input_reference_channel_mask);
    virtual ~Korvo2AudioCodec();

    virtual void SetOutputVolume(int volume) override;
    virtual void OutputData(std::vector<int16_t>& data) override;
    virtual void EnableInput(bool enable) override;
    virtual void EnableOutput(bool enable) override;
};

#endif // _KORVO2_AUDIO_CODEC_H_
