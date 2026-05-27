#include "korvo2_audio_codec.h"
#include "config.h"

#include <driver/i2c_master.h>
#include <driver/i2s_std.h>
#include <driver/i2s_tdm.h>
#include <esp_log.h>
#include <esp_timer.h>

#include <algorithm>
#include <cmath>
#include <cstring>

#define TAG "Korvo2AudioCodec"

#ifndef AUDIO_KORVO2_RX_STD_32BIT_STEREO
#define AUDIO_KORVO2_RX_STD_32BIT_STEREO 0
#endif

#ifndef AUDIO_KORVO2_MIC1_SLOT
#define AUDIO_KORVO2_MIC1_SLOT 1
#endif

namespace {

constexpr int kEs7210PhysicalChannels = 4;
constexpr int kKorvo2CaptureSlots = AUDIO_KORVO2_CAPTURE_SLOT_COUNT;
constexpr int kKorvo2Mic0Slot = AUDIO_KORVO2_MIC_SLOT;
constexpr int kKorvo2Mic1Slot = AUDIO_KORVO2_MIC1_SLOT;
constexpr int kKorvo2ReferenceSlot = AUDIO_KORVO2_REFERENCE_SLOT;
constexpr int kEs8311SystemReg13 = 0x13;
constexpr int kEs8311AdcReg1b = 0x1b;
constexpr int kEs8311AdcReg1c = 0x1c;
constexpr int kEs8311GpioReg44 = 0x44;
constexpr bool kKorvo2RxStd32 =
    AUDIO_KORVO2_RX_STD_32BIT_MONO || AUDIO_KORVO2_RX_STD_32BIT_STEREO;
constexpr uint8_t kKorvo2I2sReadChannels =
#if AUDIO_KORVO2_RX_STD_32BIT_STEREO
    2;
#elif AUDIO_KORVO2_RX_STD_32BIT_MONO
    1;
#else
    kKorvo2CaptureSlots;
#endif
constexpr uint8_t kKorvo2I2sBitsPerSample =
#if AUDIO_KORVO2_RX_STD_32BIT_STEREO || AUDIO_KORVO2_RX_STD_32BIT_MONO
    32;
#else
    16;
#endif
constexpr uint16_t kKorvo2CaptureChannelMask =
#if AUDIO_KORVO2_RX_STD_32BIT_STEREO || AUDIO_KORVO2_RX_STD_32BIT_MONO
    0;
#else
    ESP_CODEC_DEV_MAKE_CHANNEL_MASK(0) |
    ESP_CODEC_DEV_MAKE_CHANNEL_MASK(1) |
    ESP_CODEC_DEV_MAKE_CHANNEL_MASK(2) |
    ESP_CODEC_DEV_MAKE_CHANNEL_MASK(3);
#endif

#if AUDIO_KORVO2_SLOT_MONITOR
struct SlotStats {
    int first = 0;
    int last = 0;
    int min = 0;
    int max = 0;
    int peak = 0;
    int mean = 0;
    int rms = 0;
};

SlotStats CalculateSlotStats(const std::vector<int16_t>& samples, int channel, int channels) {
    SlotStats stats;
    bool initialized = false;
    int peak = 0;
    int64_t sum = 0;
    double sum_squares = 0.0;
    int count = 0;
    for (int i = channel; i < static_cast<int>(samples.size()); i += channels) {
        int value = samples[i];
        if (!initialized) {
            stats.first = value;
            stats.min = value;
            stats.max = value;
            initialized = true;
        }
        stats.last = value;
        stats.min = std::min(stats.min, value);
        stats.max = std::max(stats.max, value);

        int abs_value = value;
        if (abs_value < 0) {
            abs_value = -abs_value;
        }
        peak = std::max(peak, abs_value);
        sum += value;
        sum_squares += static_cast<double>(value) * value;
        ++count;
    }
    if (count > 0) {
        stats.peak = peak;
        stats.mean = static_cast<int>(sum / count);
        stats.rms = static_cast<int>(std::sqrt(sum_squares / count));
    }
    return stats;
}
#endif

int ChannelCountFromMask(uint16_t channel_mask) {
    int count = 0;
    for (int channel = 0; channel < kEs7210PhysicalChannels; ++channel) {
        if (channel_mask & ESP_CODEC_DEV_MAKE_CHANNEL_MASK(channel)) {
            ++count;
        }
    }
    return count;
}

#if AUDIO_KORVO2_REF_PROBE
struct RefProbeResult {
    int signed_corr_permille = 0;
    int abs_corr_permille = 0;
    int delay_ms = 0;
};

void TrimToMaxSamples(std::vector<int16_t>& buffer, size_t max_samples) {
    if (buffer.size() > max_samples) {
        buffer.erase(buffer.begin(), buffer.begin() + (buffer.size() - max_samples));
    }
}

RefProbeResult FindBestReferenceCorrelation(const std::vector<int16_t>& capture,
    const std::vector<int16_t>& playback, int sample_rate) {
    RefProbeResult best;
    const size_t window_samples = std::min(
        capture.size(), static_cast<size_t>(sample_rate * AUDIO_KORVO2_REF_PROBE_WINDOW_MS / 1000));
    const size_t max_delay_samples = static_cast<size_t>(sample_rate * AUDIO_KORVO2_REF_PROBE_MAX_DELAY_MS / 1000);
    const size_t step_samples = std::max<size_t>(1, sample_rate * AUDIO_KORVO2_REF_PROBE_STEP_MS / 1000);

    if (window_samples < 160 || playback.size() < window_samples) {
        return best;
    }

    const size_t capture_start = capture.size() - window_samples;
    for (size_t delay = 0; delay <= max_delay_samples; delay += step_samples) {
        if (playback.size() < window_samples + delay) {
            continue;
        }
        const size_t playback_start = playback.size() - delay - window_samples;
        double sum_xy = 0.0;
        double sum_x2 = 0.0;
        double sum_y2 = 0.0;
        for (size_t i = 0; i < window_samples; ++i) {
            const double x = capture[capture_start + i];
            const double y = playback[playback_start + i];
            sum_xy += x * y;
            sum_x2 += x * x;
            sum_y2 += y * y;
        }
        if (sum_x2 <= 1.0 || sum_y2 <= 1.0) {
            continue;
        }
        const double corr = sum_xy / std::sqrt(sum_x2 * sum_y2);
        const int signed_permille = static_cast<int>(corr * 1000.0);
        const int abs_permille = std::abs(signed_permille);
        if (abs_permille > best.abs_corr_permille) {
            best.signed_corr_permille = signed_permille;
            best.abs_corr_permille = abs_permille;
            best.delay_ms = static_cast<int>(delay * 1000 / sample_rate);
        }
    }
    return best;
}
#endif

} // namespace

