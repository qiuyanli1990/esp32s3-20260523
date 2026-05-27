#include "agora/agora_rtc_session.h"

#include <algorithm>
#include <cJSON.h>
#include <cctype>
#include <cstring>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <mbedtls/base64.h>
#include <utility>

#include "sdkconfig.h"

namespace {
constexpr char kTag[] = "AgoraRTC";

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

int HexValue(char ch) {
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }
    ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    if (ch >= 'a' && ch <= 'f') {
        return 10 + (ch - 'a');
    }
    return -1;
}

std::string PercentDecode(const std::string& value) {
    std::string decoded;
    decoded.reserve(value.size());
    for (size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '%' && i + 2 < value.size()) {
            int hi = HexValue(value[i + 1]);
            int lo = HexValue(value[i + 2]);
            if (hi >= 0 && lo >= 0) {
                decoded.push_back(static_cast<char>((hi << 4) | lo));
                i += 2;
                continue;
            }
        }
        decoded.push_back(value[i] == '+' ? ' ' : value[i]);
    }
    return decoded;
}

std::string DecodePackedDatastreamText(const std::string& value) {
    std::string raw = Trim(value);
    if (raw.empty()) {
        return "";
    }

    size_t first = raw.find('|');
    if (first == std::string::npos) {
        return "";
    }
    size_t second = raw.find('|', first + 1);
    if (second == std::string::npos) {
        return "";
    }
    size_t third = raw.find('|', second + 1);
    if (third == std::string::npos || third + 1 >= raw.size()) {
        return "";
    }

    std::string b64 = raw.substr(third + 1);
    std::vector<unsigned char> decoded((b64.size() * 3) / 4 + 4);
    size_t decoded_len = 0;
    if (mbedtls_base64_decode(decoded.data(), decoded.size(), &decoded_len,
            reinterpret_cast<const unsigned char*>(b64.data()), b64.size()) != 0) {
        return "";
    }
    return std::string(reinterpret_cast<const char*>(decoded.data()), decoded_len);
}

bool LooksLikeJson(const std::string& value) {
    std::string trimmed = Trim(value);
    return !trimmed.empty() && (trimmed.front() == '{' || trimmed.front() == '[');
}

bool LooksLikeJsonFragment(const std::string& value) {
    std::string trimmed = Trim(value);
    if (trimmed.empty()) {
        return false;
    }
    if (LooksLikeJson(trimmed)) {
        return true;
    }
    return trimmed.find("\"object\"") != std::string::npos ||
        trimmed.find("\"word\"") != std::string::npos ||
        trimmed.find("\"start_ms\"") != std::string::npos ||
        trimmed.find("\"duration_ms\"") != std::string::npos ||
        trimmed.find("\"stable\"") != std::string::npos ||
        (trimmed.find('{') != std::string::npos && trimmed.find(':') != std::string::npos);
}

std::string ShortenForLog(const std::string& value, size_t max_len = 160) {
    if (value.size() <= max_len) {
        return value;
    }
    return value.substr(0, max_len) + "...";
}

std::string SanitizeForLog(const char* data, size_t length) {
    if (data == nullptr || length == 0) {
        return "";
    }

    std::string out;
    out.reserve(std::min<size_t>(length, 160));
    for (size_t i = 0; i < length && out.size() < 160; ++i) {
        unsigned char ch = static_cast<unsigned char>(data[i]);
        if (ch == '\r' || ch == '\n' || ch == '\t') {
            out.push_back(' ');
        } else if (std::isprint(ch)) {
            out.push_back(static_cast<char>(ch));
        } else {
            out.push_back('?');
        }
    }
    return ShortenForLog(Trim(out), 160);
}

std::string SummarizeStreamPreview(const char* data, size_t length) {
    if (data == nullptr || length == 0) {
        return "";
    }

    std::string raw(data, length);
    std::string decoded = DecodePackedDatastreamText(raw);
    if (!decoded.empty()) {
        return ShortenForLog(SanitizeForLog(decoded.data(), decoded.size()), 160);
    }
    return SanitizeForLog(data, length);
}

