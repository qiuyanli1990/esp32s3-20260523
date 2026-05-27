# ESP32-S3 Korvo-2 AEC 集成参考

本文记录当前工程中 Korvo-2 板子的硬件 AEC 修复方案、通道流程、关键代码和验证方式。本文面向后续移植、回归和问题排查。

## 1. 环境信息

### 工程环境

| 项目 | 当前值 |
| --- | --- |
| 工程目录 | `/Users/qiuyanli/esp32s3-20260523` |
| 目标芯片 | `esp32s3` |
| 板型配置 | `CONFIG_BOARD_TYPE_ESP_KORVO_2=y` |
| 设备 AEC | `CONFIG_USE_DEVICE_AEC=y` |
| 音频处理器 | `CONFIG_USE_AUDIO_PROCESSOR=y` |
| 文档日期 | 2026-05-21 |

### 工具链

| 工具 | 当前值 |
| --- | --- |
| ESP-IDF | `ESP-IDF v5.5.4` |
| IDF export | `/Users/qiuyanli/esp/esp-idf554/export.sh` |
| Python | `Python 3.14.2` |
| GCC | `xtensa-esp-elf-gcc 14.2.0` |
| OS | `Darwin 24.6.0 arm64/x86_64` |

### 本地 ADF 参考

本次修复对齐 Espressif ADF Korvo-2 的硬件参考输入模型：

- `/Users/qiuyanli/esp/esp-adf-v2.8/components/audio_board/esp32_s3_korvo2_v3/board_def.h`
  - `ES7210_MIC_SELECT (ES7210_INPUT_MIC1 | ES7210_INPUT_MIC2 | ES7210_INPUT_MIC3)`
  - `AUDIO_ADC_INPUT_CH_FORMAT "RMNM"`
- `/Users/qiuyanli/esp/esp-adf-v2.8/examples/ai_agent/coze_ws_app/main/board_config.h`
  - `INPUT_CH_ALLOCATION ("RMNM")`
  - `BOARD_CHANNELS 2`
  - `BOARD_SAMPLE_BITS 32`
- `/Users/qiuyanli/esp/esp-adf-v2.8/docs/en/design-guide/dev-boards/user-guide-esp32-s3-korvo-2-v3.0.rst`
  - Korvo-2 的 AEC reference 由 ES8311 DAC 输出或 PA 输出提供，默认推荐 ES8311 DAC output。
  - reference 信号由 ES7210 的 `ADC_MIC3P/ADC_MIC3N` 采回并送入 ESP32-S3 AEC 算法。

## 2. 问题背景

修复前 Korvo-2 的采集链路按 `MR` 2 通道处理：

- AFE 输入格式是 `MR`。
- I2S RX 读 1 个 32-bit mono slot 或等效的 2 个 16-bit lane。
- codec 把 `slot0` 当 mic，把 `slot1` 当 reference。

这个模型不匹配 Korvo-2 官方 ADF 配置。Korvo-2 的硬件参考链路需要按 `RMNM` 处理：

- `R`：硬件 echo reference。
- `M`：近端麦克风。
- `N`：unused lane，占位但不参与 AFE。
- `M`：第二路近端麦克风。

如果继续使用 `MR`，AFE 看到的 reference 位置和实际 ES7210/I2S lane 不一致，设备 AEC 会表现为“不工作”或抑制很弱。

## 3. 总体方案

当前方案：Korvo-2 走硬件 AEC reference，采集链路对齐 ADF 的 `RMNM`。

```
远端播放 PCM
    |
    v
ES8311 DAC / PA 输出
    |
    v
板级 AEC reference 回采路径
    |
    v
ES7210 ADC_MIC3P/N
    |
    v
I2S RX: STD stereo, 32-bit * 2 slots
    |
    v
按 int16_t 视角展开为 4 lane: [R, M, N, M]
    |
    v
AFE feed: input_format="RMNM", total=4, mic=2, ref=1
    |
    v
AEC/NS/SE 处理后的近端语音
    |
    v
上行到 Agora / 业务音频链路
```

关键点：

- ES7210 选择 `MIC1/MIC2/MIC3`，和 ADF 保持一致。
- I2S RX 读两个 32-bit STD stereo slot。
- 应用层以 `int16_t` 读取后，两个 32-bit slot 等价展开为 4 个 16-bit lane。
- AFE 输入格式固定为 `RMNM`。
- `N` lane 在送 AFE 前清零，避免未使用 lane 的随机值干扰调试。