Korvo2AudioCodec::Korvo2AudioCodec(void* i2c_master_handle, int input_sample_rate, int output_sample_rate,
    gpio_num_t mclk, gpio_num_t bclk, gpio_num_t ws, gpio_num_t dout, gpio_num_t din,
    gpio_num_t pa_pin, uint8_t es8311_addr, uint8_t es7210_addr,
    bool input_reference, uint16_t input_mic_channel_mask, uint16_t input_reference_channel_mask)
    : input_mic_channel_mask_(input_mic_channel_mask),
      input_reference_channel_mask_(input_reference_channel_mask),
      hardware_input_reference_(input_reference) {
    duplex_ = true;
    input_reference_ = hardware_input_reference_;
    const int mic_channels = std::max(1, ChannelCountFromMask(input_mic_channel_mask_));
    if (hardware_input_reference_) {
        input_channels_ = AUDIO_KORVO2_AFE_TOTAL_CH_NUM;
        SetAfeInputFormatOverride(AUDIO_KORVO2_AFE_INPUT_FORMAT);
        SetReferenceOrder(AUDIO_KORVO2_AFE_INPUT_FORMAT[0] == 'R'
            ? AudioReferenceOrder::kReferenceBeforeNearEnd
            : AudioReferenceOrder::kReferenceAfterNearEnd);
    } else {
        input_channels_ = mic_channels;
        SetReferenceOrder(AudioReferenceOrder::kReferenceAfterNearEnd);
    }
    input_sample_rate_ = input_sample_rate;
    output_sample_rate_ = output_sample_rate;
    input_gain_ = AUDIO_INPUT_MIC_GAIN_DB;
    SetSoftwareReferenceSupported(!hardware_input_reference_);

    CreateDuplexChannels(mclk, bclk, ws, dout, din);

    audio_codec_i2s_cfg_t i2s_cfg = {
        .port = I2S_NUM_0,
        .rx_handle = rx_handle_,
        .tx_handle = tx_handle_,
    };
    data_if_ = audio_codec_new_i2s_data(&i2s_cfg);
    assert(data_if_ != nullptr);

    audio_codec_i2c_cfg_t i2c_cfg = {
        .port = I2C_NUM_1,
        .addr = es8311_addr,
        .bus_handle = i2c_master_handle,
    };
    out_ctrl_if_ = audio_codec_new_i2c_ctrl(&i2c_cfg);
    assert(out_ctrl_if_ != nullptr);

    gpio_if_ = audio_codec_new_gpio();
    assert(gpio_if_ != nullptr);

    es8311_codec_cfg_t es8311_cfg = {};
    es8311_cfg.ctrl_if = out_ctrl_if_;
    es8311_cfg.gpio_if = gpio_if_;
    es8311_cfg.codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC;
    es8311_cfg.pa_pin = pa_pin;
    es8311_cfg.use_mclk = true;
    es8311_cfg.hw_gain.pa_voltage = 5.0;
    es8311_cfg.hw_gain.codec_dac_voltage = 3.3;
    out_codec_if_ = es8311_codec_new(&es8311_cfg);
    assert(out_codec_if_ != nullptr);

    esp_codec_dev_cfg_t dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_OUT,
        .codec_if = out_codec_if_,
        .data_if = data_if_,
    };
    output_dev_ = esp_codec_dev_new(&dev_cfg);
    assert(output_dev_ != nullptr);
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_codec_dev_write_reg(output_dev_, kEs8311SystemReg13, 0x10));
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_codec_dev_write_reg(output_dev_, kEs8311AdcReg1b, 0x0A));
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_codec_dev_write_reg(output_dev_, kEs8311AdcReg1c, 0x6A));
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_codec_dev_write_reg(output_dev_, kEs8311GpioReg44, AUDIO_KORVO2_ES8311_GPIO44));
    int system13 = 0;
    int adc1b = 0;
    int adc1c = 0;
    int gpio44 = 0;
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_codec_dev_read_reg(output_dev_, kEs8311SystemReg13, &system13));
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_codec_dev_read_reg(output_dev_, kEs8311AdcReg1b, &adc1b));
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_codec_dev_read_reg(output_dev_, kEs8311AdcReg1c, &adc1c));
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_codec_dev_read_reg(output_dev_, kEs8311GpioReg44, &gpio44));
    ESP_LOGI(TAG, "ES8311 AEC reference route: reg13=0x%02x reg1b=0x%02x reg1c=0x%02x gpio44=0x%02x",
        system13, adc1b, adc1c, gpio44);

    i2c_cfg.addr = es7210_addr;
    in_ctrl_if_ = audio_codec_new_i2c_ctrl(&i2c_cfg);
    assert(in_ctrl_if_ != nullptr);

    es7210_codec_cfg_t es7210_cfg = {};
    es7210_cfg.ctrl_if = in_ctrl_if_;
    es7210_cfg.mic_selected = AUDIO_KORVO2_ES7210_MIC_SELECTED;
    in_codec_if_ = es7210_codec_new(&es7210_cfg);
    assert(in_codec_if_ != nullptr);

    dev_cfg.dev_type = ESP_CODEC_DEV_TYPE_IN;
    dev_cfg.codec_if = in_codec_if_;
    input_dev_ = esp_codec_dev_new(&dev_cfg);
    assert(input_dev_ != nullptr);

    const std::string afe_format = GetAfeInputFormat();
    ESP_LOGI(TAG,
        "Korvo-2 audio initialized, mic_mask=0x%x ref_mask=0x%x logical_channels=%d "
        "capture_slots=%d i2s_read_channels=%u i2s_bits=%u hw_ref=%d sw_ref=%d format=%s",
        input_mic_channel_mask_, input_reference_channel_mask_,
        input_channels_,
        kKorvo2CaptureSlots,
        static_cast<unsigned>(kKorvo2I2sReadChannels),
        static_cast<unsigned>(kKorvo2I2sBitsPerSample),
        hardware_input_reference_ ? 1 : 0,
        software_reference_supported() ? 1 : 0,
        afe_format.c_str());
}

