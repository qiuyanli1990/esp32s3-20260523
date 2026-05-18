#include "voiceprint/voiceprint_uploader.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <string_view>
#include <vector>

#include <esp_crt_bundle.h>
#include <esp_heap_caps.h>
#include <esp_http_client.h>
#include <esp_log.h>
#include <esp_random.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <apps/esp_sntp.h>
#include <mbedtls/base64.h>
#include <mbedtls/md.h>

#include "agora_project_config.h"
#include "audio_codec.h"
#include "audio_service.h"
#include "board.h"

namespace {

constexpr char kTag[] = "Voiceprint";
constexpr int kSampleRate = 16000;
constexpr int kFrameMs = 20;
constexpr int kFrameSamples = kSampleRate * kFrameMs / 1000;
constexpr int kMinCaptureDurationMs = 10000;
constexpr int kMaxCaptureDurationMs = 30000;
constexpr int kMinOutputDurationMs = 10000;
constexpr int kMaxOutputDurationMs = 15000;
constexpr int kDefaultHttpTimeoutMs = 30000;

extern const char voiceprint_start_ogg_start[] asm("_binary_voiceprint_start_ogg_start");
extern const char voiceprint_start_ogg_end[] asm("_binary_voiceprint_start_ogg_end");
extern const char voiceprint_uploading_ogg_start[] asm("_binary_voiceprint_uploading_ogg_start");
extern const char voiceprint_uploading_ogg_end[] asm("_binary_voiceprint_uploading_ogg_end");
extern const char voiceprint_upload_failed_ogg_start[] asm("_binary_voiceprint_upload_failed_ogg_start");
extern const char voiceprint_upload_failed_ogg_end[] asm("_binary_voiceprint_upload_failed_ogg_end");
extern const char voiceprint_upload_success_ogg_start[] asm("_binary_voiceprint_upload_success_ogg_start");
extern const char voiceprint_upload_success_ogg_end[] asm("_binary_voiceprint_upload_success_ogg_end");

struct HttpResponseBuffer {
    char* data = nullptr;
    int capacity = 0;
    int length = 0;
};

struct VoiceFrameInfo {
    bool is_voice = false;
    int peak = 0;
    int rms = 0;
};

struct WindowScore {
    size_t start_frame = 0;
    size_t frame_count = 0;
    int effective_ms = 0;
    int voiced_frames = 0;
    int effective_frames = 0;
    int max_peak = 0;
    int max_rms = 0;
};

std::string_view VoiceprintStartPrompt() {
    return {voiceprint_start_ogg_start,
        static_cast<size_t>(voiceprint_start_ogg_end - voiceprint_start_ogg_start)};
}

std::string_view VoiceprintUploadingPrompt() {
    return {voiceprint_uploading_ogg_start,
        static_cast<size_t>(voiceprint_uploading_ogg_end - voiceprint_uploading_ogg_start)};
}

std::string_view VoiceprintUploadFailedPrompt() {
    return {voiceprint_upload_failed_ogg_start,
        static_cast<size_t>(voiceprint_upload_failed_ogg_end - voiceprint_upload_failed_ogg_start)};
}

std::string_view VoiceprintUploadSuccessPrompt() {
    return {voiceprint_upload_success_ogg_start,
        static_cast<size_t>(voiceprint_upload_success_ogg_end - voiceprint_upload_success_ogg_start)};
}

void PlayPrompt(AudioService& audio_service, std::string_view prompt) {
    if (prompt.empty()) {
        return;
    }

    audio_service.PlaySound(prompt);
    audio_service.WaitForPlaybackQueueEmpty();
    vTaskDelay(pdMS_TO_TICKS(150));
}

esp_err_t HttpEventHandler(esp_http_client_event_t* evt) {
    if (evt == nullptr || evt->user_data == nullptr) {
        return ESP_OK;
    }

    auto* response = static_cast<HttpResponseBuffer*>(evt->user_data);
    if (evt->event_id == HTTP_EVENT_ON_DATA && evt->data != nullptr && evt->data_len > 0 &&
        response->data != nullptr && response->capacity > 0 && response->length < response->capacity - 1) {
        int copy_len = std::min(evt->data_len, response->capacity - response->length - 1);
        memcpy(response->data + response->length, evt->data, copy_len);
        response->length += copy_len;
        response->data[response->length] = '\0';
    }

    return ESP_OK;
}

bool IsBlank(const char* value) {
    if (value == nullptr) {
        return true;
    }
    while (*value != '\0') {
        if (!std::isspace(static_cast<unsigned char>(*value))) {
            return false;
        }
        ++value;
    }
    return true;
}

std::string SanitizePathSegment(const std::string& value) {
    std::string out;
    out.reserve(value.size());
    for (char ch : value) {
        unsigned char uch = static_cast<unsigned char>(ch);
        if (std::isalnum(uch) || ch == '-' || ch == '_' || ch == '.') {
            out.push_back(ch);
        } else {
            out.push_back('_');
        }
    }
    return out.empty() ? "unknown" : out;
}

std::string SanitizePathPrefix(const std::string& value) {
    std::string trimmed = value;
    while (!trimmed.empty() && trimmed.front() == '/') {
        trimmed.erase(trimmed.begin());
    }
    while (!trimmed.empty() && trimmed.back() == '/') {
        trimmed.pop_back();
    }
    std::string out;
    out.reserve(trimmed.size());
    bool last_was_slash = false;
    for (char ch : trimmed) {
        unsigned char uch = static_cast<unsigned char>(ch);
        if (std::isalnum(uch) || ch == '-' || ch == '_' || ch == '.') {
            out.push_back(ch);
            last_was_slash = false;
        } else if (ch == '/') {
            if (!out.empty() && !last_was_slash) {
                out.push_back('/');
                last_was_slash = true;
            }
        } else {
            out.push_back('_');
            last_was_slash = false;
        }
    }
    while (!out.empty() && out.front() == '/') {
        out.erase(out.begin());
    }
    while (!out.empty() && out.back() == '/') {
        out.pop_back();
    }
    return out.empty() ? "voiceprints" : out;
}

std::string PercentEncodePath(const std::string& value) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(value.size());
    for (unsigned char ch : value) {
        if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
            (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' ||
            ch == '.' || ch == '~' || ch == '/') {
            out.push_back(static_cast<char>(ch));
        } else {
            out.push_back('%');
            out.push_back(kHex[ch >> 4]);
            out.push_back(kHex[ch & 0x0F]);
        }
    }
    return out;
}