bool ExtractActionJsonString(cJSON* node, std::string* out, int depth);

bool SerializeJsonNode(cJSON* node, std::string* out) {
    if (node == nullptr || out == nullptr) {
        return false;
    }
    char* payload = cJSON_PrintUnformatted(node);
    *out = payload != nullptr ? payload : "";
    if (payload != nullptr) {
        cJSON_free(payload);
    }
    return !out->empty();
}

bool ContainsMessageUserMarker(const std::string& value) {
    return value.find("\"object\"") != std::string::npos &&
        value.find("message.user") != std::string::npos;
}

std::string ExtractObjectNameForLog(const std::string& value) {
    static constexpr const char* kNeedle = "\"object\"";
    size_t pos = value.find(kNeedle);
    if (pos == std::string::npos) {
        return "";
    }
    pos = value.find(':', pos + strlen(kNeedle));
    if (pos == std::string::npos) {
        return "";
    }
    pos = value.find('"', pos);
    if (pos == std::string::npos) {
        return "";
    }
    size_t end = value.find('"', pos + 1);
    if (end == std::string::npos || end <= pos + 1) {
        return "";
    }
    return value.substr(pos + 1, end - pos - 1);
}

std::string ExtractMessageUserActionPayloadDirect(const std::string& value) {
    std::string normalized = Trim(value);
    std::string packed = DecodePackedDatastreamText(normalized);
    if (!packed.empty()) {
        normalized = Trim(packed);
    }
    if (!LooksLikeJson(normalized)) {
        return "";
    }

    cJSON* root = cJSON_ParseWithLength(normalized.c_str(), normalized.size());
    if (root == nullptr) {
        return "";
    }

    std::string out;
    cJSON* object = cJSON_GetObjectItemCaseSensitive(root, "object");
    if (cJSON_IsString(object) && object->valuestring != nullptr &&
        strcmp(object->valuestring, "message.user") == 0) {
        cJSON* content = cJSON_GetObjectItemCaseSensitive(root, "content");
        if (ExtractActionJsonString(content, &out, 0)) {
            cJSON_Delete(root);
            return out;
        }
    }

    cJSON_Delete(root);
    return "";
}

bool ExtractActionJsonString(cJSON* node, std::string* out, int depth) {
    if (node == nullptr || out == nullptr || depth > 4) {
        return false;
    }

    if (cJSON_IsString(node) && node->valuestring != nullptr) {
        std::string value = Trim(node->valuestring);
        std::string packed = DecodePackedDatastreamText(value);
        if (!packed.empty()) {
            value = Trim(packed);
        }
        if (!LooksLikeJson(value)) {
            return false;
        }
        cJSON* nested = cJSON_ParseWithLength(value.c_str(), value.size());
        if (nested == nullptr) {
            return false;
        }
        bool found = ExtractActionJsonString(nested, out, depth + 1);
        cJSON_Delete(nested);
        return found;
    }

    if (cJSON_IsArray(node)) {
        cJSON* child = nullptr;
        cJSON_ArrayForEach(child, node) {
            if (ExtractActionJsonString(child, out, depth + 1)) {
                return true;
            }
        }
        return false;
    }

    if (!cJSON_IsObject(node)) {
        return false;
    }

    cJSON* action_type = cJSON_GetObjectItemCaseSensitive(node, "action_type");
    if (cJSON_IsString(action_type) && action_type->valuestring != nullptr) {
        return SerializeJsonNode(node, out);
    }

    cJSON* executor = cJSON_GetObjectItemCaseSensitive(node, "executor");
    if (cJSON_IsString(executor) && executor->valuestring != nullptr) {
        return SerializeJsonNode(node, out);
    }

    cJSON* actions = cJSON_GetObjectItemCaseSensitive(node, "actions");
    if (cJSON_IsArray(actions)) {
        return SerializeJsonNode(node, out);
    }

    static constexpr const char* kContainerKeys[] = {"content", "payload", "message", "data", "actions"};
    for (const char* key : kContainerKeys) {
        cJSON* child = cJSON_GetObjectItemCaseSensitive(node, key);
        if (ExtractActionJsonString(child, out, depth + 1)) {
            return true;
        }
    }

    cJSON* child = nullptr;
    cJSON_ArrayForEach(child, node) {
        if (ExtractActionJsonString(child, out, depth + 1)) {
            return true;
        }
    }
    return false;
}

