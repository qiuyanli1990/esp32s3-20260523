# 编译命令

## 开发模式

如果你是在本地反复调试 Watcher，先运行一次：

```bash
python scripts/select_board.py sensecap-watcher
```

之后就可以直接使用根目录开发流：

```bash
idf.py menuconfig
idf.py build
idf.py flash monitor
```

如果之后切到别的板子，再重新执行一次 `select_board.py`。

## 一键编译

```bash
python scripts/release.py sensecap-watcher
```

`release.py` 会自动：

- 在隔离的 `.build/sensecap-watcher` 目录里构建
- 用项目根 `sdkconfig.defaults`、`sdkconfig.defaults.esp32s3` 和 [main/boards/sensecap-watcher/sdkconfig.defaults](sdkconfig.defaults) 生成独立的 `.sdkconfig/sensecap-watcher.sdkconfig`
- 自动补上这块板子对应的 `CONFIG_BOARD_TYPE_SEEED_STUDIO_SENSECAP_WATCHER=y`

一键编译完成后，烧录也要指向对应的变体构建目录：

```bash
idf.py -B .build/sensecap-watcher flash monitor
```

不要直接运行裸的 `idf.py flash monitor`，因为那会默认使用根目录的 `build/`，不是 `release.py` 生成的 Watcher 构建目录。

## Agora SDK 目录说明

如果这块板子的业务功能依赖 `Agora IoT SDK`，请把 SDK 文件保存在 `components/agora_iot_sdk/`，不要再保留第二份副本。

原因：

- ESP-IDF 会直接从 `components/` 发现本地 SDK，不需要额外改 CMake 搜索路径
- SDK 头文件、静态库和本地兼容补丁可以放在同一处维护
- 避免 `components/` 和 `third_party/` 各放一份导致版本混乱

当前仓库已经按这个方式组织 Agora SDK，实际生效路径是 `components/agora_iot_sdk/`。

## 手动配置编译

如果你想继续使用根目录的标准 ESP-IDF 开发流，推荐直接运行：

```bash
python scripts/select_board.py sensecap-watcher
```

它会把板级 `sdkconfig.defaults` 合并进根目录 `sdkconfig`，并重配根 `build/`。然后你就可以直接执行：

```bash
idf.py menuconfig
idf.py build
idf.py flash monitor
```

如果你坚持完全手动配置，再从下面开始：

```bash
idf.py set-target esp32s3
```

注意：如果不先执行这一步，`menuconfig` 里的板型列表不会出现 `Seeed Studio SenseCAP Watcher`。

**配置**

```bash
idf.py menuconfig
```

选择板子

```
Xiaozhi Assistant -> Board Type -> SenseCAP Watcher
```

Watcher 的板级额外配置已经写在 [sdkconfig.defaults](sdkconfig.defaults) 里。`release.py` 会自动把它们并进独立的 `.sdkconfig/sensecap-watcher.sdkconfig`；如果你手动跑 `idf.py`，需要自己确认下面这些值。

如果你使用 `release.py`，下面这些值会自动带入；
如果你坚持手动编译，请确保它们和 [sdkconfig.defaults](sdkconfig.defaults) 保持一致：

```
CONFIG_ESPTOOLPY_FLASHSIZE_32MB=y
CONFIG_PARTITION_TABLE_CUSTOM=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions/v2/32m.csv"
CONFIG_BOOTLOADER_CACHE_32BIT_ADDR_QUAD_FLASH=y
CONFIG_ESPTOOLPY_FLASH_MODE_AUTO_DETECT=n
CONFIG_IDF_EXPERIMENTAL_FEATURES=y
CONFIG_FREERTOS_HZ=1000
CONFIG_USE_DEVICE_AEC=y
```

## 编译烧入

```bash
idf.py build flash
```

注意: 如果当前设备出货之前是SenseCAP 固件(非小智版本),请特别小心处理闪存固件分区地址，以避免错误擦除 SenseCAP Watcher 的自身设备信息（EUI 等），否则设备即使恢复成SenseCAP固件也无法正确连接到 SenseCraft 服务器！所以在刷写固件之前，请务必记录设备的相关必要信息，以确保有恢复的方法！

您可以使用以下命令备份生产信息

```bash
# firstly backup the factory information partition which contains the credentials for connecting the SenseCraft server
esptool.py --chip esp32s3 --baud 2000000 --before default_reset --after hard_reset --no-stub read_flash 0x9000 204800 nvsfactory.bin

```