int CaptureDurationMs() {
    return std::clamp(static_cast<int>(VOICEPRINT_CAPTURE_DURATION_MS),
        kMinCaptureDurationMs, kMaxCaptureDurationMs);
}

int OutputDurationMs() {
    return std::clamp(static_cast<int>(VOICEPRINT_OUTPUT_DURATION_MS),
        kMinOutputDurationMs, std::min(kMaxOutputDurationMs, CaptureDurationMs()));
}

int MinEffectiveAudioMs() {
    return std::clamp(static_cast<int>(VOICEPRINT_MIN_EFFECTIVE_AUDIO_MS),
        0, OutputDurationMs());
}

int VadHangoverMs() {
    return std::clamp(static_cast<int>(VOICEPRINT_VAD_HANGOVER_MS), 0, 1000);
}

int IntegerSqrt(int64_t value) {
    if (value <= 0) {
        return 0;
    }
    int64_t low = 1;
    int64_t high = std::min<int64_t>(value, 32767);
    int64_t result = 0;
    while (low <= high) {
        int64_t mid = low + (high - low) / 2;
        int64_t square = mid * mid;
        if (square <= value) {
            result = mid;
            low = mid + 1;
        } else {
            high = mid - 1;
        }
    }
    return static_cast<int>(result);
}

