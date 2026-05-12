# ESP32-S3 Multi-Board AI Conversation Firmware

([中文](README_zh.md) | English | [日本語](README_ja.md))

## Overview

This project is an AI conversation firmware for multiple ESP32-S3 development boards.

The current main flow is: create an Agora convo agent session through the Sentino API, then use Agora RTC on the firmware side for real-time audio, video, and datastream interaction. Generic session logic is mainly implemented in `main/application.*` and `main/agora/`. Board-specific logic is concentrated under `main/boards/<board>/`.

## Features

- Real-time voice conversation: microphone capture, AEC, uplink and downlink audio
- Real-time video uplink: camera-enabled boards can send video streams
- Display pipeline: supports `EmoteDisplay`, `LVGL/LcdDisplay`, and `OledDisplay`
- Datastream display control: supports `display_emotion` and `display_weather`
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
- `SENTINO_USER_ID`: use a random string
- `SENTINO_DEVICE_ID`: use a random integer
- `SENTINO_API_BASE_URL`: keep the current default value

Also confirm these settings:

- `CONFIG_SENTINO_DEFAULT_AGENT_ID`: set this to your own agent ID or an existing agent ID from the platform
- `CONFIG_SENTINO_AGENT_OPTIONS_JSON`: configure the optional agent list available on the device side

See `sdkconfig.defaults` for example configuration:

```text
CONFIG_SENTINO_DEFAULT_AGENT_ID="YOUR_AGENT_ID"
CONFIG_SENTINO_AGENT_OPTIONS_JSON="[]"
```

### 2. List Supported Boards

```bash
python scripts/select_board.py --list-boards
```

The current repository is adapted for `esp-vocat` and `sensecap-watcher`.

### 3. Select a Board and Build

Example: `esp-vocat`

```bash
python scripts/select_board.py esp-vocat
idf.py build
idf.py flash monitor
```

Example: `sensecap-watcher`

```bash
python scripts/select_board.py sensecap-watcher
idf.py build
idf.py flash monitor
```

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
- `esp-vocat`: `AUDIO_INPUT_REFERENCE=false`

#### Pass the Value to the Codec

When creating `AudioCodec` in the board implementation, pass `AUDIO_INPUT_REFERENCE` so that the board definition and runtime behavior stay consistent.

#### Validate on Real Hardware

At minimum, verify:

- Local speech is still recognized reliably while the speaker is playing
- Remote echo does not leak significantly into uplink audio
- AEC state remains stable during mute, interruption, and continuous conversation

### 8. Build and Verify

```bash
python scripts/select_board.py my-custom-board
idf.py build
idf.py flash monitor
```

## References

- `docs/sentino-conversation.md`
- `docs/custom-board.md`
