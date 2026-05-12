#include "agora/agora_agent_manager.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <memory>

#include <cJSON.h>
#include <esp_crt_bundle.h>
#include <esp_heap_caps.h>
#include <esp_http_client.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "agora_project_config.h"
#include "board.h"
#include "settings.h"
#include "system_info.h"
#include <wifi_manager.h>

extern "C" {
#include "agora_rtc_api.h"
}

namespace {

constexpr char kTag[] = "SentinoConvo";
constexpr int kHttpBufferSize = 4096;
constexpr uint32_t kSentinoStopTaskStackBytes = 12 * 1024;

struct HttpResponseBuffer {
    char* data = nullptr;
    int capacity = 0;
    int length = 0;
};

esp_err_t HttpEventHandler(esp_http_client_event_t* evt) {
    if (evt == nullptr || evt->user_data == nullptr) {
        return ESP_OK;
    }

    auto* response = static_cast<HttpResponseBuffer*>(evt->user_data);
    if (evt->event_id == HTTP_EVENT_ON_DATA && evt->data != nullptr && evt->data_len > 0 &&
        response->data != nullptr && response->capacity > 0 && response->length < response->capacity - 1) {
        int copy_len = evt->data_len;
        int remaining = response->capacity - response->length - 1;
        if (copy_len > remaining) {
            copy_len = remaining;
        }
        memcpy(response->data + response->length, evt->data, copy_len);
        response->length += copy_len;
        response->data[response->length] = '\0';
    }

    return ESP_OK;
}

std::string Trim(const std::string& value) {
    size_t start = 0;
    while (start < value.size() && std::isspace(static_cast<unsigned char>(value[start]))) {
        ++start;
    }
    size_t end = value.size();
    while (end > start && std::isspace(static_cast<unsigned char>(value[end - 1]))) {
        --end;
    }
    return value.substr(start, end - start);
}

std::string GetJsonString(const cJSON* object, const char* key) {
    if (object == nullptr || key == nullptr) {
        return "";
    }

    cJSON* item = cJSON_GetObjectItemCaseSensitive(const_cast<cJSON*>(object), key);
    if (cJSON_IsString(item) && item->valuestring != nullptr) {
        return item->valuestring;
    }
    if (cJSON_IsNumber(item)) {
        return std::to_string(item->valueint);
    }
    return "";
}

bool ParseUint32(const cJSON* item, uint32_t* out_value) {
    if (item == nullptr || out_value == nullptr) {
        return false;
    }
    if (cJSON_IsNumber(item)) {
        *out_value = static_cast<uint32_t>(item->valuedouble);
        return true;
    }
    if (!cJSON_IsString(item) || item->valuestring == nullptr) {
        return false;
    }

    char* end_ptr = nullptr;
    unsigned long value = strtoul(item->valuestring, &end_ptr, 10);
    if (end_ptr == item->valuestring || *end_ptr != '\0') {
        return false;
    }
    *out_value = static_cast<uint32_t>(value);
    return true;
}

std::string ResolveUserId() {
    if (strlen(SENTINO_USER_ID) > 0) {
        return SENTINO_USER_ID;
    }
    return std::string("user_") + Board::GetInstance().GetUuid();
}

std::string ResolveDeviceId() {
    if (strlen(SENTINO_DEVICE_ID) > 0) {
        return SENTINO_DEVICE_ID;
    }
    return std::string("device_") + Board::GetInstance().GetUuid();
}

std::string ResolveAgentId() {
    Settings settings("wifi", false);
    return Trim(settings.GetString("agent_id", SENTINO_DEFAULT_AGENT_ID));
}

void AddStringIfNotEmpty(cJSON* object, const char* key, const std::string& value, bool* added) {
    if (object == nullptr || key == nullptr || value.empty()) {
        return;
    }
    cJSON_AddStringToObject(object, key, value.c_str());
    if (added != nullptr) {
        *added = true;
    }
}

void AddNumberIfValidString(cJSON* object, const char* key, const char* value, bool* added) {
    if (object == nullptr || key == nullptr || value == nullptr || value[0] == '\0') {
        return;
    }

    char* end_ptr = nullptr;
    double parsed = strtod(value, &end_ptr);
    if (end_ptr == value || *end_ptr != '\0') {
        return;
    }

    cJSON_AddNumberToObject(object, key, parsed);
    if (added != nullptr) {
        *added = true;
    }
}

std::string StripJsonToken(const std::string& raw_token) {
    std::string token = Trim(raw_token);
    while (!token.empty() &&
           (token.front() == '"' || token.front() == '\'' || token.front() == '[')) {
        token.erase(token.begin());
    }
    while (!token.empty() &&
           (token.back() == '"' || token.back() == '\'' || token.back() == ']')) {
        token.pop_back();
    }
    return Trim(token);
}

void AddLanguagesToObject(cJSON* object) {
    if (object == nullptr || strlen(SENTINO_LANGUAGES_JSON) == 0) {
        return;
    }

    cJSON* parsed = cJSON_Parse(SENTINO_LANGUAGES_JSON);
    if (cJSON_IsArray(parsed)) {
        cJSON_AddItemToObject(object, "languages", parsed);
        return;
    }
    if (parsed != nullptr) {
        cJSON_Delete(parsed);
    }

    cJSON* languages = cJSON_CreateArray();
    std::string raw = SENTINO_LANGUAGES_JSON;
    size_t start = 0;
    while (start <= raw.size()) {
        size_t end = raw.find(',', start);
        std::string token = StripJsonToken(raw.substr(start, end == std::string::npos ? std::string::npos : end - start));
        if (!token.empty()) {
            cJSON_AddItemToArray(languages, cJSON_CreateString(token.c_str()));
        }
        if (end == std::string::npos) {
            break;
        }
        start = end + 1;
    }

    if (cJSON_GetArraySize(languages) > 0) {
        cJSON_AddItemToObject(object, "languages", languages);
    } else {
        cJSON_Delete(languages);
    }
}

void AddDeviceInfo(cJSON* object) {
    if (object == nullptr) {
        return;
    }

    bool has_device_info = false;
    cJSON* device_info = cJSON_CreateObject();
    AddStringIfNotEmpty(device_info, "timezone", SENTINO_DEVICE_TIMEZONE, &has_device_info);
    AddNumberIfValidString(device_info, "lat", SENTINO_DEVICE_LAT, &has_device_info);
    AddNumberIfValidString(device_info, "lon", SENTINO_DEVICE_LON, &has_device_info);

    auto& wifi = WifiManager::GetInstance();
    AddStringIfNotEmpty(device_info, "ip", wifi.GetIpAddress(), &has_device_info);
    AddStringIfNotEmpty(device_info, "mac", SystemInfo::GetMacAddress(), &has_device_info);

    int wifi_rssi = wifi.GetRssi();
    if (wifi_rssi != 0) {
        cJSON_AddNumberToObject(device_info, "wifi_rssi", wifi_rssi);
        has_device_info = true;
    }

    int battery_percent = 0;
    bool charging = false;
    bool discharging = false;
    if (Board::GetInstance().GetBatteryLevel(battery_percent, charging, discharging)) {
        battery_percent = std::clamp(battery_percent, 0, 100);
        cJSON_AddNumberToObject(device_info, "device_battery_percent", battery_percent);
        cJSON_AddBoolToObject(device_info, "charging", charging);
        has_device_info = true;
    }

    if (has_device_info) {
        cJSON_AddItemToObject(object, "device_info", device_info);
    } else {
        cJSON_Delete(device_info);
    }
}

bool ParseConversationResponse(const char* response_body, AgoraAgentConfig* config, std::string* error) {
    if (response_body == nullptr || config == nullptr) {
        return false;
    }

    cJSON* root = cJSON_Parse(response_body);
    if (root == nullptr) {
        if (error != nullptr) {
            *error = "Failed to parse Sentino conversation response";
        }
        return false;
    }

    cJSON* payload = cJSON_GetObjectItemCaseSensitive(root, "data");
    if (!cJSON_IsObject(payload)) {
        payload = root;
    }

    std::string conversation_id = GetJsonString(payload, "conversation_id");
    std::string channel_name = GetJsonString(payload, "channel");
    if (channel_name.empty()) {
        channel_name = GetJsonString(payload, "channelName");
    }
    std::string app_id = GetJsonString(payload, "app_id");
    if (app_id.empty()) {
        app_id = GetJsonString(payload, "appId");
    }
    std::string rtc_token = GetJsonString(payload, "rtc_token");
    if (rtc_token.empty()) {
        rtc_token = GetJsonString(payload, "rtcToken");
    }

    cJSON* rtc_uid_item = cJSON_GetObjectItemCaseSensitive(payload, "rtc_uid");
    if (rtc_uid_item == nullptr) {
        rtc_uid_item = cJSON_GetObjectItemCaseSensitive(payload, "uid");
    }
    uint32_t rtc_uid = 0;

    bool ok = !conversation_id.empty() &&
        !channel_name.empty() &&
        !app_id.empty() &&
        !rtc_token.empty() &&
        ParseUint32(rtc_uid_item, &rtc_uid);

    if (!ok && error != nullptr) {
        *error = "Sentino conversation response is missing required RTC fields";
    }

    if (ok) {
        config->conversation_id = conversation_id;
        config->channel_name = channel_name;
        config->app_id = app_id;
        config->rtc_token = rtc_token;
        config->agent_rtc_uid = 0;
        config->remote_rtc_uid = rtc_uid;
    }

    cJSON_Delete(root);
    return ok;
}

}  // namespace

