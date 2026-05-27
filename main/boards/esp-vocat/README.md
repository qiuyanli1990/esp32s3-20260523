# ESP-VoCat 喵伴

## 简介

<div align="center">
    <a href="https://oshwhub.com/esp-college/echoear"><b> 立创开源平台 </b></a>
</div>

ESP-VoCat 喵伴是一款智能 AI 开发套件，搭载 ESP32-S3-WROOM-1 模组，1.85 寸 QSPI 圆形触摸屏，双麦阵列，支持离线语音唤醒与声源定位算法。硬件详情等可查看[立创开源项目](https://oshwhub.com/esp-college/echoear)。

## 配置、编译命令

## 开发模式

如果你是在本地反复调试 VoCat，先运行一次：

```bash
python scripts/select_board.py esp-vocat
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
python scripts/release.py esp-vocat
```

`release.py` 会自动：

- 在隔离的 `.build/esp-vocat` 目录里构建
- 用项目根 `sdkconfig.defaults`、`sdkconfig.defaults.esp32s3` 和 [main/boards/esp-vocat/sdkconfig.defaults](sdkconfig.defaults) 生成独立的 `.sdkconfig/esp-vocat.sdkconfig`
- 自动补上 ESP-VoCat 对应的 `CONFIG_BOARD_TYPE_ESP_VOCAT=y`

当前这块板子的板级默认配置在 [sdkconfig.defaults](sdkconfig.defaults)：

```text
CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y
CONFIG_PARTITION_TABLE_CUSTOM=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions/v2/16m.csv"
CONFIG_USE_DEVICE_AEC=y
CONFIG_USE_EMOTE_MESSAGE_STYLE=y
CONFIG_MMAP_FILE_NAME_LENGTH=32
CONFIG_FLASH_EXPRESSION_ASSETS=y
```

这也是推荐用法，因为脚本不会再把配置硬写进共享 `sdkconfig`；每块板子都有自己的 `.sdkconfig/<variant>.sdkconfig` 和 `.build/<variant>`，切板时不会把之前保存过的 flash size、partition 和显示风格串进来。

一键编译完成后，烧录也要指向对应的变体构建目录：

```bash
idf.py -B .build/esp-vocat flash monitor
```

不要直接运行裸的 `idf.py flash monitor`，因为那会默认使用根目录的 `build/`，不是 `release.py` 生成的 VoCat 构建目录。

项目层现在也补了 VoCat 相关的 `Kconfig default`，所以在全新配置或重新配置时，切到 `Espressif ESP-VoCat` 会默认带上：

- `Enable Device-Side AEC`
- `Emote animation style`
- `Flash size = 16MB`
- `Custom partition CSV file = partitions/v2/16m.csv`
- `Max file name length = 32`

但 `.sdkconfig/esp-vocat.sdkconfig` 也是持久化文件；如果你之前手动改过这块板子的本地配置，旧值仍然可能保留，所以切板后最好还是顺手确认一次下面这些项。

## 手动配置编译

如果你想继续使用根目录的标准 ESP-IDF 开发流，推荐直接运行：

```bash
python scripts/select_board.py esp-vocat
```

它会把板级 `sdkconfig.defaults` 合并进根目录 `sdkconfig`，并重配根 `build/`。然后你就可以直接执行：

```bash
idf.py menuconfig
idf.py build
idf.py flash monitor
```

如果你坚持完全手动配置，推荐先从全新配置开始，再逐项确认下面这些值：

```bash
idf.py set-target esp32s3
idf.py menuconfig
```

至少确认下面这些值和 [sdkconfig.defaults](sdkconfig.defaults) 保持一致：

- `Xiaozhi Assistant -> Board Type -> Espressif ESP-VoCat`
- `Serial flasher config -> Flash size -> 16MB`
- `Partition Table -> Custom partition CSV file -> partitions/v2/16m.csv`
- `Xiaozhi Assistant -> Enable Device-Side AEC -> 开启`
- `Xiaozhi Assistant -> Select display style -> Emote animation style`
- `Xiaozhi Assistant -> Flash Assets -> Flash Emote Assets`
- `mmap file support format -> Max file name length -> 32`

按 `S` 保存，按 `Q` 退出。

## 编译烧录

```bash
idf.py build flash
```

将 ESP-VoCat 喵伴连接至电脑，注意打开电源后再烧录。