Korvo2AudioCodec::~Korvo2AudioCodec() {
    ESP_ERROR_CHECK(esp_codec_dev_close(output_dev_));
    esp_codec_dev_delete(output_dev_);
    ESP_ERROR_CHECK(esp_codec_dev_close(input_dev_));
    esp_codec_dev_delete(input_dev_);

    audio_codec_delete_codec_if(in_codec_if_);
    audio_codec_delete_ctrl_if(in_ctrl_if_);
    audio_codec_delete_codec_if(out_codec_if_);
    audio_codec_delete_ctrl_if(out_ctrl_if_);
    audio_codec_delete_gpio_if(gpio_if_);
    audio_codec_delete_data_if(data_if_);
}

void Korvo2AudioCodec::CreateDuplexChannels(gpio_num_t mclk, gpio_num_t bclk, gpio_num_t ws,
    gpio_num_t dout, gpio_num_t din) {
    assert(input_sample_rate_ == output_sample_rate_);

    i2s_chan_config_t chan_cfg = {
        .id = I2S_NUM_0,
        .role = I2S_ROLE_MASTER,
        .dma_desc_num = AUDIO_CODEC_DMA_DESC_NUM,
        .dma_frame_num = AUDIO_CODEC_DMA_FRAME_NUM,
        .auto_clear_after_cb = true,
        .auto_clear_before_cb = false,
        .intr_priority = 0,
    };
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &tx_handle_, &rx_handle_));

    i2s_std_config_t std_cfg = {
        .clk_cfg = {
            .sample_rate_hz = static_cast<uint32_t>(output_sample_rate_),
            .clk_src = I2S_CLK_SRC_DEFAULT,
            .ext_clk_freq_hz = 0,
            .mclk_multiple = I2S_MCLK_MULTIPLE_256
        },
        .slot_cfg = {
            .data_bit_width = I2S_DATA_BIT_WIDTH_16BIT,
            .slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO,
            .slot_mode = I2S_SLOT_MODE_STEREO,
            .slot_mask = I2S_STD_SLOT_BOTH,
            .ws_width = I2S_DATA_BIT_WIDTH_16BIT,
            .ws_pol = false,
            .bit_shift = true,
            .left_align = true,
            .big_endian = false,
            .bit_order_lsb = false
        },
        .gpio_cfg = {
            .mclk = mclk,
            .bclk = bclk,
            .ws = ws,
            .dout = dout,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false
            }
        }
    };