bool AgoraAgentManager::Initialize() {
    ready_ = LoadStaticConfig();
    return ready_;
}

bool AgoraAgentManager::LoadStaticConfig() {
    config_ = AgoraAgentConfig{};
    config_.agent_name = ResolveAgentId();
    // Match the known-good Agora reference implementation for ESP32-S3 boards.
    config_.rtc_audio_codec_type = AUDIO_CODEC_TYPE_G722;
    config_.pcm_sample_rate = SENTINO_RTC_PCM_SAMPLE_RATE;
    config_.pcm_frame_bytes =
        (SENTINO_RTC_PCM_SAMPLE_RATE * SENTINO_RTC_PCM_DURATION_MS / 1000) * static_cast<int>(sizeof(int16_t));

    if (strlen(SENTINO_API_BASE_URL) == 0 || strlen(SENTINO_API_BEARER_TOKEN) == 0 || config_.agent_name.empty()) {
        SetError("Sentino credentials are incomplete; set bearer token and a default or provisioned agent_id");
        return false;
    }
    if (SENTINO_CONVERSATION_SAMPLE_RATE <= 0 || SENTINO_RTC_PCM_SAMPLE_RATE <= 0 || SENTINO_RTC_PCM_DURATION_MS <= 0) {
        SetError("Sentino audio configuration is invalid");
        return false;
    }

    config_.user_id = ResolveUserId();
    config_.device_id = ResolveDeviceId();
    ClearRunningState();
    last_error_.clear();
    return true;
}

