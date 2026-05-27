# AEC Board Matrix

This document records the current runtime AEC path in this repository. It is
not a complete schematic audit of every supported board, so check the board
source and runtime logs before changing audio routing.

## Legend

- `hardware_ref`: the capture stream includes a reference lane and the runtime
  logs `hardware input reference`.
- `software_ref(codec)`: the codec creates a reference lane from playback before
  common AFE processing.
- `software_ref(common)`: the codec reports no hardware reference lane; common
  code feeds playback to AEC through `AudioCodec::SetOutputTap()`.
- `raw_only`: the current codec path does not expose a supported device-side AEC
  route.

## Current Focus Boards

| Board | Codec | `AUDIO_INPUT_REFERENCE` | Runtime mode | Status |
| --- | --- | --- | --- | --- |
| `esp-vocat` | `BoxAudioCodec` | `true` | `hardware_ref` | Current code path; validate on hardware before changing channel layout. |
| `sensecap-watcher` | `SensecapAudioCodec` | `true` | `hardware_ref` | Validated Watcher path. |
| `esp32s3-korvo2-v3` | `BoxAudioCodec` | `true` | `hardware_ref` | Current code path for Korvo-2 v3 style capture. |
| `esp32s3-korvo-2` | `Korvo2AudioCodec` | `true` | `hardware_ref` | Dedicated Korvo-2 RMNM path; see `docs/korvo2_aec_integration_code_zh.md`. |

## Codec Families

| Mode | Codec / condition | Notes |
| --- | --- | --- |
| `hardware_ref` | `SensecapAudioCodec` with `AUDIO_INPUT_REFERENCE=true` | Watcher is the validated example in this repo. |
| `hardware_ref` | `BoxAudioCodec` with `AUDIO_INPUT_REFERENCE=true` | Runtime treats one capture lane as speaker reference. Confirm the lane order on the target board. |
| `hardware_ref` | `Korvo2AudioCodec` with `AUDIO_INPUT_REFERENCE=true` | Korvo-2 maps ADF-style capture slots to mic/reference lanes explicitly. |
| `software_ref(codec)` | `BoxAudioCodecLite` with `AUDIO_INPUT_REFERENCE=CONFIG_USE_AUDIO_PROCESSOR` | `esp-box-lite` injects playback from an internal buffer rather than a hardware loopback. |
| `software_ref(common)` | `Es8311AudioCodec` family | `Es8311AudioCodec` hardcodes `input_reference_=false`, so boards using it rely on the common speaker tap. |
| `software_ref(common)` | `Es8389AudioCodec` family | Same runtime model as ES8311: duplex audio, no hardware reference lane. |
| `raw_only` | `AdcPdmAudioCodec` or simplex `NoAudioCodecSimplex` boards | No supported device-side AEC route is exposed by the current codec path. |

## Rules

- Do not infer AEC support from `AUDIO_INPUT_REFERENCE` alone.
- Confirm `CONFIG_USE_DEVICE_AEC=y` in the selected board config when testing
  device-side AEC.
- Confirm runtime logs after session start. The expected line should name either
  `hardware input reference` or the software speaker-tap path.
- Keep board channel order, codec `Read()` layout, and AFE input channel count
  aligned. Most hardware-reference bugs come from a wrong reference lane rather
  than from the AFE itself.
- For boards without a clean hardware loopback, prefer the common speaker tap
  over pretending a second mic lane is a reference lane.