VoiceFrameInfo AnalyzeVoiceFrame(const int16_t* samples, size_t sample_count) {
    VoiceFrameInfo info;
    if (samples == nullptr || sample_count == 0) {
        return info;
    }

    int64_t sum = 0;
    int64_t sum_squares = 0;
    for (size_t i = 0; i < sample_count; ++i) {
        int32_t sample = samples[i];
        sum += sample;
        sum_squares += static_cast<int64_t>(sample) * sample;
        int peak = sample < 0 ? -sample : sample;
        info.peak = std::max(info.peak, peak);
    }

    int64_t mean = sum / static_cast<int64_t>(sample_count);
    int64_t mean_square = sum_squares / static_cast<int64_t>(sample_count);
    int64_t variance = mean_square - mean * mean;
    info.rms = IntegerSqrt(variance);
    int64_t threshold = static_cast<int64_t>(VOICEPRINT_VAD_RMS_THRESHOLD);
    info.is_voice = variance >= threshold * threshold;
    return info;
}

WindowScore ScoreWindow(const std::vector<VoiceFrameInfo>& frames, size_t start_frame, size_t frame_count) {
    WindowScore score;
    score.start_frame = start_frame;
    score.frame_count = frame_count;

    int hangover_ms = 0;
    const size_t end_frame = std::min(frames.size(), start_frame + frame_count);
    for (size_t i = start_frame; i < end_frame; ++i) {
        const VoiceFrameInfo& frame = frames[i];
        score.max_peak = std::max(score.max_peak, frame.peak);
        score.max_rms = std::max(score.max_rms, frame.rms);
        if (frame.is_voice) {
            ++score.voiced_frames;
            hangover_ms = VadHangoverMs();
        }
        if (frame.is_voice || hangover_ms > 0) {
            score.effective_ms += kFrameMs;
            ++score.effective_frames;
        }
        if (!frame.is_voice && hangover_ms > 0) {
            hangover_ms = std::max(0, hangover_ms - kFrameMs);
        }
    }

    return score;
}

WindowScore SelectBestWindow(const std::vector<VoiceFrameInfo>& frames, size_t frame_count) {
    WindowScore best;
    if (frames.empty() || frame_count == 0) {
        return best;
    }

    frame_count = std::min(frame_count, frames.size());
    const size_t last_start = frames.size() - frame_count;
    bool has_best = false;
    for (size_t start = 0; start <= last_start; ++start) {
        WindowScore score = ScoreWindow(frames, start, frame_count);
        if (!has_best || score.effective_ms > best.effective_ms ||
            (score.effective_ms == best.effective_ms && score.voiced_frames > best.voiced_frames)) {
            best = score;
            has_best = true;
        }
    }
    return best;
}

std::string MakeRfc1123Date() {
    time_t now = 0;
    time(&now);

    struct tm utc = {};
    gmtime_r(&now, &utc);

    char buf[40] = {};
    if (strftime(buf, sizeof(buf), "%a, %d %b %Y %H:%M:%S GMT", &utc) == 0) {
        return "";
    }
    return buf;
}

std::string HmacSha1Base64(const std::string& key, const std::string& data) {
    const mbedtls_md_info_t* md_info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA1);
    if (md_info == nullptr) {
        return "";
    }

    unsigned char digest[20] = {};
    int ret = mbedtls_md_hmac(md_info,
        reinterpret_cast<const unsigned char*>(key.data()), key.size(),
        reinterpret_cast<const unsigned char*>(data.data()), data.size(),
        digest);
    if (ret != 0) {
        ESP_LOGE(kTag, "mbedtls_md_hmac failed: %d", ret);
        return "";
    }

    unsigned char encoded[64] = {};
    size_t encoded_len = 0;
    ret = mbedtls_base64_encode(encoded, sizeof(encoded), &encoded_len,
        digest, sizeof(digest));
    if (ret != 0) {
        ESP_LOGE(kTag, "mbedtls_base64_encode failed: %d", ret);
        return "";
    }
    return std::string(reinterpret_cast<const char*>(encoded), encoded_len);
}