std::string ExtractDatastreamPayloadFromJson(cJSON* root, int depth) {
    if (root == nullptr || depth > 3) {
        return "";
    }

    if (!cJSON_IsObject(root)) {
        return "";
    }

    cJSON* object = cJSON_GetObjectItemCaseSensitive(root, "object");
    if (!cJSON_IsString(object) || object->valuestring == nullptr ||
        strcmp(object->valuestring, "message.user") != 0) {
        return "";
    }

    cJSON* content = cJSON_GetObjectItemCaseSensitive(root, "content");
    std::string action_payload;
    if (ExtractActionJsonString(content, &action_payload, 0)) {
        return action_payload;
    }
    return "";
}

std::string NormalizeDatastreamPayload(const std::string& value) {
    std::string normalized = Trim(PercentDecode(value));
    if (normalized.empty()) {
        return "";
    }

    std::string packed = DecodePackedDatastreamText(normalized);
    if (!packed.empty()) {
        normalized = Trim(packed);
    }

    if (LooksLikeJson(normalized)) {
        cJSON* nested = cJSON_ParseWithLength(normalized.c_str(), normalized.size());
        if (nested != nullptr) {
            std::string text = ExtractDatastreamPayloadFromJson(nested, 0);
            cJSON_Delete(nested);
            return text;
        }
    }

    return "";
}

bool TryExtractStructuredDatastreamPayload(const std::string& payload, std::string* text) {
    if (text == nullptr) {
        return false;
    }

    cJSON* root = cJSON_ParseWithLength(payload.c_str(), payload.size());
    if (root != nullptr) {
        *text = ExtractDatastreamPayloadFromJson(root, 0);
        cJSON_Delete(root);
        return !text->empty();
    }

    std::string normalized = Trim(PercentDecode(payload));
    std::string packed = DecodePackedDatastreamText(normalized);
    if (!packed.empty()) {
        cJSON* packed_root = cJSON_ParseWithLength(packed.c_str(), packed.size());
        if (packed_root != nullptr) {
            *text = ExtractDatastreamPayloadFromJson(packed_root, 0);
            cJSON_Delete(packed_root);
            return !text->empty();
        }
        return false;
    }

    if (LooksLikeJsonFragment(normalized) || normalized.find('|') != std::string::npos) {
        return false;
    }

    *text = NormalizeDatastreamPayload(normalized);
    return !text->empty();
}
}

AgoraRtcSession* AgoraRtcSession::instance_ = nullptr;

