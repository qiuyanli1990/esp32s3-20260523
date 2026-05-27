# ESP32 多板 AI 对话固件

（中文 | [English](README.md)）

## 项目简介

这是一个运行在多种 ESP32 开发板上的 AI 对话固件，当前重点适配 ESP32-S3 语音/屏显类板子。

当前主链路是：通过 Sentino 接口创建 agora convo agent 会话，与固件端 Agora RTC 进行实时音频、视频和 datastream 交互。通用会话逻辑主要集中在 `main/application.*` 和 `main/agora/`，板级差异集中在 `main/boards/<board>/`。

## 主要功能

- 实时语音对话：麦克风采集、AEC、音频上下行
- 可选声纹录入上传：可在对话开始前录制声纹样本并上传到 OSS 生成 `sample_url`
- 实时视频上行：带摄像头的板子可接入视频流
- 屏显链路：支持 `EmoteDisplay`、`LVGL/LcdDisplay`、`OledDisplay`
- datastream 屏显：支持 `display_emotion`、`display_weather` 以及兼容的情绪显示别名
- 素材体系：字体、emoji、emote、weather 统一走 `assets` 分区
- 板级扩展：按键、背光、电量、摄像头、LED、网络

## 对话流程图

```text
用户
  │ 语音 / 按键 / 触摸
  ▼
开发板固件
  │
  ├─ 音频采集 / 播放
  ├─ 摄像头
  └─ 屏显
  │
  ▼
Application
  │ 设备状态机 / 会话控制 / datastream 分发
  │
  ├─ 调 Sentino 接口创建会话
  │
  └─ 使用返回的动态参数加入 Agora RTC
       │
       ├─ 实时音频 / 视频上行
       ├─ 语音下行回放
       └─ datastream 回到本地屏显
            │
            ▼
       云端 Agent
       ASR / LLM / TTS
```

## 代码结构

| 路径 | 作用 |
| --- | --- |
| `main/application.*` | 设备主状态机、会话起停、datastream 分发 |
| `main/agora/` | Sentino 会话创建、Agora RTC、音视频桥接 |
| `main/voiceprint/` | 声纹样本采集、基于 VAD 的有效片段选择和 OSS 上传 |
| `main/audio/` | 音频采集、播放、AEC、VAD、编解码处理 |
| `main/display/` | Emote、LVGL、OLED 等显示后端 |
| `main/assets/` | `assets` 分区加载与运行时素材访问 |
| `main/boards/<board>/` | 板级 GPIO、Codec、Display、Camera、Network 初始化 |
| `docs/sentino-conversation.md` | 当前会话链路说明 |
| `docs/custom-board.md` | 自定义开发板适配说明 |
| `docs/aec-board-matrix.md` | 各 Codec 路径的设备端 AEC 模式矩阵 |
| `docs/korvo2_aec_integration_code_zh.md` | Korvo-2 AEC 集成说明 |

## 环境准备

### 1. 安装 ESP-IDF

硬性要求：`ESP-IDF v5.5.4`

```bash
mkdir -p ~/esp
cd ~/esp
git clone -b v5.5.4 --recursive https://github.com/espressif/esp-idf.git
cd esp-idf
./install.sh esp32s3
source ./export.sh
```

每次打开新终端后，重新执行：

```bash
source ~/esp/esp-idf/export.sh
```

如果你使用 VS Code 或 Cursor，也可以直接安装 ESP-IDF 插件，并把 SDK 版本设为 `5.5.4`。

### 2. 获取源码

```bash
git clone <your-repo-or-fork-url>
cd <repo-dir>
```

### 3. 依赖说明

- 第一次 `idf.py build` 会根据 `main/idf_component.yml` 和锁文件拉取受管组件
- `Agora IoT SDK` 已经在 `components/agora_iot_sdk/` 里，不需要额外拷贝

## 运行项目

### 1. 配置后端参数

先到 `studio.sentino.jp/home` 注册并登录，创建自己的 agent，拿到 API token 和 agent id， 创建agent Id的时候开启天气查询和device control tools可以让大模型下发天气信息和情绪信息。

然后修改 `main/agora_project_config.h`：

- `SENTINO_API_BEARER_TOKEN`：必填，填自己的 API token
- `SENTINO_USER_ID`：填一个业务侧用户标识；留空时固件会使用 `user_<board-uuid>`
- `SENTINO_DEVICE_ID`：填一个业务侧设备标识；留空时固件会使用 `device_<board-uuid>`
- `SENTINO_API_BASE_URL`：保持当前默认值即可

再确认以下配置：