std::string BuildObjectKey() {
    std::string prefix = SanitizePathPrefix(VOICEPRINT_OSS_OBJECT_PREFIX);
    std::string device_id = SanitizePathSegment(Board::GetInstance().GetUuid());

    time_t now = 0;
    time(&now);
    uint32_t timestamp = now > 1700000000 ?
        static_cast<uint32_t>(now) :
        static_cast<uint32_t>(esp_timer_get_time() / 1000);

    char filename[64] = {};
    snprintf(filename, sizeof(filename), "%lu_%08lx.pcm",
        static_cast<unsigned long>(timestamp),
        static_cast<unsigned long>(esp_random()));

    return prefix + "/" + device_id + "/" + filename;
}

std::string BuildOssHost(const std::string& bucket, const std::string& endpoint) {
    return bucket + "." + endpoint;
}

std::string BuildDefaultPublicBaseUrl(const std::string& bucket, const std::string& endpoint) {
    return "https://" + BuildOssHost(bucket, endpoint);
}

std::string BuildSampleUrl(const std::string& object_key) {
    std::string bucket = VOICEPRINT_OSS_BUCKET;
    std::string endpoint = VOICEPRINT_OSS_ENDPOINT;
    std::string base_url = VOICEPRINT_OSS_PUBLIC_BASE_URL;
    if (base_url.empty()) {
        base_url = BuildDefaultPublicBaseUrl(bucket, endpoint);
    }
    while (!base_url.empty() && base_url.back() == '/') {
        base_url.pop_back();
    }
    return base_url + "/" + PercentEncodePath(object_key);
}

bool IsSystemTimeSynced() {
    time_t now = 0;
    time(&now);
    return now > 1700000000;
}

bool EnsureSystemTimeSynced() {
    if (IsSystemTimeSynced()) {
        return true;
    }

    if (!esp_sntp_enabled()) {
        ESP_LOGI(kTag, "Starting SNTP time sync before OSS upload");
        esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
        esp_sntp_setservername(0, "ntp.aliyun.com");
        esp_sntp_init();
    }

    const int timeout_ms = std::max(0, static_cast<int>(VOICEPRINT_SNTP_SYNC_TIMEOUT_MS));
    int waited_ms = 0;
    while (waited_ms < timeout_ms) {
        if (IsSystemTimeSynced()) {
            ESP_LOGI(kTag, "System time synchronized for OSS upload");
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(200));
        waited_ms += 200;
    }
    return IsSystemTimeSynced();
}

bool ValidateConfig(std::string* error) {
    if (IsBlank(VOICEPRINT_OSS_BUCKET) || IsBlank(VOICEPRINT_OSS_ENDPOINT)) {
        if (error != nullptr) {
            *error = "OSS bucket or endpoint is not configured";
        }
        return false;
    }
    std::string bucket = VOICEPRINT_OSS_BUCKET;
    std::string endpoint = VOICEPRINT_OSS_ENDPOINT;
    if (endpoint.find("://") != std::string::npos ||
        endpoint.find('/') != std::string::npos ||
        endpoint.find(':') != std::string::npos ||
        endpoint.rfind(bucket + ".", 0) == 0) {
        if (error != nullptr) {
            *error = "VOICEPRINT_OSS_ENDPOINT must be a region endpoint like oss-cn-shanghai.aliyuncs.com";
        }
        return false;
    }
    if (IsBlank(VOICEPRINT_STS_ACCESS_KEY_ID) ||
        IsBlank(VOICEPRINT_STS_ACCESS_KEY_SECRET) ||
        IsBlank(VOICEPRINT_STS_SECURITY_TOKEN)) {
        if (error != nullptr) {
            *error = "STS AccessKeyId, AccessKeySecret, or SecurityToken is not configured";
        }
        return false;
    }
    return true;
}