bool AgoraRtcSession::Start(const AgoraAgentConfig& config) {
    if (started_) {
        return true;
    }
    if (config.app_id.empty()) {
        SetError("Agora app id is empty");
        return false;
    }
    if (config.channel_name.empty()) {
        SetError("Agora channel name is empty");
        return false;
    }
    if (config.rtc_token.empty()) {
        SetError("Agora rtc token is empty");
        return false;
    }

    instance_ = this;
    config_ = config;
    app_id_ = config.app_id;

    memset(&event_handler_, 0, sizeof(event_handler_));
    event_handler_.on_join_channel_success = &AgoraRtcSession::OnJoinChannelSuccess;
    event_handler_.on_connection_lost = &AgoraRtcSession::OnConnectionLost;
    event_handler_.on_rejoin_channel_success = &AgoraRtcSession::OnRejoinChannelSuccess;
    event_handler_.on_user_joined = &AgoraRtcSession::OnUserJoined;
    event_handler_.on_user_offline = &AgoraRtcSession::OnUserOffline;
    event_handler_.on_audio_data = &AgoraRtcSession::OnAudioData;
    event_handler_.on_mixed_audio_data = &AgoraRtcSession::OnMixedAudioData;
    event_handler_.on_stream_message = &AgoraRtcSession::OnStreamMessage;
    event_handler_.on_error = &AgoraRtcSession::OnError;

    memset(&service_opt_, 0, sizeof(service_opt_));
    service_opt_.area_code = AREA_CODE_GLOB;
    service_opt_.log_cfg.log_disable = true;
    service_opt_.log_cfg.log_level = RTC_LOG_WARNING;
    service_opt_.log_cfg.log_path = "io.agora.rtc_sdk";

    ESP_LOGI(kTag, "Init RTC with persistent handler=%p service_opt=%p", &event_handler_, &service_opt_);
    int rval = agora_rtc_init(app_id_.c_str(), &event_handler_, &service_opt_);
    if (rval < 0) {
        instance_ = nullptr;
        SetError(std::string("agora_rtc_init failed: ") + agora_rtc_err_2_str(rval));
        return false;
    }

    rval = agora_rtc_create_connection(&conn_id_);
    if (rval < 0) {
        instance_ = nullptr;
        SetError(std::string("agora_rtc_create_connection failed: ") + agora_rtc_err_2_str(rval));
        agora_rtc_fini();
        return false;
    }

    rtc_channel_options_t channel_options = {};
    channel_options.auto_subscribe_audio = true;
    channel_options.auto_subscribe_video = false;
    channel_options.audio_codec_opt.audio_codec_type = static_cast<audio_codec_type_e>(config_.rtc_audio_codec_type);
    channel_options.audio_codec_opt.pcm_sample_rate = config_.pcm_sample_rate;
    channel_options.audio_codec_opt.pcm_channel_num = 1;
    channel_options.audio_codec_opt.pcm_duration = 20;

    ESP_LOGI(kTag,
        "Join RTC: channel=%s uid=%lu codec=%d sample_rate=%d channels=%d duration=%d sub_audio=%d sub_video=%d jitter=%d mixer=%d",
        config_.channel_name.c_str(),
        static_cast<unsigned long>(config_.remote_rtc_uid),
        channel_options.audio_codec_opt.audio_codec_type,
        channel_options.audio_codec_opt.pcm_sample_rate,
        channel_options.audio_codec_opt.pcm_channel_num,
        channel_options.audio_codec_opt.pcm_duration,
        channel_options.auto_subscribe_audio,
        channel_options.auto_subscribe_video,
        channel_options.enable_audio_jitter_buffer,
        channel_options.enable_audio_mixer);

    rval = agora_rtc_join_channel(conn_id_, config_.channel_name.c_str(), config_.remote_rtc_uid,
        config_.rtc_token.c_str(), &channel_options);
    if (rval < 0) {
        instance_ = nullptr;
        SetError(std::string("agora_rtc_join_channel failed: ") + agora_rtc_err_2_str(rval));
        agora_rtc_destroy_connection(conn_id_);
        conn_id_ = CONNECTION_ID_INVALID;
        agora_rtc_fini();
        return false;
    }

    started_ = true;
    last_error_.clear();
    return true;
}

