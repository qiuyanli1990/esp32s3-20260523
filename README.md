# ESP32 Multi-Board AI Conversation Firmware

([中文](README_zh.md) | English)

## Overview

This project is an AI conversation firmware for multiple ESP32 development boards, with the current focus on ESP32-S3 voice/display boards.

The current main flow is: create an Agora convo agent session through the Sentino API, then use Agora RTC on the firmware side for real-time audio, video, and datastream interaction. Generic session logic is mainly implemented in `main/application.*` and `main/agora/`. Board-specific logic is concentrated under `main/boards/<board>/`.

## Features

- Real-time voice conversation: microphone capture, AEC, uplink and downlink audio
- Optional voiceprint enrollment upload: records a voice sample before conversation start and uploads it to OSS to produce a `sample_url`
- Real-time video uplink: camera-enabled boards can send video streams
- Display pipeline: supports `EmoteDisplay`, `LVGL/LcdDisplay`, and `OledDisplay`
- Datastream display control: supports `display_emotion`, `display_weather`, and compatible emotion-display aliases
- Asset system: fonts, emoji, emote, and weather assets are packed into the `assets` partition
- Board-level extension: buttons, backlight, battery, camera, LED, and network

## Conversation Flow

```text
User
  │ Voice / Button / Touch
  ▼
Board Firmware
  │
  ├─ Audio capture / playback
  ├─ Camera
  └─ Display
  │
  ▼
Application
  │ Device state machine / session control / datastream dispatch
  │
  ├─ Create a session through the Sentino API
  │
  └─ Join Agora RTC with the returned dynamic parameters
       │
       ├─ Real-time audio / video uplink
       ├─ Voice downlink playback
       └─ Datastream back to local display
            │
            ▼
       Cloud Agent
       ASR / LLM / TTS
```

## Code Structure

| Path | Purpose |
| --- | --- |
| `main/application.*` | Main device state machine, session start/stop, datastream dispatch |
| `main/agora/` | Sentino session creation, Agora RTC, audio/video bridge |
| `main/voiceprint/` | Voiceprint sample capture, VAD-based window selection, and OSS upload |
| `main/audio/` | Audio capture, playback, AEC, VAD, codec processing |
| `main/display/` | Emote, LVGL, OLED, and other display backends |
| `main/assets/` | `assets` partition loading and runtime asset access |
| `main/boards/<board>/` | Board-level GPIO, codec, display, camera, and network initialization |
| `docs/sentino-conversation.md` | Current conversation flow documentation |
| `docs/custom-board.md` | Custom board porting guide |

## Environment Setup

### 1. Install ESP-IDF

Required version: `ESP-IDF v5.5.4`

```bash
mkdir -p ~/esp
cd ~/esp
git clone -b v5.5.4 --recursive https://github.com/espressif/esp-idf.git
cd esp-idf
./install.sh esp32s3
source ./export.sh
```

Run this again whenever you open a new terminal:

```bash
source ~/esp/esp-idf/export.sh
```

If you use VS Code or Cursor, you can also install the ESP-IDF extension directly and set the SDK version to `5.5.4`.

### 2. Get the Source Code

```bash
git clone <your-repo-or-fork-url>
cd <repo-dir>
```

### 3. Dependencies

- The first `idf.py build` will fetch managed components according to `main/idf_component.yml` and the lock file.
- `Agora IoT SDK` is already included under `components/agora_iot_sdk/`; no extra copy step is required.

## Run the Project

### 1. Configure Backend Parameters

Register and log in at `studio.sentino.jp/home`, create your own agent, then obtain the API token and agent ID. When creating the agent ID, enable the weather query and device control tools so that the model can send weather and emotion display commands.

Then update `main/agora_project_config.h`:

- `SENTINO_API_BEARER_TOKEN`: required; use your own API token
- `SENTINO_USER_ID`: business-side user identifier; leave empty to use `user_<board-uuid>`
- `SENTINO_DEVICE_ID`: business-side device identifier; leave empty to use `device_<board-uuid>`
- `SENTINO_API_BASE_URL`: keep the current default value

Also confirm these settings:

- `SENTINO_DEFAULT_AGENT_ID`: default agent ID
- `SENTINO_AGENT_OPTIONS`: optional runtime agent list on the device; the selection window opens before conversation start when at least two options are configured
- `SENTINO_ENABLE_VOICEPRINT`: whether to run voiceprint enrollment/upload before conversation start; default is `0`
- `SENTINO_LANGUAGES_JSON`, `SENTINO_GREETING_MESSAGE`: optional language list and greeting message sent in the Sentino conversation request

