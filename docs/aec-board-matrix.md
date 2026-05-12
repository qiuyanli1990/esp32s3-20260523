# AEC Board Matrix

This matrix is keyed to the **current runtime code path**, not to a full
schematic audit of every board.

## Legend

- `hardware_ref`: the runtime sees a reference lane in the capture stream and
  will log `hardware input reference`.
- `software_ref(codec)`: the codec synthesizes a reference lane from playback
  internally before the data reaches the common AFE path.
- `software_ref(common)`: the board reports no hardware reference lane, and the
  common speaker tap in `AudioCodec::SetOutputTap()` provides the reference.
- `raw_only`: the current codec path does not expose a supported device-side
  AEC route.

## Validated Boards

| Board | Current mode | Status | Notes |
| --- | --- | --- | --- |
| `esp-vocat` | `software_ref(common)` | validated | Fixed by changing the board to stop pretending a second mic lane was a hardware AEC reference. |
| `sensecap-watcher` | `hardware_ref` | validated | Current Watcher build works on the existing hardware-reference path. |

## Codec Family Matrix

| Mode | Codec / condition | Boards | Status | Notes |
| --- | --- | --- | --- | --- |
| `hardware_ref` | `SensecapAudioCodec` with `AUDIO_INPUT_REFERENCE=true` | `sensecap-watcher` | validated | Current Watcher path is the reference implementation for this family. |
| `hardware_ref` | `BoxAudioCodec` with `AUDIO_INPUT_REFERENCE=true` | `esp-box`, `esp-box-3`, `esp32s3-korvo2-v3`, `esp32s3-korvo2-v3-rndis`, `kevin-box-2`, `tudouzi`, `zhengchen-cam`, `zhengchen-cam-ml307`, `waveshare/esp32-c6-touch-lcd-1.83`, `waveshare/esp32-p4-wifi6-touch-lcd`, `waveshare/esp32-s3-audio-board`, `waveshare/esp32-s3-rlcd-4.2`, `waveshare/esp32-s3-touch-amoled-1.75`, `waveshare/esp32-s3-touch-amoled-2.06`, `waveshare/esp32-s3-touch-lcd-1.54`, `waveshare/esp32-s3-touch-lcd-1.83`, `waveshare/esp32-s3-touch-lcd-1.85c`, `waveshare/esp32-s3-touch-lcd-3.49`, `waveshare/esp32-s3-touch-lcd-4b` | inferred | These boards currently tell the runtime that one capture lane is a reference lane. |
| `software_ref(common)` | `BoxAudioCodec` with `AUDIO_INPUT_REFERENCE=false` | `esp-vocat`, `waveshare/esp32-c6-touch-amoled-1.43`, `waveshare/esp32-c6-touch-amoled-2.06` | `esp-vocat` validated, others inferred | These boards now rely on the common speaker tap rather than a capture reference lane. |
| `software_ref(codec)` | `BoxAudioCodecLite` with `AUDIO_INPUT_REFERENCE=CONFIG_USE_AUDIO_PROCESSOR` | `esp-box-lite` | inferred | The codec explicitly says the board has no hardware loopback and injects playback from an internal buffer. |
| `hardware_ref` | `CoreS3AudioCodec` with `AUDIO_INPUT_REFERENCE=true` | `m5stack-core-s3` | inferred | Runtime treats the returned second lane as the AEC reference. |
| `hardware_ref` | `Tab5AudioCodec` with `AUDIO_INPUT_REFERENCE=true` | `m5stack-tab5` | inferred | Same runtime model as the ES7210/box family. |
| `hardware_ref` | `K10AudioCodec` with `AUDIO_INPUT_REFERENCE=true` | `df-k10` | inferred | Runtime currently treats lane 1 as reference. |
| `hardware_ref` | `Es8388AudioCodec` with `input_reference=true` | `atk-dnesp32s3`, `atk-dnesp32s3m-wifi`, `atk-dnesp32s3m-4g`, `labplus-ledong-v2`, `labplus-mpython-v3`, `yunliao-s3` | inferred | The codec opens two input channels and marks channel 1 as reference when enabled. |
| `hardware_ref` | custom stereo/PDM duplex codecs with `input_reference=true` | `lilygo-t-circle-s3`, `lilygo-t-display-s3-pro-mvsrlora`, `lilygo-t-cameraplus-s3 v1.0/v1.1` | inferred | These custom codecs currently report a reference lane to the runtime, but they still need board-level validation. |
| `software_ref(common)` | custom duplex codecs with `input_reference=false` | `lilygo-t-cameraplus-s3 v1.2` | inferred | Version `v1.2` switches to the generic speaker-tap path. |
| `software_ref(common)` | `Es8311AudioCodec` family | `aipi-lite`, `atom-echos3r`, `atommatrix-echo-base`, `atoms3-echo-base`, `atoms3r-cam-m12-echo-base`, `atoms3r-echo-base`, `esp-spot`, `esp-p4-function-ev-board`, `jiuchuan-s3`, `kevin-c3`, `kevin-sp-v4-dev`, `kevin-yuying-313lcd`, `lichuang-c3-dev`, `magiclick-2p4`, `magiclick-2p5`, `magiclick-c3`, `magiclick-c3-v2`, `movecall-cuican-esp32s3`, `movecall-moji-esp32s3`, `movecall-moji2-esp32c5`, `sp-esp32-s3-1.28-box`, `sp-esp32-s3-1.54-muma`, `surfer-c3-1.14tft`, `waveshare/esp32-c6-lcd-1.69`, `waveshare/esp32-c6-touch-amoled-1.32`, `waveshare/esp32-c6-touch-amoled-1.8`, `waveshare/esp32-p4-nano`, `waveshare/esp32-s3-epaper-1.54`, `waveshare/esp32-s3-epaper-3.97`, `waveshare/esp32-s3-touch-amoled-1.32`, `waveshare/esp32-s3-touch-amoled-1.8`, `waveshare/esp32-s3-touch-lcd-3.5`, `waveshare/esp32-s3-touch-lcd-3.5b`, `waveshare/esp32-touch-lcd-3.5`, `wireless-tag-wtp4c5mp07s`, `xingzhi-abs-2.0`, `xingzhi-metal-1.54-wifi`, `xmini-c3`, `xmini-c3-v3`, `xmini-c3-4g`, `genjutech-s3-1.54tft` | inferred | `Es8311AudioCodec` itself hardcodes `input_reference_=false`, so these boards only have the common software-reference path. |
| `software_ref(common)` | `Es8389AudioCodec` family | `atk-dnesp32s3-box2-wifi`, `atk-dnesp32s3-box2-4g` | inferred | Same as the ES8311 family from the runtime point of view: duplex, but no hardware reference lane. |
| `raw_only` | `AdcPdmAudioCodec` family | `esp-hi`, `esp-sensairshuttle` | inferred | These codecs do not currently advertise a supported device-side AEC path. |
| `raw_only` | simplex boards using `NoAudioCodecSimplex` | `bread-compact-*`, `du-chatx`, `electron-bot`, `hu-087`, `kevin-sp-v3-dev`, `lceda-course-examples/*`, `minsi-k08-dual`, `taiji-pi-s3`, `waveshare/esp32-s3-touch-lcd-1.46`, `waveshare/esp32-s3-touch-lcd-1.85`, `xingzhi-cube-*`, `zhengchen-1.54tft-*` | inferred | These boards are not on the device-side AEC path today. |

## Non-obvious Rules

- Do **not** infer AEC support from `AUDIO_INPUT_REFERENCE` alone.
- `Es8311AudioCodec` hardcodes `input_reference_=false`, so any board using it is
  on the common software-reference path, even if the local board header still
  defines `AUDIO_INPUT_REFERENCE`.
- `Es8389AudioCodec` also hardcodes `input_reference_=false`.
- `esp-box-lite` is special: it reports a reference lane to the runtime, but the
  reference is generated in the codec from playback, not from a hardware loopback.
- `esp-vocat` is also special: it uses `BoxAudioCodec`, but it must stay on
  `software_ref(common)` because the second capture lane is not a clean hardware
  AEC reference on this board.