#if AUDIO_KORVO2_RX_STD_32BIT_STEREO || AUDIO_KORVO2_RX_STD_32BIT_MONO
    i2s_std_config_t rx_std_cfg = std_cfg;
    rx_std_cfg.slot_cfg.data_bit_width = I2S_DATA_BIT_WIDTH_32BIT;
    rx_std_cfg.slot_cfg.slot_bit_width = I2S_SLOT_BIT_WIDTH_32BIT;
    rx_std_cfg.slot_cfg.slot_mode = AUDIO_KORVO2_RX_STD_32BIT_STEREO ? I2S_SLOT_MODE_STEREO : I2S_SLOT_MODE_MONO;
    rx_std_cfg.slot_cfg.slot_mask = AUDIO_KORVO2_RX_STD_32BIT_STEREO ? I2S_STD_SLOT_BOTH : I2S_STD_SLOT_LEFT;
    rx_std_cfg.slot_cfg.ws_width = I2S_DATA_BIT_WIDTH_32BIT;
    rx_std_cfg.gpio_cfg.dout = I2S_GPIO_UNUSED;
    rx_std_cfg.gpio_cfg.din = din;
#else
    i2s_tdm_config_t tdm_cfg = {
        .clk_cfg = {
            .sample_rate_hz = static_cast<uint32_t>(input_sample_rate_),
            .clk_src = I2S_CLK_SRC_DEFAULT,
            .ext_clk_freq_hz = 0,
            .mclk_multiple = I2S_MCLK_MULTIPLE_256,
            .bclk_div = 8,
        },
        .slot_cfg = {
            .data_bit_width = I2S_DATA_BIT_WIDTH_16BIT,
            .slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO,
            .slot_mode = I2S_SLOT_MODE_STEREO,
            .slot_mask = i2s_tdm_slot_mask_t(I2S_TDM_SLOT0 | I2S_TDM_SLOT1 | I2S_TDM_SLOT2 | I2S_TDM_SLOT3),
            .ws_width = I2S_TDM_AUTO_WS_WIDTH,
            .ws_pol = false,
            .bit_shift = true,
            .left_align = false,
            .big_endian = false,
            .bit_order_lsb = false,
            .skip_mask = false,
            .total_slot = I2S_TDM_AUTO_SLOT_NUM
        },
        .gpio_cfg = {
            .mclk = mclk,
            .bclk = bclk,
            .ws = ws,
            .dout = I2S_GPIO_UNUSED,
            .din = din,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false
            }
        }
    };