Example:

```cpp
#define SENTINO_DEFAULT_AGENT_ID "your-default-agent-id"

static constexpr SentinoAgentOptionConfig SENTINO_AGENT_OPTIONS[] = {
    {"your-agent-id-a", "Agent A"},
    {"your-agent-id-b", "Agent B"},
};
```

If `SENTINO_AGENT_OPTIONS` is empty or contains only one option, no runtime selection window is shown. The selected agent ID is persisted in NVS under `wifi/agent_id` and reused after reboot; select another agent at runtime, or run `idf.py erase-flash flash monitor`, to clear an old selection.

### 2. Configure Voiceprint Enrollment Upload (Optional)

Adds an optional voiceprint enrollment upload flow. When `SENTINO_ENABLE_VOICEPRINT` is enabled and no cached sample URL exists in NVS, the firmware:

- plays the voiceprint start prompt
- records up to 30 seconds of microphone PCM at 16 kHz
- selects the best 15-second window with enough effective speech
- uploads raw 16-bit mono `.pcm` data to Aliyun OSS through a signed `PutObject` request
- saves the uploaded sample URL in the `voiceprint` NVS namespace for later sessions

Configure these values in `main/agora_project_config.h`:

- `SENTINO_ENABLE_VOICEPRINT`: default is `0`; set it to `1` only after configuring OSS
- `VOICEPRINT_OSS_BUCKET`: your OSS bucket name
- `VOICEPRINT_OSS_ENDPOINT`: region endpoint, for example `oss-cn-shanghai.aliyuncs.com`
- `VOICEPRINT_OSS_OBJECT_PREFIX`: object key prefix, default `voiceprints`
- `VOICEPRINT_OSS_PUBLIC_BASE_URL`: optional public or CDN base URL; leave empty to use `https://<bucket>.<endpoint>`
- `VOICEPRINT_STS_ACCESS_KEY_ID`, `VOICEPRINT_STS_ACCESS_KEY_SECRET`, `VOICEPRINT_STS_SECURITY_TOKEN`: temporary STS credentials used to sign the OSS upload request

The current firmware-side upload code is a direct OSS upload POC. Do not commit real AK/SK, STS token, or long-lived AccessKey credentials to GitHub. For customer deployments, decide according to your backend and security model whether the device should request short-lived, least-privilege STS credentials from your service before uploading, or whether the device should upload the voiceprint audio to your service and let the service store it in OSS. The firmware can be extended to request and refresh STS credentials before calling the OSS upload path.

Voiceprint audio may be biometric or otherwise sensitive data. Production deployments should handle user consent, retention, access control, transport security, and storage policy at the business-service layer.

### 3. List Supported Boards

```bash
python3 scripts/select_board.py --list-boards
```

Common boards in this project:

| Board | `select_board` argument | Notes |
| --- | --- | --- |
| SenseCAP Watcher | `sensecap-watcher` | Round LCD, knob |
| ESP-VOCAT | `esp-vocat` | Round LCD, touch |
| ESP32-S3-Korvo2 V3 | `esp32s3-korvo2-v3` | Korvo version with LCD/camera |

For the older ESP32-S3-Korvo-2 audio board, use `esp32s3-korvo-2`.

### 4. Select a Board and Build

First choose one board-selection command:

```bash
# Watcher
python3 scripts/select_board.py sensecap-watcher

# VOCAT
python3 scripts/select_board.py esp-vocat

# Korvo2 V3
python3 scripts/select_board.py esp32s3-korvo2-v3
```

Then build and flash:

```bash
idf.py build
idf.py flash monitor
```

After board selection, the root `sdkconfig` and `build/` directory are for the active board. `idf.py flash` also flashes the board-specific `build/generated_assets.bin` into the `assets` partition, so display assets follow the selected board after a fresh build.

### 5. Switch Agent ID at Runtime

First configure at least two entries in `SENTINO_AGENT_OPTIONS` in `main/agora_project_config.h`. When the device is idle and network-connected, press the conversation button to start a session. If voiceprint is enabled, the firmware completes the voiceprint flow first, then shows `Agent: <label>` and opens a 5-second selection window.