bool AgoraAgentManager::BuildStartJson(std::string* body) {
    if (body == nullptr) {
        SetError("Invalid Sentino conversation request body");
        return false;
    }

    cJSON* root = cJSON_CreateObject();
    if (root == nullptr) {
        SetError("Failed to allocate Sentino conversation request JSON");
        return false;
    }

    config_.user_id = ResolveUserId();
    config_.device_id = ResolveDeviceId();
    config_.agent_name = ResolveAgentId();
    if (config_.agent_name.empty()) {
        cJSON_Delete(root);
        SetError("Sentino agent_id is empty");
        return false;
    }

    cJSON_AddStringToObject(root, "agent_id", config_.agent_name.c_str());
    cJSON_AddStringToObject(root, "user_id", config_.user_id.c_str());
    cJSON_AddStringToObject(root, "device_id", config_.device_id.c_str());
    cJSON_AddNumberToObject(root, "sample_rate", SENTINO_CONVERSATION_SAMPLE_RATE);

    if (strlen(SENTINO_GREETING_MESSAGE) > 0) {
        cJSON_AddStringToObject(root, "greeting_message", SENTINO_GREETING_MESSAGE);
    }

    AddLanguagesToObject(root);
    AddDeviceInfo(root);

    char* json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (json == nullptr) {
        SetError("Failed to serialize Sentino conversation request JSON");
        return false;
    }

    *body = json;
    cJSON_free(json);
    return true;
}

std::string AgoraAgentManager::BuildAuthHeader() const {
    return "Bearer " + std::string(SENTINO_API_BEARER_TOKEN);
}

void AgoraAgentManager::SetError(const std::string& error) {
    last_error_ = error;
    ESP_LOGE(kTag, "%s", error.c_str());
}