#endif

    ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx_handle_, &std_cfg));
#if AUDIO_KORVO2_RX_STD_32BIT_STEREO || AUDIO_KORVO2_RX_STD_32BIT_MONO
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(rx_handle_, &rx_std_cfg));
#else
    ESP_ERROR_CHECK(i2s_channel_init_tdm_mode(rx_handle_, &tdm_cfg));
#endif
    ESP_ERROR_CHECK(i2s_channel_enable(tx_handle_));
    ESP_ERROR_CHECK(i2s_channel_enable(rx_handle_));
    ESP_LOGI(TAG, "Duplex I2S channels created, rx_std32=%d i2s_read_channels=%u capture_slots=%d",
        kKorvo2RxStd32 ? 1 : 0,
        static_cast<unsigned>(kKorvo2I2sReadChannels),
        kKorvo2CaptureSlots);
}

void Korvo2AudioCodec::SetOutputVolume(int volume) {
    ESP_ERROR_CHECK(esp_codec_dev_set_out_vol(output_dev_, volume));
    AudioCodec::SetOutputVolume(volume);
}

#if AUDIO_KORVO2_REF_PROBE
void Korvo2AudioCodec::AppendReferenceProbePlayback(const int16_t* data, int samples) {
    if (data == nullptr || samples <= 0) {
        return;
    }
    std::lock_guard<std::mutex> lock(ref_probe_mutex_);
    ref_probe_playback_buffer_.insert(ref_probe_playback_buffer_.end(), data, data + samples);
    TrimToMaxSamples(ref_probe_playback_buffer_, static_cast<size_t>(output_sample_rate_) * 3);
}