## 4. 通道映射

当前软件通道契约如下：

| 16-bit lane | AFE 标记 | 当前宏 | 含义 |
| --- | --- | --- | --- |
| `s0` | `R` | `AUDIO_KORVO2_REFERENCE_SLOT 0` | echo reference |
| `s1` | `M` | `AUDIO_KORVO2_MIC_SLOT 1` | mic0 |
| `s2` | `N` | 无业务输入 | unused，占位并清零 |
| `s3` | `M` | `AUDIO_KORVO2_MIC1_SLOT 3` | mic1 |

注意不要把 AFE lane 编号和 ES7210 物理增益 channel mask 混淆：

- AFE lane：描述 `RMNM` 中每个 16-bit lane 的算法语义。
- ES7210 channel mask：描述 ES7210 物理输入通道的增益设置。

当前增益配置：

```c
#define AUDIO_INPUT_MIC_CHANNEL_MASK       (ESP_CODEC_DEV_MAKE_CHANNEL_MASK(0) | ESP_CODEC_DEV_MAKE_CHANNEL_MASK(1))
#define AUDIO_INPUT_REFERENCE_CHANNEL_MASK ESP_CODEC_DEV_MAKE_CHANNEL_MASK(2)
```

含义是对 ES7210 物理 MIC1/MIC2/MIC3 设置输入增益，其中 MIC3 作为硬件 reference。

## 5. 关键配置

文件：`main/boards/esp32s3-korvo-2/config.h`

```c
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

#define AUDIO_INPUT_MIC_CHANNEL_MASK       (ESP_CODEC_DEV_MAKE_CHANNEL_MASK(0) | ESP_CODEC_DEV_MAKE_CHANNEL_MASK(1))
#define AUDIO_INPUT_REFERENCE_CHANNEL_MASK ESP_CODEC_DEV_MAKE_CHANNEL_MASK(2)
```

`0x07` 表示 ES7210 选择 MIC1、MIC2、MIC3。

## 6. Codec 初始化逻辑

文件：`main/boards/esp32s3-korvo-2/korvo2_audio_codec.cc`

Korvo-2 硬件 reference 模式下，不再通过“麦克风数 + reference”推导 AFE 格式，而是直接使用板级配置的 `RMNM`。

```c++
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
```

这样可以保证以下接口一致：

- `codec->input_channels() == 4`
- `codec->GetAfeInputFormat() == "RMNM"`
- `codec->near_end_input_channels() == 2`
- `codec->afe_input_channels() == 4`
- `codec->raw_capture_channel_index() == 1`

## 7. I2S RX 配置

文件：`main/boards/esp32s3-korvo-2/korvo2_audio_codec.cc`

Korvo-2 当前使用 STD stereo 32-bit RX：

```c++
#if AUDIO_KORVO2_RX_STD_32BIT_STEREO || AUDIO_KORVO2_RX_STD_32BIT_MONO
    i2s_std_config_t rx_std_cfg = std_cfg;
    rx_std_cfg.slot_cfg.data_bit_width = I2S_DATA_BIT_WIDTH_32BIT;
    rx_std_cfg.slot_cfg.slot_bit_width = I2S_SLOT_BIT_WIDTH_32BIT;
    rx_std_cfg.slot_cfg.slot_mode = AUDIO_KORVO2_RX_STD_32BIT_STEREO ? I2S_SLOT_MODE_STEREO : I2S_SLOT_MODE_MONO;
    rx_std_cfg.slot_cfg.slot_mask = AUDIO_KORVO2_RX_STD_32BIT_STEREO ? I2S_STD_SLOT_BOTH : I2S_STD_SLOT_LEFT;
    rx_std_cfg.slot_cfg.ws_width = I2S_DATA_BIT_WIDTH_32BIT;
    rx_std_cfg.gpio_cfg.dout = I2S_GPIO_UNUSED;
    rx_std_cfg.gpio_cfg.din = din;
#endif
```

输入 device open 时也按 32-bit、2 channel 打开：

```c++
esp_codec_dev_sample_info_t fs = {
    .bits_per_sample = kKorvo2I2sBitsPerSample,
    .channel = kKorvo2I2sReadChannels,
    .channel_mask = kKorvo2CaptureChannelMask,
    .sample_rate = static_cast<uint32_t>(input_sample_rate_),
    .mclk_multiple = 0,
};
```