- `SENTINO_DEFAULT_AGENT_ID`：默认 agent id
- `SENTINO_AGENT_OPTIONS`：设备侧可选 agent 列表，至少配置两个选项时，启动对话前会进入运行时选择窗口
- `SENTINO_ENABLE_VOICEPRINT`：是否在对话前执行声纹录入上传，默认 `0`
- `SENTINO_LANGUAGES_JSON`、`SENTINO_GREETING_MESSAGE`：可选，对应 Sentino 会话请求里的语言列表和开场白

示例：

```cpp
#define SENTINO_DEFAULT_AGENT_ID "your-default-agent-id"

static constexpr SentinoAgentOptionConfig SENTINO_AGENT_OPTIONS[] = {
    {"your-agent-id-a", "Agent A"},
    {"your-agent-id-b", "Agent B"},
};
```

`SENTINO_AGENT_OPTIONS` 为空或只有一个选项时不会弹出选择窗口。运行时选中的 agent id 会保存在 NVS 的 `wifi/agent_id`，后续重启会沿用；如果要清掉旧选择，可以重新选择一次，或执行 `idf.py erase-flash flash monitor`。

### 2. 配置声纹录入上传

增加了可选的声纹录入上传链路。开启 `SENTINO_ENABLE_VOICEPRINT` 后，如果设备 NVS 里还没有缓存过声纹样本 URL，固件会在开始对话前执行：

- 播放声纹录入提示音
- 以 16 kHz 采集最长 30 秒麦克风 PCM
- 根据 VAD / RMS 选出有效语音更充分的 15 秒片段
- 将 16-bit mono `.pcm` 原始音频通过签名 `PutObject` 请求上传到阿里云 OSS
- 把上传后的样本 URL 保存在 `voiceprint` NVS namespace，后续会话复用该 URL

相关配置都在 `main/agora_project_config.h`：

- `SENTINO_ENABLE_VOICEPRINT`：默认是 `0`，配置好 OSS 后再改成 `1`
- `VOICEPRINT_OSS_BUCKET`：OSS bucket 名称
- `VOICEPRINT_OSS_ENDPOINT`：OSS 区域 endpoint，例如 `oss-cn-shanghai.aliyuncs.com`
- `VOICEPRINT_OSS_OBJECT_PREFIX`：对象前缀，默认 `voiceprints`
- `VOICEPRINT_OSS_PUBLIC_BASE_URL`：可选的公开访问或 CDN 基础 URL；留空时使用 `https://<bucket>.<endpoint>`
- `VOICEPRINT_STS_ACCESS_KEY_ID`、`VOICEPRINT_STS_ACCESS_KEY_SECRET`、`VOICEPRINT_STS_SECURITY_TOKEN`：用于签名上传请求的 STS 临时凭证

当前固件里的上传实现是设备端直传 OSS 的 POC，不要把真实 AK/SK、STS token 或长期 AccessKey 提交到 GitHub。客户需要根据自身业务和安全要求决定：是让设备先从业务服务端获取短期、最小权限的 STS 临时凭证后直传 OSS，还是让设备把声纹音频传给业务服务端，由服务端再转存 OSS。固件侧可以在调用 OSS 上传前扩展“向服务端请求/刷新 STS 凭证”的逻辑。

声纹音频可能属于生物特征或敏感数据。生产环境需要在业务服务端层面处理用户授权、数据留存、访问控制、传输安全和存储策略。

### 3. 查看支持的板型

```bash
python3 scripts/select_board.py --list-boards
```

当前常用三块板：

| 板子 | `select_board` 参数 | 说明 |
| --- | --- | --- |
| SenseCAP Watcher | `sensecap-watcher` | 圆屏、旋钮 |
| ESP-VOCAT | `esp-vocat` | 圆屏、触摸 |
| ESP32-S3-Korvo2 V3 | `esp32s3-korvo2-v3` | Korvo 带屏幕/摄像头版本 |

如果手上是老的 ESP32-S3-Korvo-2 音频板，板型参数是 `esp32s3-korvo-2`。

### 4. 选择板型并编译

先任选一个板型切换命令：

```bash
# Watcher
python3 scripts/select_board.py sensecap-watcher

# VOCAT
python3 scripts/select_board.py esp-vocat

# Korvo2 V3
python3 scripts/select_board.py esp32s3-korvo2-v3
```

然后编译并刷机：

```bash
idf.py build
idf.py flash monitor
```

切板后根目录 `sdkconfig` 和 `build/` 会切到当前板子。`idf.py flash` 会把当前板子生成的 `build/generated_assets.bin` 一起刷进 `assets` 分区，所以不同板子的显示资源会随切板和重新 build 自动匹配。

### 5. 运行时切换 agent id

先在 `main/agora_project_config.h` 里配置两个或更多 `SENTINO_AGENT_OPTIONS`。设备空闲且联网后，按会话键启动对话；如果开启了声纹，固件会先完成声纹流程，然后显示当前 `Agent: <label>` 并进入 5 秒选择窗口。