bool CapturePcm(AudioService& audio_service, AudioCodec* codec, std::vector<int16_t>* pcm,
    int* effective_audio_ms, std::string* error,
    const std::function<void(const char*)>& status_callback) {
    if (codec == nullptr || pcm == nullptr || effective_audio_ms == nullptr) {
        if (error != nullptr) {
            *error = "Invalid audio capture arguments";
        }
        return false;
    }

    const int capture_duration_ms = CaptureDurationMs();
    const int output_duration_ms = OutputDurationMs();
    const int target_samples = kSampleRate * capture_duration_ms / 1000;
    std::vector<int16_t> captured_pcm;
    captured_pcm.reserve(target_samples);
    pcm->clear();
    pcm->reserve(kSampleRate * output_duration_ms / 1000);
    *effective_audio_ms = 0;

    const bool input_was_enabled = codec->input_enabled();
    if (codec->output_enabled()) {
        codec->EnableOutput(false);
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    auto finish_capture = [&audio_service, codec, input_was_enabled]() {
        audio_service.SetAudioHardwareBusy(false);
        if (!input_was_enabled && codec->input_enabled()) {
            codec->EnableInput(false);
        }
    };

    audio_service.SetAudioHardwareBusy(true);
    if (status_callback) {
        status_callback("Voiceprint recording: keep speaking");
    }
    ESP_LOGI(kTag, "Capture voiceprint PCM: capture_window=%dms upload_duration=%dms sample_rate=%dHz frame=%dms free_internal=%u free_psram=%u",
        capture_duration_ms, output_duration_ms, kSampleRate, kFrameMs,
        static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
        static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));

    int captured_ms = 0;
    int consecutive_failures = 0;
    std::vector<int16_t> raw;
    std::vector<int16_t> mono;
    std::vector<VoiceFrameInfo> frame_infos;
    std::vector<size_t> frame_starts;
    frame_infos.reserve(capture_duration_ms / kFrameMs + 1);
    frame_starts.reserve(capture_duration_ms / kFrameMs + 1);

    while (captured_ms < capture_duration_ms) {
        raw.clear();
        if (!audio_service.ReadAudioData(raw, kSampleRate, kFrameSamples)) {
            if (++consecutive_failures > 100) {
                finish_capture();
                if (error != nullptr) {
                    *error = "Timed out reading microphone audio";
                }
                return false;
            }
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        consecutive_failures = 0;

        const int input_channels = std::max(1, codec->input_channels());
        const int capture_channel = std::clamp(codec->raw_capture_channel_index(), 0, input_channels - 1);
        const size_t mono_samples = raw.size() / static_cast<size_t>(input_channels);
        if (mono_samples == 0) {
            continue;
        }

        mono.resize(mono_samples);
        if (input_channels > 1) {
            for (size_t i = 0, j = static_cast<size_t>(capture_channel); i < mono_samples; ++i, j += input_channels) {
                mono[i] = raw[j];
            }
        } else {
            memcpy(mono.data(), raw.data(), mono_samples * sizeof(int16_t));
        }

        const int chunk_ms = static_cast<int>(mono_samples * 1000 / kSampleRate);
        VoiceFrameInfo frame_info = AnalyzeVoiceFrame(mono.data(), mono.size());
        captured_ms += chunk_ms;

        if ((captured_pcm.size() + mono.size()) * sizeof(int16_t) > VOICEPRINT_MAX_PCM_BYTES) {
            finish_capture();
            if (error != nullptr) {
                *error = "Voiceprint PCM exceeds configured max file size";
            }
            return false;
        }
        frame_starts.push_back(captured_pcm.size());
        frame_infos.push_back(frame_info);
        captured_pcm.insert(captured_pcm.end(), mono.begin(), mono.end());
    }

    finish_capture();

    const size_t output_frame_count = static_cast<size_t>(output_duration_ms / kFrameMs);
    if (frame_infos.size() < output_frame_count || frame_starts.size() != frame_infos.size()) {
        if (error != nullptr) {
            *error = "Captured voiceprint audio is shorter than the output window";
        }
        return false;
    }

    WindowScore best = SelectBestWindow(frame_infos, output_frame_count);
    *effective_audio_ms = best.effective_ms;

    if (*effective_audio_ms < MinEffectiveAudioMs()) {
        ESP_LOGW(kTag,
            "Voiceprint effective audio too short: capture_window=%dms upload_window=%dms best_start=%ums effective=%dms min=%dms max_peak=%d max_rms=%d threshold=%d hangover=%dms voiced_frames=%d effective_frames=%d/%u",
            capture_duration_ms, output_duration_ms,
            static_cast<unsigned>(best.start_frame * kFrameMs),
            *effective_audio_ms, MinEffectiveAudioMs(), best.max_peak,
            best.max_rms,
            static_cast<int>(VOICEPRINT_VAD_RMS_THRESHOLD),
            VadHangoverMs(), best.voiced_frames, best.effective_frames,
            static_cast<unsigned>(output_frame_count));
        if (error != nullptr) {
            *error = "Effective voice audio is too short: " +
                std::to_string(*effective_audio_ms) + "ms";
        }
        if (status_callback) {
            status_callback("Voiceprint recording failed: not enough speech");
        }
        return false;
    }

    const size_t start_sample = frame_starts[best.start_frame];
    const size_t end_frame = std::min(frame_starts.size(), best.start_frame + best.frame_count);
    const size_t end_sample = end_frame < frame_starts.size() ? frame_starts[end_frame] : captured_pcm.size();
    if (end_sample <= start_sample) {
        if (error != nullptr) {
            *error = "Failed to select voiceprint output window";
        }
        return false;
    }

    if ((end_sample - start_sample) * sizeof(int16_t) > VOICEPRINT_MAX_PCM_BYTES) {
        if (error != nullptr) {
            *error = "Selected voiceprint PCM exceeds configured max file size";
        }
        return false;
    }

    pcm->assign(captured_pcm.begin() + start_sample, captured_pcm.begin() + end_sample);
    const int selected_duration_ms = static_cast<int>(pcm->size() * 1000 / kSampleRate);

    ESP_LOGI(kTag,
        "Voiceprint PCM captured: capture_window=%dms selected_start=%ums selected_duration=%dms bytes=%u effective_audio=%dms max_peak=%d max_rms=%d threshold=%d hangover=%dms voiced_frames=%d effective_frames=%d/%u",
        capture_duration_ms, static_cast<unsigned>(best.start_frame * kFrameMs),
        selected_duration_ms,
        static_cast<unsigned>(pcm->size() * sizeof(int16_t)), *effective_audio_ms,
        best.max_peak, best.max_rms, static_cast<int>(VOICEPRINT_VAD_RMS_THRESHOLD),
        VadHangoverMs(), best.voiced_frames, best.effective_frames,
        static_cast<unsigned>(output_frame_count));
    if (status_callback) {
        status_callback("Voiceprint recording finished, uploading");
    }
    return true;
}

bool UploadToOss(const std::vector<int16_t>& pcm, const std::string& object_key,
    std::string* sample_url, std::string* error) {
    std::string bucket = VOICEPRINT_OSS_BUCKET;
    std::string endpoint = VOICEPRINT_OSS_ENDPOINT;
    std::string host = BuildOssHost(bucket, endpoint);
    std::string encoded_object_key = PercentEncodePath(object_key);
    std::string url = "https://" + host + "/" + encoded_object_key;
    if (!EnsureSystemTimeSynced()) {
        if (error != nullptr) {
            *error = "System time is not synchronized for OSS Date header";
        }
        return false;
    }
    std::string date = MakeRfc1123Date();
    if (date.empty()) {
        if (error != nullptr) {
            *error = "Failed to build OSS Date header";
        }
        return false;
    }

    const char* content_type = "application/octet-stream";
    const char* security_token = VOICEPRINT_STS_SECURITY_TOKEN;
    std::string canonicalized_headers = std::string("x-oss-security-token:") + security_token + "\n";
    std::string canonicalized_resource = "/" + bucket + "/" + object_key;
    std::string string_to_sign = std::string("PUT\n\n") + content_type + "\n" + date + "\n" +
        canonicalized_headers + canonicalized_resource;
    std::string signature = HmacSha1Base64(VOICEPRINT_STS_ACCESS_KEY_SECRET, string_to_sign);
    if (signature.empty()) {
        if (error != nullptr) {
            *error = "Failed to sign OSS PutObject request";
        }
        return false;
    }

    std::string authorization = std::string("OSS ") + VOICEPRINT_STS_ACCESS_KEY_ID + ":" + signature;
    std::string content_length = std::to_string(pcm.size() * sizeof(int16_t));

    char response_data[512] = {};
    HttpResponseBuffer response = {
        .data = response_data,
        .capacity = static_cast<int>(sizeof(response_data)),
        .length = 0,
    };

    esp_http_client_config_t cfg = {};
    cfg.url = url.c_str();
    cfg.event_handler = HttpEventHandler;
    cfg.user_data = &response;
    cfg.timeout_ms = VOICEPRINT_OSS_HTTP_TIMEOUT_MS > 0 ? VOICEPRINT_OSS_HTTP_TIMEOUT_MS : kDefaultHttpTimeoutMs;
    cfg.buffer_size = VOICEPRINT_OSS_HTTP_RX_BUFFER_BYTES > 0 ? VOICEPRINT_OSS_HTTP_RX_BUFFER_BYTES : 2048;
    cfg.buffer_size_tx = VOICEPRINT_OSS_HTTP_TX_BUFFER_BYTES > 0 ? VOICEPRINT_OSS_HTTP_TX_BUFFER_BYTES : 8192;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == nullptr) {
        if (error != nullptr) {
            *error = "Failed to initialize OSS HTTP client";
        }
        return false;
    }

    esp_http_client_set_method(client, HTTP_METHOD_PUT);
    esp_http_client_set_header(client, "Content-Type", content_type);
    esp_http_client_set_header(client, "Content-Length", content_length.c_str());
    esp_http_client_set_header(client, "Date", date.c_str());
    esp_http_client_set_header(client, "Authorization", authorization.c_str());
    esp_http_client_set_header(client, "x-oss-security-token", security_token);
    esp_http_client_set_post_field(client,
        reinterpret_cast<const char*>(pcm.data()),
        static_cast<int>(pcm.size() * sizeof(int16_t)));

    ESP_LOGI(kTag, "Upload voiceprint PCM to OSS: host=%s object=%s bytes=%s",
        host.c_str(), object_key.c_str(), content_length.c_str());

    esp_err_t err = esp_http_client_perform(client);
    int status_code = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err == ESP_OK && status_code >= 200 && status_code < 300) {
        if (sample_url != nullptr) {
            *sample_url = BuildSampleUrl(object_key);
        }
        ESP_LOGI(kTag, "Voiceprint upload success: http=%d sample_url=%s",
            status_code, sample_url != nullptr ? sample_url->c_str() : "");
        return true;
    }

    if (error != nullptr) {
        *error = "OSS PutObject failed err=";
        *error += esp_err_to_name(err);
        *error += " http=";
        *error += std::to_string(status_code);
        if (response.length > 0) {
            *error += " body=";
            *error += response_data;
        }
    }
    return false;
}

}  // namespace

