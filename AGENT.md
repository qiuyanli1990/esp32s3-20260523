# AGENT.md

## Project

- Repo: `esp32s3-20260523`
- Type: ESP-IDF firmware for ESP32-series AI conversation devices
- Primary target today: ESP32-S3 boards
- Required ESP-IDF version: `v5.5.4`
- Main flow: Sentino conversation API creates a dynamic Agora RTC session, then firmware joins RTC for real-time audio, optional video, and datastream display commands.

Generic conversation logic belongs in `main/application.*` and `main/agora/`. Board-specific hardware work belongs under `main/boards/<board>/`.

## Source Of Truth

Read these first before making non-trivial changes:

1. `README_zh.md` or `README.md`
2. `docs/sentino-conversation.md`
3. `docs/custom-board.md` when adding or changing board support
4. `docs/aec-board-matrix.md` when touching AEC, codec, microphone, speaker reference, or audio routing
5. `docs/code_style.md` when formatting C/C++ changes

If documentation and checked-in code disagree, prefer the checked-in code for immediate implementation details, then update the relevant doc if the task changes the intended behavior.

## Code Map

- `main/application.*`: device state machine, session start/stop, runtime agent selection, voiceprint-before-start flow, datastream parsing and display dispatch.
- `main/agora/`: Sentino conversation create/stop, Agora RTC session lifecycle, audio bridge, video bridge.
- `main/voiceprint/`: voiceprint PCM capture, VAD/RMS window selection, OSS upload.
- `main/audio/`: codec, capture/playback, AFE, AEC, wake word, realtime PCM services.
- `main/display/`: `EmoteDisplay`, LVGL/LCD, OLED, common display API.
- `main/assets.*` and `main/assets/`: assets partition loading, fonts, emoji, emote, weather assets.
- `main/boards/<board>/`: board GPIO, codec, display, camera, network, battery, button, LED setup.
- `components/agora_iot_sdk/`: vendored Agora IoT/RTC SDK libraries and headers.
- `scripts/select_board.py`: select one board for root development build.
- `scripts/release.py`: isolated variant builds under `.build/` with generated `.sdkconfig/`.

## Task Guidance

### Conversation Flow

- Keep Sentino request/response handling in `main/agora/agora_agent_manager.*`.
- Keep Agora RTC SDK lifecycle in `main/agora/agora_rtc_session.*`.
- Keep user-visible state and datastream UI dispatch in `main/application.*`.
- Session start order is: network ready, optional voiceprint, runtime agent selection, Sentino conversation create, Agora RTC join, audio/video bridge start.
- Session stop should clean up local RTC/audio/video first, then request Sentino conversation stop asynchronously.

### Board Porting

- Do not overwrite an existing board type for different hardware.
- Create or update `main/boards/<board>/config.h`, `<board>.cc`, `config.json`, and `sdkconfig.defaults`.
- Register new boards in both `main/Kconfig.projbuild` and `main/CMakeLists.txt`.
- Keep GPIO and hardware constants in `config.h`; avoid raw GPIO values scattered through `.cc` files.
- Choose the network base class deliberately: `WifiBoard`, `Ml307Board`, `DualNetworkBoard`, `RndisBoard`, or another existing common base.

### AEC And Audio

- Do not infer AEC support from `AUDIO_INPUT_REFERENCE` alone.
- Check `docs/aec-board-matrix.md` and the actual codec class before changing AEC behavior.
- Validate whether the board is on `hardware_ref`, `software_ref(codec)`, `software_ref(common)`, or `raw_only`.
- Preserve sample-rate consistency across board config, codec setup, AFE, and RTC PCM settings.
- Avoid per-frame heap allocation in realtime audio paths.

### Display And Assets

- Parse datastream actions in `main/application.cc`, not in display backends.
- Route `display_weather` through `Display::ShowWeather()`.
- Route `display_emotion`, `play_emotion`, `emotion`, and `display_emoji` through `Display::SetEmotion()`.
- For Emote assets, keep `CONFIG_MMAP_FILE_NAME_LENGTH >= 32`.
- Use `CONFIG_FLASH_EXPRESSION_ASSETS` for Emote boards and `CONFIG_FLASH_DEFAULT_ASSETS` for LVGL/OLED/default display assets.

### Voiceprint

- Voiceprint is optional and controlled by `SENTINO_ENABLE_VOICEPRINT` in `main/agora_project_config.h`.
- Voiceprint upload failure should not block conversation start unless the user explicitly asks for strict failure behavior.
- Treat voiceprint audio, OSS credentials, STS tokens, and sample URLs as sensitive.
- Do not commit real long-lived AccessKey credentials.

## Build And Verification

Source ESP-IDF before running build commands:

```bash
source ~/esp/esp-idf/export.sh
```

List supported boards:

```bash
python scripts/select_board.py --list-boards
```

Select a board for development:

```bash
python scripts/select_board.py esp-vocat
idf.py build
idf.py flash monitor
```

For `sensecap-watcher`:

```bash
python scripts/select_board.py sensecap-watcher
idf.py build
idf.py flash monitor
```

Release builds:

```bash
python scripts/release.py <board>
python scripts/release.py <board> --name <variant>
```

Release builds use `.build/<variant>` and `.sdkconfig/<variant>.sdkconfig`. Flash release builds with the exact `idf.py -B .build/<variant> ...` command printed by `scripts/release.py`; plain `idf.py flash monitor` uses root `build/`.

When making meaningful firmware changes, run at least `idf.py build` for the active board before closing out. If hardware flashing or runtime verification is not possible, say so explicitly.

## Configuration And Secrets

- `main/agora_project_config.h` contains local Sentino and voiceprint configuration. Do not print secrets in final answers or logs.
- Keep real API tokens, OSS AK/SK, STS tokens, Wi-Fi credentials, and tenant-specific values out of commits.
- Prefer placeholders in tracked files and local ignored config or provisioning flows for real deployments.
- If changing user-facing config macros, update README/docs or examples in the same change.
- If stale NVS affects behavior, mention `idf.py erase-flash flash monitor` as a test step.

## Coding Style

- Follow the existing C++ style and `.clang-format`.
- Prefer existing board, codec, display, audio, and settings abstractions over new parallel systems.
- Keep changes scoped to the requested area. Avoid unrelated refactors.
- Add comments only for non-obvious sequencing, hardware constraints, or protocol details.
- Use structured parsers/APIs such as `cJSON` for JSON, not ad hoc string parsing unless the existing code path already requires normalization.

## Local Skills

This workspace includes project-specific Codex skills under `codex-skills/` and installed copies under `~/.codex/skills/`:

- `esp32-sentino-agora-conversation`
- `esp32-port-board`
- `esp32-debug-aec-audio`
- `esp32-voiceprint-oss`
- `esp32-display-assets`
- `esp32-build-release`

Use them as task-specific guidance when available.

## Change Policy

- Do not revert user changes unless explicitly asked.
- Do not delete generated or local artifacts unless the task requires it and the scope is clear.
- Do not touch unrelated projects outside this repo.
- If adding external components, keep them isolated under `components/` or another clearly scoped location and document the build impact.
- If a network-dependent command fails because of sandbox or network access, ask for approval to rerun it with the required access instead of changing project configuration to work around the failure.