| 板子 | 启动对话 | 选择窗口内切下一个 agent | 确认 |
| --- | --- | --- | --- |
| Watcher | 短按滚轮按钮 | 旋转滚轮，或再次短按滚轮按钮 | 停止操作约 5 秒 |
| VOCAT | 点按触摸屏，或短按 BOOT 键 | 再次点按触摸屏，或短按 BOOT 键 | 停止操作约 5 秒 |
| Korvo2 V3 | 按 REC 键，或短按 BOOT 键 | 再次按 REC 键，或短按 BOOT 键 | 停止操作约 5 秒 |

确认后会保存选中的 agent id，并继续创建 Sentino 会话。Watcher 长按滚轮是电源/恢复相关操作；Korvo 的 SET 键是进入配网，不用于切换 agent。

## 新板适配步骤

### 1. 选择网络基类

- `WifiBoard`：纯 Wi-Fi 板
- `Ml307Board`：挂 ML307 / ML307R 4G 模组的板子
- `DualNetworkBoard`：同时支持 Wi-Fi 和 ML307 的板子

### 2. 创建板级目录

在 `main/boards/` 下创建新目录，通常至少包含这些文件：

- `config.h`
- `<board>.cc`
- `config.json`
- `sdkconfig.defaults`
- `README.md`

### 3. 写 `config.h`

这里放板级硬件事实：

- I2S / I2C / SPI / UART 引脚
- 音频采样率
- 屏幕尺寸、旋转、镜像
- 按键、背光、LED、电池、摄像头引脚
- Codec 地址和功放控制引脚

建议把硬件定义都收敛在 `config.h`，不要把引脚直接写死在 `.cc` 里。

### 4. 实现板级类

在 `<board>.cc` 里完成：

- 选择继承基类：`WifiBoard` / `Ml307Board` / `DualNetworkBoard`
- 初始化 AudioCodec
- 初始化 Display
- 初始化 Backlight / Button / Camera / Power / Network
- 最后使用 `DECLARE_BOARD(...)`

### 5. 注册板型

需要同时修改两处：

1. `main/Kconfig.projbuild`
   新增 `BOARD_TYPE_<YOUR_BOARD>`
2. `main/CMakeLists.txt`
   新增对应板型分支，指定 `BOARD_TYPE`、字体和默认素材配置

### 6. 选择屏显路线

#### Emote 路线

适合常驻表情屏：

- 板级里创建 `EmoteDisplay`
- 开启 `CONFIG_USE_EMOTE_MESSAGE_STYLE=y`
- 开启 `CONFIG_FLASH_EXPRESSION_ASSETS=y`

#### LVGL / OLED 路线

适合信息卡片、菜单和图片预览：

- 板级里创建 `SpiLcdDisplay` / `RgbLcdDisplay` / `OledDisplay`
- 开启 `CONFIG_FLASH_DEFAULT_ASSETS=y`

### 7. Custom Board 的 AEC 怎么配

如果你的板子是全双工语音板，建议明确把 AEC 路径定下来。

#### 先决定是否开启设备端 AEC

在 `main/boards/<board>/sdkconfig.defaults` 里配置：

```text
CONFIG_USE_DEVICE_AEC=y
```

如果你的板子没有稳定的麦克风采集和扬声器回放链路，或者本身不是对话型设备，就不要开。

#### 再决定 `AUDIO_INPUT_REFERENCE`

在 `config.h` 里定义：

```c
#define AUDIO_INPUT_REFERENCE true
```

或：

```c
#define AUDIO_INPUT_REFERENCE false
```

判断原则：

- `true`：硬件采集链路里有单独的播放参考通道，可以把参考信号直接送进运行时 AEC
- `false`：没有独立参考通道，走公共的软件参考路径

可以直接参考这些现成板：

- `sensecap-watcher`：`AUDIO_INPUT_REFERENCE=true`
- `esp-vocat`：`AUDIO_INPUT_REFERENCE=true`
- `esp32s3-korvo2-v3`：`AUDIO_INPUT_REFERENCE=true`
- `esp32s3-korvo-2`：`AUDIO_INPUT_REFERENCE=true`

#### 板级实现里把这个值传给 Codec

也就是在创建 AudioCodec 的时候，把 `AUDIO_INPUT_REFERENCE` 一起传进去，保持板级定义和运行时一致。

#### 最后做实际验证

至少验证这三件事：

- 扬声器外放时，本地说话能不能稳定被识别
- 远端回声会不会明显串回上行
- 静音、打断、连续对话时 AEC 状态会不会漂

### 8. 编译验证

```bash
python3 scripts/select_board.py my-custom-board
idf.py build
idf.py flash monitor
```

## 参考文档

- `docs/sentino-conversation.md`
- `docs/custom-board.md`
- `docs/aec-board-matrix.md`
- `docs/korvo2_aec_integration_code_zh.md`