在当前配置下：

- `kKorvo2I2sBitsPerSample = 32`
- `kKorvo2I2sReadChannels = 2`
- `kKorvo2CaptureSlots = 4`

因此 `Read()` 以 `int16_t` 视角读取时，每帧为 4 个 16-bit sample，正好对应 `RMNM`。

## 8. Read 打包逻辑

文件：`main/boards/esp32s3-korvo-2/korvo2_audio_codec.cc`

硬件 reference 且 logical channel 数等于 capture slot 数时，直接复制 raw lane，保留 `RMNM` 顺序，并清零 `N` lane：

```c++
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
```

这一步非常关键：AFE 的 `feed()` 收到的内存布局必须和 `input_format="RMNM"` 完全一致。

## 9. AFE 配置

文件：`main/audio/processors/afe_audio_processor.cc`

Korvo-2 硬件 reference 模式下，AFE 参数来自 board config：

```c++
input_format = AUDIO_KORVO2_AFE_INPUT_FORMAT;
afe_total_ch_num = AUDIO_KORVO2_AFE_TOTAL_CH_NUM;
afe_mic_num = AUDIO_KORVO2_AFE_MIC_NUM;
ref_num = AUDIO_KORVO2_AFE_REF_NUM;
```

当前 AFE 运行参数：

- `input_format = "RMNM"`
- `total_ch_num = 4`
- `mic_num = 2`
- `ref_num = 1`
- `aec_init = true`
- Korvo-2 当前使用 `AFE_TYPE_SR` + `AFE_MODE_LOW_COST` + `AEC_MODE_SR_LOW_COST`

## 10. 公共 AudioCodec 改动

文件：`main/audio/audio_codec.h`、`main/audio/audio_codec.cc`

公共层新增 AFE format override 支持。默认 override 为空，所以其他板子仍按原逻辑推导 `MR`、`MMR` 等格式，不受影响。

```c++
void SetAfeInputFormatOverride(const std::string& format) {
    afe_input_format_override_ = format;
}
```

```c++
std::string AudioCodec::GetAfeInputFormat() const {
    if (!afe_input_format_override_.empty()) {
        return afe_input_format_override_;
    }

    // 原有推导逻辑保持不变
}
```

override 存在时，公共 accessor 会按 override 解析：

- `near_end_input_channels()` 统计 `M` 数量。
- `afe_input_channels()` 返回 format 长度。
- `raw_capture_channel_index()` 返回第一个 `M` 的位置。

这保证了调试、Agora raw capture fallback、audio testing 和 voiceprint 等路径不会误把 `R` 当成近端 mic。

## 11. 运行时验证

### 编译验证

已执行：

```bash
source /Users/qiuyanli/esp/esp-idf554/export.sh
idf.py build
```

结果：

```text
Project build complete.
Generated /Users/qiuyanli/esp32s3-20260523/build/esp32s3-20260523.bin
```

### 期望日志

启动后重点看以下日志：

```text
Duplex I2S channels created, rx_std32=1 i2s_read_channels=2 capture_slots=4
Korvo-2 audio initialized, ... logical_channels=4 ... i2s_read_channels=2 i2s_bits=32 ... format=RMNM
AFE initialized ... input_format=RMNM total_ch=4 mic=2 ref=1 hw_ref=1 sw_ref=0
Korvo-2 raw slots: ... format=RMNM slots=[ref:s0 mic0:s1 mic1:s3] rx_std32=1 hw_ref=1 sw_ref=0
```

### 简单功能验证

1. 播放远端语音或 TTS，使扬声器持续输出。
2. 本地近讲，确认上行声音仍清晰。
3. 不说话时播放远端语音，确认远端回声不再明显上行。
4. 查看 slot monitor：
   - `s0` 应随扬声器播放变化，是 reference 候选。
   - `s1`、`s3` 应随近端讲话变化。
   - `s2` 在送 AFE 前被清零，不参与算法。

## 12. 排查建议

### AEC 仍然弱或无效

优先检查：

- 日志里的 `input_format` 是否为 `RMNM`。
- AFE 日志里的 `total_ch=4 mic=2 ref=1` 是否正确。
- `rx_std32=1 i2s_read_channels=2 capture_slots=4` 是否正确。
- slot monitor 里 `s0` 是否随扬声器输出变化。
- ES8311 reference route 寄存器日志是否存在：

