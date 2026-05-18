# ESP32-S3 多板 AI 对话固件

（中文 | [English](README.md) | [日本語](README_ja.md)）

## 项目简介

这是一个运行在多种 ESP32-S3 开发板上的 AI 对话固件。

当前主链路是：通过 Sentino 接口创建 agora convo agent 会话，与固件端 Agora RTC 进行实时音频、视频和 datastream 交互。通用会话逻辑主要集中在 `main/application.*` 和 `main/agora/`，板级差异集中在 `main/boards/<board>/`。

## 主要功能

- 实时语音对话：麦克风采集、AEC、音频上下行
- 声纹录入上传：v2 可在对话开始前录制声纹样本并上传到 OSS
- 实时视频上行：带摄像头的板子可接入视频流
- 屏显链路：支持 `EmoteDisplay`、`LVGL/LcdDisplay`、`OledDisplay`
- datastream 屏显：支持 `display_emotion` 和 `display_weather`
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
- `SENTINO_USER_ID`：填一个随机字符串
- `SENTINO_DEVICE_ID`：填一个随机整数
- `SENTINO_API_BASE_URL`：保持当前默认值即可

再确认以下配置：

- `CONFIG_SENTINO_DEFAULT_AGENT_ID`：填自己捏的agent id或者平台上现有的agent id
- `CONFIG_SENTINO_AGENT_OPTIONS_JSON`：配置设备侧可选 agent 列表
见sdkconfig.defaults配置
CONFIG_SENTINO_DEFAULT_AGENT_ID="e15116ba-1004-4d31-93e4-a001ae2a8258"
CONFIG_SENTINO_AGENT_OPTIONS_JSON="[{\"id\":\"aa588e26-b980-42a2-9d66-da04c162bfcc\",\"label\":\"Agent A\"},{\"id\":\"abb54559-dafd-443a-91ea-c8fc62fad078\",\"label\":\"Agent B\"},{\"id\":\"6cda06c2-ecf3-4437-a355-bdc0eb5c1a82\",\"label\":\"Agent emotion\"},{\"id\":\"24bec7cf-56ca-4e73-89a4-78ecdc7cb927\",\"label\":\"Agent weather\"},{\"id\":\"bd39242c-f1d9-4efb-b82e-87edb8ff3ea4\",\"label\":\"Agent weather and emotion\"},{\"id\":\"e15116ba-1004-4d31-93e4-a001ae2a8258\",\"label\":\"esp32s3\"}]"

### 2. 配置声纹录入上传（可选，v2）

v2 增加了可选的声纹录入上传链路。开启 `VOICEPRINT_OSS_ENABLED` 后，如果设备 NVS 里还没有缓存过声纹样本 URL，固件会在开始对话前执行：

- 播放声纹录入提示音
- 以 16 kHz 采集最长 30 秒麦克风 PCM
- 根据 VAD / RMS 选出有效语音更充分的 15 秒片段
- 将 16-bit mono `.pcm` 原始音频通过签名 `PutObject` 请求上传到阿里云 OSS
- 把上传后的样本 URL 保存在 `voiceprint` NVS namespace，后续会话复用该 URL

相关配置都在 `main/agora_project_config.h`：

- `VOICEPRINT_OSS_ENABLED`：GitHub clean 版默认是 `0`，配置好 OSS 后再改成 `1`
- `VOICEPRINT_OSS_BUCKET`：OSS bucket 名称
- `VOICEPRINT_OSS_ENDPOINT`：OSS 区域 endpoint，例如 `oss-cn-shanghai.aliyuncs.com`
- `VOICEPRINT_OSS_OBJECT_PREFIX`：对象前缀，默认 `voiceprints`
- `VOICEPRINT_OSS_PUBLIC_BASE_URL`：可选的公开访问或 CDN 基础 URL；留空时使用 `https://<bucket>.<endpoint>`
- `VOICEPRINT_STS_ACCESS_KEY_ID`、`VOICEPRINT_STS_ACCESS_KEY_SECRET`、`VOICEPRINT_STS_SECURITY_TOKEN`：用于签名上传请求的 STS 临时凭证

当前固件里的上传实现是设备端直传 OSS 的 POC，不要把真实 AK/SK、STS token 或长期 AccessKey 提交到 GitHub。客户需要根据自身业务和安全要求决定：是让设备先从业务服务端获取短期、最小权限的 STS 临时凭证后直传 OSS，还是让设备把声纹音频传给业务服务端，由服务端再转存 OSS。固件侧可以在调用 OSS 上传前扩展“向服务端请求/刷新 STS 凭证”的逻辑。

声纹音频可能属于生物特征或敏感数据。生产环境需要在业务服务端层面处理用户授权、数据留存、访问控制、传输安全和存储策略。

### 3. 查看支持的板型

```bash
python scripts/select_board.py --list-boards // 目前代码仓库只适配了vocat和sensecap watcher
```

### 4. 选择板型并编译

示例：`esp-vocat`

```bash
python scripts/select_board.py esp-vocat
idf.py build
idf.py flash monitor
```

示例：`sensecap-watcher`

```bash
python scripts/select_board.py sensecap-watcher
idf.py build
idf.py flash monitor
```

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

可以直接参考这两个现成板：

- `sensecap-watcher`：`AUDIO_INPUT_REFERENCE=true`
- `esp-vocat`：`AUDIO_INPUT_REFERENCE=false`

#### 板级实现里把这个值传给 Codec

也就是在创建 AudioCodec 的时候，把 `AUDIO_INPUT_REFERENCE` 一起传进去，保持板级定义和运行时一致。

#### 最后做实际验证

至少验证这三件事：

- 扬声器外放时，本地说话能不能稳定被识别
- 远端回声会不会明显串回上行
- 静音、打断、连续对话时 AEC 状态会不会漂

### 8. 编译验证

```bash
python scripts/select_board.py my-custom-board
idf.py build
idf.py flash monitor
```

## 参考文档

- `docs/sentino-conversation.md`
- `docs/custom-board.md`