| Board | Start conversation | Next agent during selection | Confirm |
| --- | --- | --- | --- |
| Watcher | Short-press the knob button | Rotate the knob, or short-press the knob button again | Stop input for about 5 seconds |
| VOCAT | Tap the touch screen, or short-press BOOT | Tap the touch screen again, or short-press BOOT | Stop input for about 5 seconds |
| Korvo2 V3 | Press REC, or short-press BOOT | Press REC again, or short-press BOOT | Stop input for about 5 seconds |

After confirmation, the firmware saves the selected agent ID and continues creating the Sentino conversation. On Watcher, long-pressing the knob is for power/factory-reset behavior; on Korvo, SET enters Wi-Fi provisioning and is not used for agent selection.

## Porting a New Board

### 1. Choose the Network Base Class

- `WifiBoard`: Wi-Fi-only boards
- `Ml307Board`: boards with an ML307 / ML307R 4G module
- `DualNetworkBoard`: boards that support both Wi-Fi and ML307

### 2. Create the Board Directory

Create a new directory under `main/boards/`. It usually contains at least:

- `config.h`
- `<board>.cc`
- `config.json`
- `sdkconfig.defaults`
- `README.md`

### 3. Write `config.h`

Put board-level hardware facts here:

- I2S / I2C / SPI / UART pins
- Audio sample rate
- Display size, rotation, and mirror settings
- Button, backlight, LED, battery, and camera pins
- Codec address and amplifier control pins

Keep hardware definitions centralized in `config.h`. Avoid hardcoding pins directly in `.cc` files.

### 4. Implement the Board Class

In `<board>.cc`, implement:

- Select the base class: `WifiBoard`, `Ml307Board`, or `DualNetworkBoard`
- Initialize `AudioCodec`
- Initialize `Display`
- Initialize `Backlight` / `Button` / `Camera` / `Power` / `Network`
- Register the board with `DECLARE_BOARD(...)`

### 5. Register the Board Type

Update both files:

1. `main/Kconfig.projbuild`
   Add `BOARD_TYPE_<YOUR_BOARD>`.
2. `main/CMakeLists.txt`
   Add the board branch and specify `BOARD_TYPE`, fonts, and default asset configuration.

### 6. Choose the Display Pipeline

#### Emote Pipeline

Best for screens that should always show an animated expression:

- Create `EmoteDisplay` in the board implementation
- Enable `CONFIG_USE_EMOTE_MESSAGE_STYLE=y`
- Enable `CONFIG_FLASH_EXPRESSION_ASSETS=y`

#### LVGL / OLED Pipeline

Best for information cards, menus, and image previews:

- Create `SpiLcdDisplay`, `RgbLcdDisplay`, or `OledDisplay`
- Enable `CONFIG_FLASH_DEFAULT_ASSETS=y`

### 7. Configure AEC for a Custom Board

If your board is designed for full-duplex voice conversation, define the AEC path explicitly.

#### Decide Whether to Enable Device-Side AEC

Configure this in `main/boards/<board>/sdkconfig.defaults`:

```text
CONFIG_USE_DEVICE_AEC=y
```

Do not enable it if the board does not have a stable microphone capture and speaker playback path, or if the board is not intended for conversation.

#### Decide `AUDIO_INPUT_REFERENCE`

Define this in `config.h`:

```c
#define AUDIO_INPUT_REFERENCE true
```

or:

```c
#define AUDIO_INPUT_REFERENCE false
```

Decision rules:

- `true`: the hardware capture path has a dedicated playback reference channel that can be sent directly to runtime AEC
- `false`: there is no independent reference channel; use the common software reference path

You can refer to these existing boards:

- `sensecap-watcher`: `AUDIO_INPUT_REFERENCE=true`
- `esp-vocat`: `AUDIO_INPUT_REFERENCE=true`
- `esp32s3-korvo2-v3`: `AUDIO_INPUT_REFERENCE=true`
- `esp32s3-korvo-2`: `AUDIO_INPUT_REFERENCE=true`

#### Pass the Value to the Codec

When creating `AudioCodec` in the board implementation, pass `AUDIO_INPUT_REFERENCE` so that the board definition and runtime behavior stay consistent.

#### Validate on Real Hardware

At minimum, verify:

- Local speech is still recognized reliably while the speaker is playing
- Remote echo does not leak significantly into uplink audio
- AEC state remains stable during mute, interruption, and continuous conversation

### 8. Build and Verify

```bash
python3 scripts/select_board.py my-custom-board
idf.py build
idf.py flash monitor
```

## References

- `docs/sentino-conversation.md`
- `docs/custom-board.md`
