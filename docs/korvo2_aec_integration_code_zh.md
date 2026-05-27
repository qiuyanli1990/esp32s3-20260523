# Korvo-2 AEC 集成方案与代码

## 1. AEC 链路

Korvo-2 使用硬件 reference 回采，AFE 输入格式固定为 `RMNM`。

```text
远端播放 PCM
  -> ES8311 DAC / PA
  -> 板级 AEC reference 回采
  -> ES7210 MIC3
  -> I2S RX: 2 channels * 32-bit
  -> int16_t lane: [R, M, N, M]
  -> AFE: input_format="RMNM", total=4, mic=2, ref=1
  -> AEC 处理后的近端语音
  -> 上行业务链路
```

## 2. 通道约定

| int16 lane | AFE 标记 | 宏 | 含义 |
| --- | --- | --- | --- |
| `s0` | `R` | `AUDIO_KORVO2_REFERENCE_SLOT` | echo reference |
| `s1` | `M` | `AUDIO_KORVO2_MIC_SLOT` | mic0 |
| `s2` | `N` | 占位 | unused，送 AFE 前清零 |
| `s3` | `M` | `AUDIO_KORVO2_MIC1_SLOT` | mic1 |

## 3. Board 配置

文件：`main/boards/esp32s3-korvo-2/config.h`

```c
#define AUDIO_INPUT_SAMPLE_RATE  16000
#define AUDIO_OUTPUT_SAMPLE_RATE 16000
#define AUDIO_INPUT_MIC_GAIN_DB  30.0f

#define AUDIO_INPUT_REFERENCE true
#define AUDIO_KORVO2_AFE_INPUT_FORMAT "RMNM"
#define AUDIO_KORVO2_AFE_TOTAL_CH_NUM 4
#define AUDIO_KORVO2_AFE_MIC_NUM 2
#define AUDIO_KORVO2_AFE_REF_NUM 1

#define AUDIO_KORVO2_RX_STD_32BIT_MONO 0
#define AUDIO_KORVO2_RX_STD_32BIT_STEREO 1
#define AUDIO_KORVO2_CAPTURE_SLOT_COUNT 4
#define AUDIO_KORVO2_MIC_SLOT 1
#define AUDIO_KORVO2_MIC1_SLOT 3
#define AUDIO_KORVO2_REFERENCE_SLOT 0
#define AUDIO_KORVO2_ES7210_MIC_SELECTED 0x07

#define AUDIO_KORVO2_REF_PROBE 0
#define AUDIO_KORVO2_SLOT_MONITOR 1
#define AUDIO_KORVO2_SLOT_MONITOR_INTERVAL_MS 200
#define AUDIO_KORVO2_REF_PROBE_WINDOW_MS 200
#define AUDIO_KORVO2_REF_PROBE_MAX_DELAY_MS 400
#define AUDIO_KORVO2_REF_PROBE_STEP_MS 10

#define AUDIO_AFE_USE_SR_LOW_COST true
#define AUDIO_AFE_USE_VC_LOW_COST false
#define AUDIO_KORVO2_SOFTWARE_REF_DELAY_MS 0
#define AUDIO_KORVO2_ES8311_GPIO44 0x50

#define AUDIO_I2S_GPIO_MCLK GPIO_NUM_16
#define AUDIO_I2S_GPIO_WS   GPIO_NUM_45
#define AUDIO_I2S_GPIO_BCLK GPIO_NUM_9
#define AUDIO_I2S_GPIO_DIN  GPIO_NUM_10
#define AUDIO_I2S_GPIO_DOUT GPIO_NUM_8

#define AUDIO_CODEC_PA_PIN       GPIO_NUM_48
#define AUDIO_CODEC_I2C_SDA_PIN  GPIO_NUM_17
#define AUDIO_CODEC_I2C_SCL_PIN  GPIO_NUM_18
#define AUDIO_CODEC_ES8311_ADDR  ES8311_CODEC_DEFAULT_ADDR
#define AUDIO_CODEC_ES7210_ADDR  ES7210_CODEC_DEFAULT_ADDR

#define AUDIO_INPUT_MIC_CHANNEL_MASK       (ESP_CODEC_DEV_MAKE_CHANNEL_MASK(0) | ESP_CODEC_DEV_MAKE_CHANNEL_MASK(1))
#define AUDIO_INPUT_REFERENCE_CHANNEL_MASK ESP_CODEC_DEV_MAKE_CHANNEL_MASK(2)
```

## 4. AudioCodec AFE Format Override

文件：`main/audio/audio_codec.h`

```c++
protected:
    void SetAfeInputFormatOverride(const std::string& format) {
        afe_input_format_override_ = format;
    }

    std::string afe_input_format_override_;
```

```c++
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
    if (!afe_input_format_override_.empty()) {
        return static_cast<int>(afe_input_format_override_.size());
    }
    return near_end_input_channels() + (has_aec_reference() ? 1 : 0);
}

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
```

文件：`main/audio/audio_codec.cc`

```c++
std::string AudioCodec::GetAfeInputFormat() const {
    if (!afe_input_format_override_.empty()) {
        return afe_input_format_override_;
    }

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
```

## 5. Korvo2AudioCodec 常量

文件：`main/boards/esp32s3-korvo-2/korvo2_audio_codec.cc`

```c++
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

} // namespace
```

## 6. Korvo2AudioCodec 初始化

文件：`main/boards/esp32s3-korvo-2/korvo2_audio_codec.cc`