```text
ES8311 AEC reference route: reg13=... reg1b=... reg1c=... gpio44=...
```

如果 `s0` 和播放信号相关性不高，可以临时打开：

```c
#define AUDIO_KORVO2_REF_PROBE 1
```

然后观察 `Korvo-2 ref probe` 日志，确认哪个 slot 和 playback 相关性最高。若 best slot 不是 `s0`，需要根据实际硬件重新调整：

```c
#define AUDIO_KORVO2_REFERENCE_SLOT ...
#define AUDIO_KORVO2_MIC_SLOT ...
#define AUDIO_KORVO2_MIC1_SLOT ...
#define AUDIO_KORVO2_AFE_INPUT_FORMAT ...
```

### 不要回退到 MR

Korvo-2 官方 ADF 模型是 `RMNM`。如果回退到 `MR`，看似通道数更简单，但会丢失第二路 mic 或把 reference 放错位置，AEC 很容易失效。

### 不要混淆两种 mask

错误示例：

```c
// 错误方向：把 AFE lane 1/3 当成 ES7210 物理 mic mask
#define AUDIO_INPUT_MIC_CHANNEL_MASK (BIT(1) | BIT(3))
```

正确方向：

```c
// ES7210 物理 MIC1/MIC2 是近端 mic，MIC3 是 reference
#define AUDIO_INPUT_MIC_CHANNEL_MASK       (ESP_CODEC_DEV_MAKE_CHANNEL_MASK(0) | ESP_CODEC_DEV_MAKE_CHANNEL_MASK(1))
#define AUDIO_INPUT_REFERENCE_CHANNEL_MASK ESP_CODEC_DEV_MAKE_CHANNEL_MASK(2)
```

## 13. 相关文件

本工程内相关文件：

- `main/boards/esp32s3-korvo-2/config.h`
  - Korvo-2 AEC 通道、ES7210 选择、slot 映射配置。
- `main/boards/esp32s3-korvo-2/korvo2_audio_codec.cc`
  - ES8311/ES7210 初始化。
  - I2S RX/TX 创建。
  - input/output device open。
  - raw slot 到 AFE input 的打包。
  - slot monitor/ref probe。
- `main/audio/audio_codec.h`
  - AFE format override。
  - channel accessor 解析。
- `main/audio/audio_codec.cc`
  - `GetAfeInputFormat()` override 入口。
- `main/audio/processors/afe_audio_processor.cc`
  - Korvo-2 AFE 参数应用。
  - AEC enable/disable。
  - AFE feed/fetch。
- `main/agora/agora_audio_bridge.cc`
  - 没有 AFE 处理时的 raw mic fallback。
  - 当前 device AEC 路径下，上行应使用 AFE 处理后的 PCM。

## 14. 集成检查清单

移植或重新集成 Korvo-2 AEC 时，按下面顺序检查：

1. `sdkconfig`：
   - `CONFIG_BOARD_TYPE_ESP_KORVO_2=y`
   - `CONFIG_USE_AUDIO_PROCESSOR=y`
   - `CONFIG_USE_DEVICE_AEC=y`
2. `config.h`：
   - `AUDIO_INPUT_REFERENCE true`
   - `AUDIO_KORVO2_AFE_INPUT_FORMAT "RMNM"`
   - `AUDIO_KORVO2_AFE_TOTAL_CH_NUM 4`
   - `AUDIO_KORVO2_AFE_MIC_NUM 2`
   - `AUDIO_KORVO2_AFE_REF_NUM 1`
   - `AUDIO_KORVO2_RX_STD_32BIT_STEREO 1`
   - `AUDIO_KORVO2_ES7210_MIC_SELECTED 0x07`
3. codec 初始化：
   - 设置 `SetAfeInputFormatOverride("RMNM")`。
   - `input_channels_ = 4`。
4. I2S RX：
   - STD stereo。
   - 32-bit data width。
   - read channel 为 2。
5. Read path：
   - 每帧 4 个 `int16_t` lane。
   - 输出顺序为 `RMNM`。
   - `N` lane 清零。
6. AFE：
   - `input_format=RMNM`
   - `total_ch=4`
   - `mic=2`
   - `ref=1`
7. 运行日志：
   - reference slot 随播放变化。
   - mic slot 随近讲变化。
   - AEC 开启后远端回声显著降低。