bool AgoraAgentManager::WaitForPendingStop(int timeout_ms) {
    if (!stop_in_progress_) {
        return true;
    }

    int waited_ms = 0;
    while (stop_in_progress_ && waited_ms < timeout_ms) {
        vTaskDelay(pdMS_TO_TICKS(100));
        waited_ms += 100;
    }

    if (stop_in_progress_) {
        SetError("Timed out waiting for previous Sentino conversation stop to finish");
        return false;
    }
    return true;
}

bool AgoraAgentManager::StartRequest() {
    std::string url = std::string(SENTINO_API_BASE_URL) + "/api/v1/conversations";

    std::string body;
    if (!BuildStartJson(&body)) {
        return false;
    }

    auto response_buffer = std::unique_ptr<char, decltype(&heap_caps_free)>(
        static_cast<char*>(heap_caps_calloc(kHttpBufferSize, sizeof(char), MALLOC_CAP_8BIT)),
        &heap_caps_free);
    if (!response_buffer) {
        SetError("Failed to allocate HTTP response buffer for Sentino conversation start");
        return false;
    }

    HttpResponseBuffer response = {
        .data = response_buffer.get(),
        .capacity = kHttpBufferSize,
        .length = 0,
    };

    esp_http_client_config_t cfg = {};
    cfg.url = url.c_str();
    cfg.event_handler = HttpEventHandler;
    cfg.user_data = &response;
    cfg.timeout_ms = SENTINO_HTTP_TIMEOUT_MS;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == nullptr) {
        SetError("Failed to initialize HTTP client for Sentino conversation start");
        return false;
    }

    esp_http_client_set_method(client, HTTP_METHOD_POST);
    std::string auth = BuildAuthHeader();
    esp_http_client_set_header(client, "Authorization", auth.c_str());
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "Accept", "application/json");
    esp_http_client_set_post_field(client, body.c_str(), static_cast<int>(body.size()));

    ESP_LOGI(kTag, "Create Sentino conversation: user_id=%s device_id=%s body_bytes=%u",
        config_.user_id.c_str(), config_.device_id.c_str(), static_cast<unsigned>(body.size()));

    esp_err_t err = esp_http_client_perform(client);
    int status_code = esp_http_client_get_status_code(client);
    ESP_LOGI(kTag, "Sentino conversation create response: err=%s http=%d bytes=%d",
        esp_err_to_name(err), status_code, response.length);

    bool success = false;
    if (err == ESP_OK && status_code >= 200 && status_code < 300) {
        std::string parse_error;
        if (ParseConversationResponse(response_buffer.get(), &config_, &parse_error)) {
            conversation_id_ = config_.conversation_id;
            running_ = true;
            success = true;
        } else {
            SetError(parse_error + ": " + std::string(response_buffer.get()));
        }
    } else {
        std::string error = "Failed to create Sentino conversation";
        error += " err=";
        error += esp_err_to_name(err);
        error += " http=";
        error += std::to_string(status_code);
        if (response.length > 0) {
            error += " body=";
            error += response_buffer.get();
        }
        SetError(error);
    }

    esp_http_client_cleanup(client);
    if (success) {
        ESP_LOGI(kTag, "Sentino conversation started: conversation_id=%s channel=%s uid=%lu",
            config_.conversation_id.c_str(), config_.channel_name.c_str(),
            static_cast<unsigned long>(config_.remote_rtc_uid));
    }
    return success;
}