```c++
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
}
```

## 7. I2S RX/TX 创建

文件：`main/boards/esp32s3-korvo-2/korvo2_audio_codec.cc`

```c++
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
}
```

## 8. 输入设备打开

文件：`main/boards/esp32s3-korvo-2/korvo2_audio_codec.cc`

```c++
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
```

## 9. 采集数据打包

文件：`main/boards/esp32s3-korvo-2/korvo2_audio_codec.cc`

```c++
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
    }
    return samples;
}
```

## 10. 输出设备与播放数据

文件：`main/boards/esp32s3-korvo-2/korvo2_audio_codec.cc`

```c++
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
```

```c++
int Korvo2AudioCodec::Write(const int16_t* data, int samples) {
    if (output_enabled_) {
        if (hardware_input_reference_ && !AUDIO_KORVO2_RX_STD_32BIT_MONO) {
            stereo_output_buffer_.resize(samples * 2);
            for (int i = 0; i < samples; ++i) {
                stereo_output_buffer_[i * 2] = data[i];
                stereo_output_buffer_[i * 2 + 1] = data[i];
            }
            ESP_ERROR_CHECK_WITHOUT_ABORT(esp_codec_dev_write(
                output_dev_, stereo_output_buffer_.data(), stereo_output_buffer_.size() * sizeof(int16_t)));
        } else {
            ESP_ERROR_CHECK_WITHOUT_ABORT(esp_codec_dev_write(output_dev_, const_cast<int16_t*>(data), samples * sizeof(int16_t)));
        }
    }
    return samples;
}
```

## 11. AFE 初始化

文件：`main/audio/processors/afe_audio_processor.cc`

```c++
std::string input_format = codec_->GetAfeInputFormat();
int afe_total_ch_num = codec_->afe_input_channels();
int afe_mic_num = codec_->near_end_input_channels();
int ref_num = codec_->has_aec_reference() ? 1 : 0;

#if CONFIG_BOARD_TYPE_ESP_KORVO_2
    if (codec_->has_hardware_input_reference()) {
#ifdef AUDIO_KORVO2_AFE_INPUT_FORMAT
        input_format = AUDIO_KORVO2_AFE_INPUT_FORMAT;
#endif
#ifdef AUDIO_KORVO2_AFE_TOTAL_CH_NUM
        afe_total_ch_num = AUDIO_KORVO2_AFE_TOTAL_CH_NUM;
#endif
#ifdef AUDIO_KORVO2_AFE_MIC_NUM
        afe_mic_num = AUDIO_KORVO2_AFE_MIC_NUM;
#endif
#ifdef AUDIO_KORVO2_AFE_REF_NUM
        ref_num = AUDIO_KORVO2_AFE_REF_NUM;
#endif
    }
#endif

afe_config_t* afe_config = afe_config_init(input_format.c_str(), models, kAfeType, kAfeMode);
afe_config->aec_mode = kAecMode;
afe_config->vad_mode = VAD_MODE_0;
afe_config->vad_min_noise_ms = 100;
afe_config->agc_init = false;
afe_config->memory_alloc_mode = AFE_MEMORY_ALLOC_MORE_PSRAM;
afe_config->aec_init = codec_->has_aec_reference();
afe_config->vad_init = true;
```

```c++
#if CONFIG_BOARD_TYPE_ESP_KORVO_2
    const bool korvo_dual_mic_hw_ref =
        codec_->has_hardware_input_reference() && afe_mic_num > 1;
    afe_config->se_init = korvo_dual_mic_hw_ref;
    if (!korvo_dual_mic_hw_ref) {
        afe_config->ns_init = false;
    }
    afe_config->agc_init = false;
    afe_config->vad_init = false;
    afe_config->wakenet_init = false;
    afe_config->fixed_first_channel = true;
    afe_config->afe_ringbuf_size = 25;
#if !AUDIO_AFE_USE_SR_LOW_COST
    afe_config->aec_filter_length = 2;
#endif
    afe_config->pcm_config.total_ch_num = afe_total_ch_num;
    afe_config->pcm_config.mic_num = afe_mic_num;
    afe_config->pcm_config.ref_num = ref_num;
    afe_config->pcm_config.sample_rate = 16000;
    afe_config = afe_config_check(afe_config);
#endif
```

## 12. AFE Feed

文件：`main/audio/processors/afe_audio_processor.cc`

```c++
void AfeAudioProcessor::Feed(std::vector<int16_t>&& data) {
    if (afe_data_ == nullptr) {
        return;
    }

    std::lock_guard<std::mutex> lock(input_buffer_mutex_);
    if (!IsRunning()) {
        return;
    }
    input_buffer_.insert(input_buffer_.end(), data.begin(), data.end());
    const size_t feed_size = afe_iface_->get_feed_chunksize(afe_data_);
    if (codec_->using_software_input_reference()) {
        FeedSoftwareReferenceFramesLocked(feed_size);
    } else {
        FeedHardwareInputFramesLocked(feed_size);
    }
}
```

```c++
void AfeAudioProcessor::FeedHardwareInputFramesLocked(size_t feed_size) {
    const size_t chunk_size = feed_size * codec_->input_channels();
    while (input_buffer_.size() >= chunk_size) {
        afe_iface_->feed(afe_data_, input_buffer_.data());
        input_buffer_.erase(input_buffer_.begin(), input_buffer_.begin() + chunk_size);
    }
}
```

