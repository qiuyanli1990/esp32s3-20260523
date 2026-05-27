# Build Instructions

## One-click Build

```bash
python scripts/release.py sensecap-watcher -c config_en.json
```

`release.py` automatically:

- runs `idf.py set-target esp32s3`
- reads `sdkconfig_append` from [config_en.json](config_en.json)
- appends the board-specific required config items

## Agora SDK Location

If this board build depends on `Agora IoT SDK`, keep a single SDK copy under `components/agora_iot_sdk/`.

Reason:

- ESP-IDF discovers local SDKs from `components/` without extra CMake path wiring
- the headers, static libraries, and local compatibility code stay together in one place
- avoiding a second copy under `third_party/` prevents version drift and path confusion

The repository now uses `components/agora_iot_sdk/` as the active Agora SDK location.

## Manual Configuration and Build

```bash
idf.py set-target esp32s3
```

Note: if you skip this step, `Seeed Studio SenseCAP Watcher` will not appear in `menuconfig`.

**Configuration**

```bash
idf.py menuconfig
```

Select the board:

```
Xiaozhi Assistant -> Board Type -> SenseCAP Watcher
```

The board-specific extra settings are already defined in [config_en.json](config_en.json).

If you use `release.py`, they are appended automatically.
If you build manually, make sure your config matches them:

```
CONFIG_BOARD_TYPE_SEEED_STUDIO_SENSECAP_WATCHER=y
CONFIG_ESPTOOLPY_FLASHSIZE_32MB=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions/v2/32m.csv"
CONFIG_BOOTLOADER_CACHE_32BIT_ADDR_QUAD_FLASH=y
CONFIG_ESPTOOLPY_FLASH_MODE_AUTO_DETECT=n
CONFIG_IDF_EXPERIMENTAL_FEATURES=y
CONFIG_LANGUAGE_EN_US=y
CONFIG_SR_WN_WN9_JARVIS_TTS=y
```

## Build and Flash

```bash
idf.py -DBOARD_NAME=sensecap-watcher-en build flash
```

Note: If your device was previously shipped with the SenseCAP firmware (not the Xiaozhi version), please be very careful with the flash partition addresses to avoid accidentally erasing the device information (such as EUI) of the SenseCAP Watcher. Otherwise, even if you restore the SenseCAP firmware, the device may not be able to connect to the SenseCraft server correctly! Therefore, before flashing the firmware, be sure to record the necessary device information to ensure you have a way to recover it!

You can use the following command to back up the factory information:

```bash
# Firstly backup the factory information partition which contains the credentials for connecting the SenseCraft server
esptool.py --chip esp32s3 --baud 2000000 --before default_reset --after hard_reset --no-stub read_flash 0x9000 204800 nvsfactory.bin
```