bool AgoraAgentManager::StopConversationById(const std::string& conversation_id) {
    if (conversation_id.empty()) {
        return true;
    }

    std::string url = std::string(SENTINO_API_BASE_URL) + "/api/v1/conversations/" +
        conversation_id + "/stop";

    auto response_buffer = std::unique_ptr<char, decltype(&heap_caps_free)>(
        static_cast<char*>(heap_caps_calloc(kHttpBufferSize, sizeof(char), MALLOC_CAP_8BIT)),
        &heap_caps_free);
    if (!response_buffer) {
        SetError("Failed to allocate HTTP response buffer for Sentino conversation stop");
        return false;
    }

    HttpResponseBuffer response = {
        .data = response_buffer.get(),
        .capacity = kHttpBufferSize,
        .length = 0,
    };

    esp_http_client_config_t cfg = {};
    cfg.url = url.c_str();
    cfg.event_handler = HttpEventHandler;
    cfg.user_data = &response;
    cfg.timeout_ms = SENTINO_HTTP_TIMEOUT_MS;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == nullptr) {
        SetError("Failed to initialize HTTP client for Sentino conversation stop");
        return false;
    }

    esp_http_client_set_method(client, HTTP_METHOD_POST);
    std::string auth = BuildAuthHeader();
    esp_http_client_set_header(client, "Authorization", auth.c_str());
    esp_http_client_set_header(client, "Accept", "application/json");

    ESP_LOGI(kTag, "Stop Sentino conversation: conversation_id=%s", conversation_id.c_str());
    esp_err_t err = esp_http_client_perform(client);
    int status_code = esp_http_client_get_status_code(client);
    ESP_LOGI(kTag, "Sentino conversation stop response: conversation_id=%s err=%s http=%d body=%s",
        conversation_id.c_str(), esp_err_to_name(err), status_code,
        response.length > 0 ? response_buffer.get() : "");

    bool success = err == ESP_OK && (status_code == 200 || status_code == 204 || status_code == 404);
    esp_http_client_cleanup(client);
    if (success) {
        return true;
    }

    std::string error = "Failed to stop Sentino conversation";
    error += " conversation_id=";
    error += conversation_id;
    error += " err=";
    error += esp_err_to_name(err);
    error += " http=";
    error += std::to_string(status_code);
    if (response.length > 0) {
        error += " body=";
        error += response_buffer.get();
    }
    SetError(error);
    return false;
}

bool AgoraAgentManager::Start() {
    if (!ready_ && !Initialize()) {
        return false;
    }
    if (running_) {
        return true;
    }
    if (!WaitForPendingStop(5000)) {
        return false;
    }

    ESP_LOGI(kTag, "Sentino conversation start request begin");
    if (!StartRequest()) {
        if (last_error_.empty()) {
            SetError("Failed to start Sentino conversation");
        }
        return false;
    }

    last_error_.clear();
    return true;
}

void AgoraAgentManager::Stop() {
    if (stop_in_progress_) {
        ESP_LOGI(kTag, "Stop requested while async conversation stop is already in progress");
        return;
    }
    if (!running_) {
        ESP_LOGI(kTag, "Stop ignored because no Sentino conversation is marked running");
        return;
    }

    std::string conversation_id = conversation_id_;
    ESP_LOGI(kTag, "Stop requested synchronously for conversation_id=%s", conversation_id.c_str());
    stop_in_progress_ = true;
    last_stop_success_ = StopConversationById(conversation_id);
    ClearRunningState();
    stop_in_progress_ = false;
}

void AgoraAgentManager::StopAsync() {
    if (stop_in_progress_) {
        ESP_LOGI(kTag, "StopAsync ignored because a previous async conversation stop is still in progress");
        return;
    }
    if (!running_) {
        ESP_LOGI(kTag, "StopAsync ignored because no Sentino conversation is marked running");
        return;
    }

    std::string conversation_id = conversation_id_;
    ESP_LOGI(kTag, "StopAsync requested for conversation_id=%s", conversation_id.c_str());
    stop_in_progress_ = true;
    last_stop_success_ = false;
    ClearRunningState();

    struct StopTaskArgs {
        AgoraAgentManager* manager;
        std::string conversation_id;
    };

    BaseType_t create_ok = xTaskCreate([](void* arg) {
        std::unique_ptr<StopTaskArgs> task_args(static_cast<StopTaskArgs*>(arg));
        if (task_args && task_args->manager != nullptr) {
            if (!task_args->conversation_id.empty()) {
                task_args->manager->last_stop_success_ =
                    task_args->manager->StopConversationById(task_args->conversation_id);
            } else {
                task_args->manager->last_stop_success_ = true;
            }
            task_args->manager->stop_in_progress_ = false;
        }
        vTaskDelete(nullptr);
    }, "sentino_conv_stop", kSentinoStopTaskStackBytes, new StopTaskArgs{this, conversation_id}, 2, nullptr);

    if (create_ok != pdPASS) {
        stop_in_progress_ = false;
        SetError("Failed to create Sentino conversation stop task");
    }
}

void AgoraAgentManager::ClearRunningState() {
    conversation_id_.clear();
    running_ = false;
    config_.conversation_id.clear();
    config_.channel_name.clear();
    config_.app_id.clear();
    config_.rtc_token.clear();
    config_.agent_rtc_uid = 0;
    config_.remote_rtc_uid = 0;
}