void Korvo2AudioCodec::UpdateReferenceProbeCaptureAndLog(const std::vector<int16_t>& raw_input, int frames) {
    if (raw_input.empty() || frames <= 0) {
        return;
    }

    std::lock_guard<std::mutex> lock(ref_probe_mutex_);
    for (int frame = 0; frame < frames; ++frame) {
        const int raw_base = frame * kKorvo2CaptureSlots;
        for (int slot = 0; slot < kKorvo2CaptureSlots; ++slot) {
            ref_probe_slot_buffers_[slot].push_back(raw_input[raw_base + slot]);
        }
    }

    const size_t max_capture_samples = static_cast<size_t>(input_sample_rate_) * 3;
    for (int slot = 0; slot < kKorvo2CaptureSlots; ++slot) {
        TrimToMaxSamples(ref_probe_slot_buffers_[slot], max_capture_samples);
    }

    const int64_t now_us = esp_timer_get_time();
    if (now_us - ref_probe_last_log_us_ < 1000000) {
        return;
    }
    ref_probe_last_log_us_ = now_us;

    RefProbeResult result[kKorvo2CaptureSlots];
    int best_slot = 0;
    for (int slot = 0; slot < kKorvo2CaptureSlots; ++slot) {
        result[slot] = FindBestReferenceCorrelation(
            ref_probe_slot_buffers_[slot], ref_probe_playback_buffer_, input_sample_rate_);
        if (result[slot].abs_corr_permille > result[best_slot].abs_corr_permille) {
            best_slot = slot;
        }
    }

    ESP_LOGI(TAG,
        "Korvo-2 ref probe: s0=%+d/1000@%dms s1=%+d/1000@%dms s2=%+d/1000@%dms s3=%+d/1000@%dms best=s%d playback_buf=%u",
        result[0].signed_corr_permille, result[0].delay_ms,
        result[1].signed_corr_permille, result[1].delay_ms,
        result[2].signed_corr_permille, result[2].delay_ms,
        result[3].signed_corr_permille, result[3].delay_ms,
        best_slot,
        static_cast<unsigned>(ref_probe_playback_buffer_.size()));
}
#endif

void Korvo2AudioCodec::OutputData(std::vector<int16_t>& data) {
    Write(data.data(), data.size());

    std::function<void(const std::vector<int16_t>& data)> callback;
    {
        std::lock_guard<std::mutex> lock(output_tap_mutex_);
        callback = output_tap_callback_;
    }
    if (callback) {
        callback(data);
    }
}

void Korvo2AudioCodec::EnableInput(bool enable) {
    std::lock_guard<std::mutex> lock(data_if_mutex_);
    if (enable == input_enabled_) {
        return;
    }
    if (enable) {
        esp_codec_dev_sample_info_t fs = {
            .bits_per_sample = kKorvo2I2sBitsPerSample,
            .channel = kKorvo2I2sReadChannels,
            .channel_mask = kKorvo2CaptureChannelMask,
            .sample_rate = static_cast<uint32_t>(input_sample_rate_),
            .mclk_multiple = 0,
        };
        ESP_ERROR_CHECK(esp_codec_dev_open(input_dev_, &fs));
        const uint16_t gain_mask = hardware_input_reference_
            ? (input_mic_channel_mask_ | input_reference_channel_mask_)
            : input_mic_channel_mask_;
        ESP_ERROR_CHECK(esp_codec_dev_set_in_channel_gain(input_dev_, gain_mask, input_gain_));
    } else {
        ESP_ERROR_CHECK(esp_codec_dev_close(input_dev_));
    }
    AudioCodec::EnableInput(enable);
}

