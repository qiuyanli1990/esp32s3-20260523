# Sentino Conversation Firmware Notes

`esp32s3-20260523` keeps the generic ESP32-S3 firmware structure from the multi-board watcher codebase, and switches the conversation entry from Agora conversational-agent HTTP APIs to Sentino `POST /api/v1/conversations`.

## Common Firmware Pieces

- AP provisioning: `main/boards/common/wifi_board.cc`
- Board abstraction and per-board pin init: `main/boards/<board>/`
- AEC / voice pipeline: `main/audio/processors/afe_audio_processor.cc`
- Sentino conversation create / stop: `main/agora/agora_agent_manager.cc`
- Agora RTC join with dynamic app id / token: `main/agora/agora_rtc_session.cc`
- Stream-message keyword to local emotion mapping: `main/application.cc`

## Required Configuration

Edit `main/agora_project_config.h` before building:

- `SENTINO_API_BEARER_TOKEN`

Then set the agent defaults in `sdkconfig` / `menuconfig`:

- `CONFIG_SENTINO_DEFAULT_AGENT_ID`
- `CONFIG_SENTINO_AGENT_OPTIONS_JSON`

Optional overrides:

- `SENTINO_USER_ID`
- `SENTINO_DEVICE_ID`
- `SENTINO_LANGUAGES_JSON`
- `SENTINO_GREETING_MESSAGE`
- `SENTINO_DEVICE_TIMEZONE`
- `SENTINO_DEVICE_LAT`
- `SENTINO_DEVICE_LON`

If `SENTINO_USER_ID` or `SENTINO_DEVICE_ID` is empty, firmware falls back to the persisted board UUID.

The AP provisioning page now exposes an `agentId` dropdown. The selected value is stored in NVS under the `wifi` namespace and overrides `CONFIG_SENTINO_DEFAULT_AGENT_ID` for future conversations.

## Conversation Flow

1. Firmware enters Wi-Fi station mode or AP provisioning mode.
2. When a session starts, firmware calls Sentino `POST /api/v1/conversations`.
3. The response provides `conversation_id`, `channel`, `app_id`, `rtc_token`, and `rtc_uid`.
4. Firmware joins Agora RTC with those dynamic parameters.
5. Incoming stream messages are parsed for structured emotion actions first, then keyword/state fallbacks such as `thinking`, `silent`, `happy`, `sad`, and matching Chinese aliases.
6. When the session ends, firmware leaves RTC and asynchronously calls Sentino stop for the same `conversation_id`.

## Adding a New Board

When adapting to another ESP32-S3 board, keep the common conversation stack unchanged and only change:

- board directory under `main/boards/<new-board>/`
- GPIO / codec / display / camera init
- board Kconfig entry in `main/Kconfig.projbuild`
- any board-specific AEC reference path or mic/speaker routing

The rest of the conversation flow should stay in the common layer.
