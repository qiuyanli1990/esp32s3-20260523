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

Then set the agent defaults in `main/agora_project_config.h`:

- `SENTINO_DEFAULT_AGENT_ID`
- `SENTINO_AGENT_OPTIONS`

Optional overrides:

- `SENTINO_USER_ID`
- `SENTINO_DEVICE_ID`
- `SENTINO_LANGUAGES_JSON`
- `SENTINO_GREETING_MESSAGE`
If `SENTINO_USER_ID` or `SENTINO_DEVICE_ID` is empty, firmware falls back to the persisted board UUID.

When a session starts, firmware completes the voiceprint step first only if `SENTINO_ENABLE_VOICEPRINT` is enabled in `main/agora_project_config.h`, then opens a short runtime agent-selection window before calling Sentino `POST /api/v1/conversations`. The selected value is stored in NVS under the `wifi` namespace and overrides `SENTINO_DEFAULT_AGENT_ID` for future conversations.

## Conversation Flow

1. Firmware enters Wi-Fi station mode or AP provisioning mode.
2. When a session starts, firmware records or loads the voiceprint sample only if voiceprint is enabled.
3. Firmware prompts for the active Sentino agent, then persists that selection.
4. Firmware calls Sentino `POST /api/v1/conversations`.
5. The response provides `conversation_id`, `channel`, `app_id`, `rtc_token`, and `rtc_uid`.
6. Firmware joins Agora RTC with those dynamic parameters.
7. Incoming stream messages are parsed for structured emotion actions first, then keyword/state fallbacks such as `thinking`, `silent`, `happy`, `sad`, and matching Chinese aliases.
8. When the session ends, firmware leaves RTC and asynchronously calls Sentino stop for the same `conversation_id`.

## Adding a New Board

When adapting to another ESP32-S3 board, keep the common conversation stack unchanged and only change:

- board directory under `main/boards/<new-board>/`
- GPIO / codec / display / camera init
- board Kconfig entry in `main/Kconfig.projbuild`
- any board-specific AEC reference path or mic/speaker routing

The rest of the conversation flow should stay in the common layer.