bool VoiceprintUploader::IsEnabled() {
    return VOICEPRINT_OSS_ENABLED != 0;
}

bool VoiceprintUploader::CaptureAndUpload(AudioService& audio_service, AudioCodec* codec,
    std::string* sample_url, std::string* error,
    std::function<void(const char*)> status_callback) {
    if (!IsEnabled()) {
        if (error != nullptr) {
            *error = "Voiceprint OSS upload is disabled";
        }
        return false;
    }
    if (!ValidateConfig(error)) {
        return false;
    }

    std::vector<int16_t> pcm;
    int effective_audio_ms = 0;
    PlayPrompt(audio_service, VoiceprintStartPrompt());
    if (!CapturePcm(audio_service, codec, &pcm, &effective_audio_ms, error, status_callback)) {
        return false;
    }
    PlayPrompt(audio_service, VoiceprintUploadingPrompt());

    std::string object_key = BuildObjectKey();
    bool ok = UploadToOss(pcm, object_key, sample_url, error);
    if (status_callback) {
        status_callback(ok ? "Voiceprint upload complete" : "Voiceprint upload failed, continuing");
    }
    PlayPrompt(audio_service, ok ? VoiceprintUploadSuccessPrompt() : VoiceprintUploadFailedPrompt());
    return ok;
}