void AgoraRtcSession::Stop() {
    auto conn_id = conn_id_;
    bool was_started = started_;
    conn_id_ = CONNECTION_ID_INVALID;
    started_ = false;
    in_channel_ = false;
    instance_ = nullptr;
    {
        std::lock_guard<std::mutex> lock(stream_buffer_mutex_);
        stream_buffers_.clear();
        last_stream_texts_.clear();
    }
    if (conn_id != CONNECTION_ID_INVALID) {
        agora_rtc_leave_channel(conn_id);
        vTaskDelay(pdMS_TO_TICKS(200));
        agora_rtc_destroy_connection(conn_id);
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (was_started || conn_id != CONNECTION_ID_INVALID) {
        agora_rtc_fini();
    }
}

bool AgoraRtcSession::SendAudio(const void* data, size_t len) {
    if (!started_ || conn_id_ == CONNECTION_ID_INVALID) {
        return false;
    }
    audio_frame_info_t info = {};
    info.data_type = AUDIO_DATA_TYPE_PCM;
    int rval = agora_rtc_send_audio_data(conn_id_, data, len, &info);
    if (rval < 0) {
        ESP_LOGE(kTag, "SendAudio failed: rval=%d len=%u started=%d in_channel=%d conn_id=%lu",
            rval, static_cast<unsigned>(len), started_, in_channel_,
            static_cast<unsigned long>(conn_id_));
    }
    return rval >= 0;
}

bool AgoraRtcSession::SendVideo(const void* data, size_t len, video_data_type_e data_type, bool key_frame, uint16_t frame_rate) {
    if (!started_ || conn_id_ == CONNECTION_ID_INVALID) {
        return false;
    }
    video_frame_info_t info = {};
    info.data_type = data_type;
    info.stream_type = VIDEO_STREAM_HIGH;
    info.frame_type = key_frame ? VIDEO_FRAME_KEY : VIDEO_FRAME_DELTA;
    info.frame_rate = frame_rate;
    info.rotation = VIDEO_ORIENTATION_0;
    int rval = agora_rtc_send_video_data(conn_id_, data, len, &info);
    return rval >= 0;
}

void AgoraRtcSession::SetError(const std::string& error) {
    last_error_ = error;
    ESP_LOGE(kTag, "%s", error.c_str());
}

void AgoraRtcSession::HandleIncomingAudio(const void* data_ptr, size_t data_len) {
    if (!started_) {
        return;
    }
    in_channel_ = true;
    if (incoming_audio_callback_) {
        incoming_audio_callback_(data_ptr, data_len);
    }
}

void AgoraRtcSession::HandleStreamMessage(uint32_t uid, const std::string& text) {
    if (!started_) {
        return;
    }
    in_channel_ = true;
    if (text.empty() || stream_message_callback_ == nullptr) {
        return;
    }

    stream_message_callback_(uid, text);
}

void AgoraRtcSession::HandleStreamChunk(uint32_t uid, const char* data, size_t length) {
    if (data == nullptr || length == 0) {
        return;
    }

    std::string chunk(data, length);
    std::string chunk_preview = SummarizeStreamPreview(data, length);
    std::string object_name = ExtractObjectNameForLog(chunk_preview);
    bool message_user_chunk = object_name == "message.user" || ContainsMessageUserMarker(chunk_preview);
    std::string text;

    if (message_user_chunk) {
        ESP_LOGD(kTag, "Raw message.user chunk uid=%lu len=%u preview=%s",
            static_cast<unsigned long>(uid), static_cast<unsigned>(length), ShortenForLog(chunk_preview).c_str());
        text = ExtractMessageUserActionPayloadDirect(chunk);
        if (!text.empty()) {
            ESP_LOGI(kTag, "Datastream dispatch uid=%lu payload=%s",
                static_cast<unsigned long>(uid), ShortenForLog(text).c_str());
            HandleStreamMessage(uid, text);
            return;
        }
    }
    if (TryExtractStructuredDatastreamPayload(chunk, &text) && !text.empty()) {
        ESP_LOGI(kTag, "Datastream dispatch uid=%lu payload=%s",
            static_cast<unsigned long>(uid), ShortenForLog(text).c_str());
        HandleStreamMessage(uid, text);
    }
}

void AgoraRtcSession::OnJoinChannelSuccess(connection_id_t conn_id, uint32_t uid, int elapsed_ms) {
    if (instance_ == nullptr) {
        return;
    }
    instance_->in_channel_ = true;
    ESP_LOGI(kTag, "Join channel success with UID=%lu: channel=%s conn=%lu elapsed_ms=%d",
        static_cast<unsigned long>(uid),
        instance_->config_.channel_name.c_str(),
        static_cast<unsigned long>(conn_id),
        elapsed_ms);
}

void AgoraRtcSession::OnConnectionLost(connection_id_t conn_id) {
    if (instance_ == nullptr) {
        return;
    }
    instance_->in_channel_ = false;
    ESP_LOGW(kTag, "[conn-%lu] connection lost", static_cast<unsigned long>(conn_id));
}

void AgoraRtcSession::OnRejoinChannelSuccess(connection_id_t conn_id, uint32_t uid, int elapsed_ms) {
    if (instance_ == nullptr) {
        return;
    }
    instance_->in_channel_ = true;
    ESP_LOGI(kTag, "Rejoin channel success: channel=%s conn=%lu uid=%lu elapsed_ms=%d",
        instance_->config_.channel_name.c_str(),
        static_cast<unsigned long>(conn_id),
        static_cast<unsigned long>(uid),
        elapsed_ms);
}

void AgoraRtcSession::OnUserJoined(connection_id_t conn_id, uint32_t uid, int elapsed_ms) {
    if (instance_ != nullptr) {
        instance_->in_channel_ = true;
        ESP_LOGI(kTag, "Mark channel active from remote user join: channel=%s conn=%lu remote_uid=%lu",
            instance_->config_.channel_name.c_str(),
            static_cast<unsigned long>(conn_id),
            static_cast<unsigned long>(uid));
    }
    ESP_LOGI(kTag, "[conn-%lu] remote user %lu joined in %d ms",
        static_cast<unsigned long>(conn_id), static_cast<unsigned long>(uid), elapsed_ms);
}

void AgoraRtcSession::OnUserOffline(connection_id_t conn_id, uint32_t uid, int reason) {
    ESP_LOGI(kTag, "[conn-%lu] remote user %lu left, reason=%d",
        static_cast<unsigned long>(conn_id), static_cast<unsigned long>(uid), reason);
}

void AgoraRtcSession::OnAudioData(connection_id_t conn_id, uint32_t uid, uint16_t sent_ts, const void* data_ptr,
    size_t data_len, const audio_frame_info_t* info_ptr) {
    (void)conn_id;
    (void)uid;
    (void)sent_ts;
    (void)info_ptr;
    if (instance_ != nullptr) {
        instance_->HandleIncomingAudio(data_ptr, data_len);
    }
}

void AgoraRtcSession::OnMixedAudioData(connection_id_t conn_id, const void* data_ptr, size_t data_len,
    const audio_frame_info_t* info_ptr) {
    (void)conn_id;
    (void)info_ptr;
    if (instance_ != nullptr) {
        instance_->HandleIncomingAudio(data_ptr, data_len);
    }
}

void AgoraRtcSession::OnStreamMessage(connection_id_t conn_id, uint32_t uid, int stream_id, const char* data,
    size_t length, uint64_t sent_ts) {
    (void)conn_id;
    (void)stream_id;
    (void)sent_ts;
    if (instance_ == nullptr) {
        return;
    }
    if (!instance_->stream_message_callback_) {
        return;
    }

    instance_->HandleStreamChunk(uid, data, length);
}

void AgoraRtcSession::OnError(connection_id_t conn_id, int code, const char* msg) {
    if (instance_ == nullptr) {
        return;
    }
    instance_->SetError(std::string("[conn-") + std::to_string(conn_id) + "] " + std::to_string(code) + ": " + (msg ? msg : ""));
}
