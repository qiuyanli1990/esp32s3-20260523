#ifndef AGORA_PROJECT_CONFIG_H
#define AGORA_PROJECT_CONFIG_H

#include "sdkconfig.h"

// Sentino conversation API configuration.
// Fill these values with your own tenant credentials before building firmware.
#define SENTINO_API_BASE_URL "https://api.sentino.jp"
#define SENTINO_API_BEARER_TOKEN "YOUR_SENTINO_API_BEARER_TOKEN"

// Agent selection defaults. The provisioning page can override the active agent_id
// and persists the selection in NVS under the "wifi" namespace.
#define SENTINO_DEFAULT_AGENT_ID CONFIG_SENTINO_DEFAULT_AGENT_ID
#define SENTINO_AGENT_OPTIONS_JSON CONFIG_SENTINO_AGENT_OPTIONS_JSON

// Optional identity overrides. If left empty, firmware falls back to:
// user_id   -> "user_" + board UUID
// device_id -> "device_" + board UUID
#define SENTINO_USER_ID ""
#define SENTINO_DEVICE_ID ""

// Conversation request parameters.
#define SENTINO_CONVERSATION_SAMPLE_RATE 16000
#define SENTINO_LANGUAGES_JSON "[\"zh\",\"en\"]"
#define SENTINO_GREETING_MESSAGE ""

// Optional device metadata attached to the conversation request.
#define SENTINO_DEVICE_TIMEZONE "Asia/Shanghai"
#define SENTINO_DEVICE_LAT ""
#define SENTINO_DEVICE_LON ""

// Agora RTC audio path used by the firmware.
#define SENTINO_RTC_PCM_SAMPLE_RATE 16000
#define SENTINO_RTC_PCM_DURATION_MS 20

// HTTPS timeout for Sentino REST requests.
#define SENTINO_HTTP_TIMEOUT_MS 15000

// Voiceprint sample upload.
// The public template does not include real OSS credentials. Enable this only
// after configuring a bucket and short-lived STS credentials for your own tenant.
#define VOICEPRINT_OSS_ENABLED 0
#define VOICEPRINT_CAPTURE_DURATION_MS 30000
#define VOICEPRINT_OUTPUT_DURATION_MS 15000
#define VOICEPRINT_MIN_EFFECTIVE_AUDIO_MS 8000
#define VOICEPRINT_MAX_PCM_BYTES (2 * 1024 * 1024)
#define VOICEPRINT_VAD_RMS_THRESHOLD 350
#define VOICEPRINT_VAD_HANGOVER_MS 500
#define VOICEPRINT_SNTP_SYNC_TIMEOUT_MS 8000
#define VOICEPRINT_OSS_HTTP_TIMEOUT_MS 30000
#define VOICEPRINT_OSS_HTTP_RX_BUFFER_BYTES 2048
#define VOICEPRINT_OSS_HTTP_TX_BUFFER_BYTES 8192
#define VOICEPRINT_OSS_BUCKET ""
#define VOICEPRINT_OSS_ENDPOINT "oss-cn-shanghai.aliyuncs.com"
#define VOICEPRINT_OSS_PUBLIC_BASE_URL ""
#define VOICEPRINT_OSS_OBJECT_PREFIX "voiceprints"
#define VOICEPRINT_STS_ACCESS_KEY_ID ""
#define VOICEPRINT_STS_ACCESS_KEY_SECRET ""
#define VOICEPRINT_STS_SECURITY_TOKEN ""

#endif  // AGORA_PROJECT_CONFIG_H