void Korvo2AudioCodec::EnableOutput(bool enable) {
    std::lock_guard<std::mutex> lock(data_if_mutex_);
    if (enable == output_enabled_) {
        return;
    }
    if (enable) {
#if AUDIO_KORVO2_RX_STD_32BIT_MONO
        const uint16_t output_channel_mask = ESP_CODEC_DEV_MAKE_CHANNEL_MASK(0);
        const uint8_t output_channels = 1;
#else
        const uint16_t output_channel_mask = hardware_input_reference_
            ? static_cast<uint16_t>(ESP_CODEC_DEV_MAKE_CHANNEL_MASK(0) | ESP_CODEC_DEV_MAKE_CHANNEL_MASK(1))
            : static_cast<uint16_t>(0);
        const uint8_t output_channels = static_cast<uint8_t>(hardware_input_reference_ ? 2 : 1);
#endif
        esp_codec_dev_sample_info_t fs = {
            .bits_per_sample = 16,
            .channel = output_channels,
            .channel_mask = output_channel_mask,
            .sample_rate = static_cast<uint32_t>(output_sample_rate_),
            .mclk_multiple = 0,
        };
        ESP_ERROR_CHECK(esp_codec_dev_open(output_dev_, &fs));
        ESP_ERROR_CHECK(esp_codec_dev_set_out_vol(output_dev_, output_volume_));
    } else {
        ESP_ERROR_CHECK(esp_codec_dev_close(output_dev_));
    }
    AudioCodec::EnableOutput(enable);
}

int Korvo2AudioCodec::Read(int16_t* dest, int samples) {
    if (input_enabled_) {
        if (samples % input_channels_ != 0) {
            ESP_LOGW(TAG, "Unexpected logical sample count %d for %d channels", samples, input_channels_);
        }
        const int frames = samples / input_channels_;
        const int raw_samples = frames * kKorvo2CaptureSlots;
        raw_input_buffer_.resize(raw_samples);

        esp_err_t ret = esp_codec_dev_read(input_dev_, raw_input_buffer_.data(), raw_samples * sizeof(int16_t));
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Read failed: %s", esp_err_to_name(ret));
            std::memset(dest, 0, samples * sizeof(int16_t));
            return samples;
        }

        for (int frame = 0; frame < frames; ++frame) {
            const int raw_base = frame * kKorvo2CaptureSlots;
            const int out_base = frame * input_channels_;

            if (hardware_input_reference_ && input_channels_ == kKorvo2CaptureSlots) {
                std::memcpy(dest + out_base, raw_input_buffer_.data() + raw_base,
                    input_channels_ * sizeof(int16_t));
                const size_t afe_format_len = std::strlen(AUDIO_KORVO2_AFE_INPUT_FORMAT);
                for (int channel = 0; channel < input_channels_ &&
                     channel < static_cast<int>(afe_format_len); ++channel) {
                    if (AUDIO_KORVO2_AFE_INPUT_FORMAT[channel] == 'N') {
                        dest[out_base + channel] = 0;
                    }
                }
                continue;
            }

            const int16_t reference_sample = raw_input_buffer_[raw_base + kKorvo2ReferenceSlot];
            const int16_t mic0_sample = raw_input_buffer_[raw_base + kKorvo2Mic0Slot];
            const int16_t mic1_sample = raw_input_buffer_[raw_base + kKorvo2Mic1Slot];
            const int near_end_channels = input_channels_ - (hardware_input_reference_ ? 1 : 0);
            int out_index = out_base;

            if (hardware_input_reference_ && reference_order() == AudioReferenceOrder::kReferenceBeforeNearEnd) {
                dest[out_index++] = reference_sample;
            }

            dest[out_index++] = mic0_sample;
            if (near_end_channels > 1) {
                dest[out_index++] = mic1_sample;
            }

            if (hardware_input_reference_ && reference_order() == AudioReferenceOrder::kReferenceAfterNearEnd) {
                dest[out_index++] = reference_sample;
            }
        }
#if AUDIO_KORVO2_SLOT_MONITOR
        static int64_t last_slot_monitor_us = 0;
        const int64_t now_us = esp_timer_get_time();
        if (now_us - last_slot_monitor_us >= AUDIO_KORVO2_SLOT_MONITOR_INTERVAL_MS * 1000) {
            last_slot_monitor_us = now_us;
            const std::string afe_format = GetAfeInputFormat();
            const SlotStats slot0 = CalculateSlotStats(raw_input_buffer_, 0, kKorvo2CaptureSlots);
            const SlotStats slot1 = CalculateSlotStats(raw_input_buffer_, 1, kKorvo2CaptureSlots);
#if AUDIO_KORVO2_CAPTURE_SLOT_COUNT >= 4
            const SlotStats slot2 = CalculateSlotStats(raw_input_buffer_, 2, kKorvo2CaptureSlots);
            const SlotStats slot3 = CalculateSlotStats(raw_input_buffer_, 3, kKorvo2CaptureSlots);
            ESP_LOGI(TAG,
                "Korvo-2 raw slots: "
                "s0[first=%d last=%d min=%d max=%d pk=%d rms=%d dc=%d] "
                "s1[first=%d last=%d min=%d max=%d pk=%d rms=%d dc=%d] "
                "s2[first=%d last=%d min=%d max=%d pk=%d rms=%d dc=%d] "
                "s3[first=%d last=%d min=%d max=%d pk=%d rms=%d dc=%d] "
                "format=%s slots=[ref:s%d mic0:s%d mic1:s%d] rx_std32=%d hw_ref=%d sw_ref=%d",
                slot0.first, slot0.last, slot0.min, slot0.max, slot0.peak, slot0.rms, slot0.mean,
                slot1.first, slot1.last, slot1.min, slot1.max, slot1.peak, slot1.rms, slot1.mean,
                slot2.first, slot2.last, slot2.min, slot2.max, slot2.peak, slot2.rms, slot2.mean,
                slot3.first, slot3.last, slot3.min, slot3.max, slot3.peak, slot3.rms, slot3.mean,
                afe_format.c_str(),
                kKorvo2ReferenceSlot,
                kKorvo2Mic0Slot,
                kKorvo2Mic1Slot,
                kKorvo2RxStd32 ? 1 : 0,
                hardware_input_reference_ ? 1 : 0,
                using_software_input_reference() ? 1 : 0);
#else
            ESP_LOGI(TAG,
                "Korvo-2 raw slots: "
                "s0[first=%d last=%d min=%d max=%d pk=%d rms=%d dc=%d] "
                "s1[first=%d last=%d min=%d max=%d pk=%d rms=%d dc=%d] "
                "format=%s slots=[ref:s%d mic0:s%d] rx_std32=%d hw_ref=%d sw_ref=%d",
                slot0.first, slot0.last, slot0.min, slot0.max, slot0.peak, slot0.rms, slot0.mean,
                slot1.first, slot1.last, slot1.min, slot1.max, slot1.peak, slot1.rms, slot1.mean,
                afe_format.c_str(),
                kKorvo2ReferenceSlot,
                kKorvo2Mic0Slot,
                kKorvo2RxStd32 ? 1 : 0,
                hardware_input_reference_ ? 1 : 0,
                using_software_input_reference() ? 1 : 0);
#endif
        }
#endif
#if AUDIO_KORVO2_REF_PROBE
        UpdateReferenceProbeCaptureAndLog(raw_input_buffer_, frames);
#endif
    }
    return samples;
}

int Korvo2AudioCodec::Write(const int16_t* data, int samples) {
    if (output_enabled_) {
        if (hardware_input_reference_ && !AUDIO_KORVO2_RX_STD_32BIT_MONO) {
#if AUDIO_KORVO2_REF_PROBE
            AppendReferenceProbePlayback(data, samples);
#endif
            stereo_output_buffer_.resize(samples * 2);
            for (int i = 0; i < samples; ++i) {
                stereo_output_buffer_[i * 2] = data[i];
                stereo_output_buffer_[i * 2 + 1] = data[i];
            }
            ESP_ERROR_CHECK_WITHOUT_ABORT(esp_codec_dev_write(
                output_dev_, stereo_output_buffer_.data(), stereo_output_buffer_.size() * sizeof(int16_t)));
        } else {
#if AUDIO_KORVO2_REF_PROBE
            if (hardware_input_reference_) {
                AppendReferenceProbePlayback(data, samples);
            }
#endif
            ESP_ERROR_CHECK_WITHOUT_ABORT(esp_codec_dev_write(output_dev_, const_cast<int16_t*>(data), samples * sizeof(int16_t)));
        }
    }
    return samples;
}
