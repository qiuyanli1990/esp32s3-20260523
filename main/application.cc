#include "application.h"

#include <cJSON.h>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <esp_log.h>
#include <esp_system.h>
#include <esp_heap_caps.h>
#include <initializer_list>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "assets.h"
#include "assets/lang_config.h"
#include "board.h"
#include "display.h"
#include "settings.h"
#include "system_info.h"
#include "voiceprint/voiceprint_uploader.h"
#include <wifi_manager.h>

#define TAG "Application"

namespace {

struct SessionTaskArgs {
    Application* app;
};

// ESP-IDF task stack sizes are specified in bytes.
constexpr uint32_t kAgoraAgentTaskStackBytes = 16 * 1024;
constexpr uint32_t kAgoraRtcTaskStackBytes = 8 * 1024;
constexpr int kDefaultWeatherDisplayDurationMs = 5000;
constexpr char kVoiceprintSettingsNamespace[] = "voiceprint";
constexpr char kVoiceprintSampleUrlKey[] = "sample_url";

struct WeatherDisplayAction {
    std::string condition_id;
    std::string weather_text;
    std::string city;
    std::string temp_text;
    int duration_ms = kDefaultWeatherDisplayDurationMs;
};

struct DatastreamDisplayActions {
    bool saw_action = false;
    std::string first_action_type;
    bool has_weather_action = false;
    bool has_weather = false;
    WeatherDisplayAction weather;
    bool has_emotion_action = false;
    std::string emotion_action_type;
    std::string emotion;
};

std::string GetJsonString(cJSON* object, const char* key) {
    if (object == nullptr || key == nullptr) {
        return "";
    }
    cJSON* item = cJSON_GetObjectItemCaseSensitive(object, key);
    if (cJSON_IsString(item) && item->valuestring != nullptr) {
        return item->valuestring;
    }
    return "";
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

std::string LoadVoiceprintSampleUrl() {
    Settings settings(kVoiceprintSettingsNamespace, false);
    return Trim(settings.GetString(kVoiceprintSampleUrlKey, ""));
}

void SaveVoiceprintSampleUrl(const std::string& sample_url) {
    if (sample_url.empty()) {
        return;
    }
    Settings settings(kVoiceprintSettingsNamespace, true);
    settings.SetString(kVoiceprintSampleUrlKey, sample_url);
}

std::string NormalizeEmotionName(const std::string& value) {
    std::string normalized;
    normalized.reserve(value.size());
    for (char ch : value) {
        unsigned char uch = static_cast<unsigned char>(ch);
        if (std::isalnum(uch)) {
            normalized.push_back(static_cast<char>(std::tolower(uch)));
        } else if (ch == '-' || ch == ' ' || ch == '.' || ch == '_') {
            normalized.push_back('_');
        }
    }

    static const std::unordered_set<std::string> kAllowedEmotions = {
        "neutral", "happy", "laughing", "funny", "sad", "angry", "crying", "loving",
        "embarrassed", "surprised", "shocked", "thinking", "winking", "cool",
        "relaxed", "delicious", "kissy", "confident", "sleepy", "silly", "confused"
    };
    if (kAllowedEmotions.count(normalized) != 0) {
        return normalized;
    }

    std::string trimmed_numeric = normalized;
    while (!trimmed_numeric.empty() &&
           std::isdigit(static_cast<unsigned char>(trimmed_numeric.back()))) {
        trimmed_numeric.pop_back();
    }
    if (kAllowedEmotions.count(trimmed_numeric) != 0) {
        return trimmed_numeric;
    }

    static const std::unordered_map<std::string, std::string> kAliases = {
        {"amazed", "surprised"},
        {"attentive", "thinking"},
        {"anxiety", "confused"},
        {"boredom", "sleepy"},
        {"calm", "relaxed"},
        {"calming", "relaxed"},
        {"cheerful", "happy"},
        {"come", "happy"},
        {"confusion", "confused"},
        {"contempt", "angry"},
        {"curious", "thinking"},
        {"dance", "silly"},
        {"disgusted", "angry"},
        {"displeased", "sad"},
        {"downcast", "sad"},
        {"dying", "sleepy"},
        {"electric", "surprised"},
        {"embarrassing", "embarrassed"},
        {"enthusiastic", "happy"},
        {"excited", "happy"},
        {"exhausted", "sleepy"},
        {"fear", "shocked"},
        {"frustrated", "angry"},
        {"furious", "angry"},
        {"go_away", "angry"},
        {"grateful", "loving"},
        {"grinning", "laughing"},
        {"helpful", "confident"},
        {"impatient", "angry"},
        {"incomprehensible", "confused"},
        {"indifferent", "neutral"},
        {"inquiring", "thinking"},
        {"irritated", "angry"},
        {"joy", "happy"},
        {"kiss", "kissy"},
        {"laughing", "laughing"},
        {"lonely", "sad"},
        {"lost", "confused"},
        {"love", "loving"},
        {"no", "sad"},
        {"no_excited", "sad"},
        {"no_sad", "crying"},
        {"oops", "embarrassed"},
        {"playful", "silly"},
        {"proud", "confident"},
        {"rage", "angry"},
        {"relief", "relaxed"},
        {"reprimand", "angry"},
        {"resigned", "sad"},
        {"sadness", "sad"},
        {"scared", "shocked"},
        {"serenity", "relaxed"},
        {"shy", "embarrassed"},
        {"sleep", "sleepy"},
        {"sleeping", "sleepy"},
        {"smart", "confident"},
        {"success", "confident"},
        {"surprise", "surprised"},
        {"surprise_open", "shocked"},
        {"thoughtful", "thinking"},
        {"tired", "sleepy"},
        {"uncertain", "confused"},
        {"uncomfortable", "embarrassed"},
        {"understanding", "relaxed"},
        {"welcoming", "happy"},
        {"think", "thinking"},
        {"wink", "winking"},
        {"wow", "surprised"},
        {"yes", "happy"},
        {"yes_sad", "sad"}
    };
    auto it = kAliases.find(normalized);
    if (it != kAliases.end()) {
        return it->second;
    }
    it = kAliases.find(trimmed_numeric);
    if (it != kAliases.end()) {
        return it->second;
    }
    return "";
}

std::string NormalizeActionName(const std::string& value) {
    std::string normalized;
    normalized.reserve(value.size());
    for (char ch : value) {
        unsigned char uch = static_cast<unsigned char>(ch);
        if (std::isalnum(uch)) {
            normalized.push_back(static_cast<char>(std::tolower(uch)));
        } else if (ch == '-' || ch == ' ' || ch == '.' || ch == '_') {
            normalized.push_back('_');
        }
    }
    return normalized;
}

std::string NormalizeWeatherToken(const std::string& value) {
    std::string normalized;
    normalized.reserve(value.size());
    bool last_was_separator = false;
    for (char ch : value) {
        unsigned char uch = static_cast<unsigned char>(ch);
        if (std::isalnum(uch)) {
            normalized.push_back(static_cast<char>(std::tolower(uch)));
            last_was_separator = false;
        } else if (ch == '-' || ch == ' ' || ch == '.' || ch == '_' || ch == '/') {
            if (!normalized.empty() && !last_was_separator) {
                normalized.push_back('-');
                last_was_separator = true;
            }
        }
    }
    if (!normalized.empty() && normalized.back() == '-') {
        normalized.pop_back();
    }
    return normalized;
}

std::string NormalizeWeatherCondition(const std::string& value) {
    std::string normalized = NormalizeWeatherToken(value);
    if (normalized.empty()) {
        return "";
    }

    static const std::unordered_set<std::string> kAllowedConditions = {
        "clear-day", "clear-night", "partly-cloudy-day", "partly-cloudy-night",
        "mostly-clear-day", "mostly-clear-night", "cloudy", "overcast",
        "drizzle", "rain", "thunderstorms", "snow", "sleet", "fog",
        "haze", "mist", "wind"
    };
    if (kAllowedConditions.count(normalized) != 0) {
        return normalized;
    }

    static const std::unordered_map<std::string, std::string> kAliases = {
        {"clear", "clear-day"},
        {"sunny", "clear-day"},
        {"partly-cloudy", "partly-cloudy-day"},
        {"partlycloudy", "partly-cloudy-day"},
        {"mostly-clear", "mostly-clear-day"},
        {"mostlyclear", "mostly-clear-day"},
        {"clouds", "cloudy"},
        {"rainy", "rain"},
        {"thunderstorm", "thunderstorms"},
        {"storm", "thunderstorms"},
        {"snowy", "snow"},
        {"foggy", "fog"},
        {"hazy", "haze"},
        {"misty", "mist"},
        {"windy", "wind"}
    };
    auto it = kAliases.find(normalized);
    return it != kAliases.end() ? it->second : "";
}

std::string HumanizeWeatherText(const std::string& value) {
    std::string humanized = Trim(value);
    for (char& ch : humanized) {
        if (ch == '-' || ch == '_') {
            ch = ' ';
        }
    }
    return humanized;
}

bool IsNumericLikeText(const std::string& value) {
    std::string trimmed = Trim(value);
    if (trimmed.empty()) {
        return false;
    }
    bool has_digit = false;
    for (char ch : trimmed) {
        unsigned char uch = static_cast<unsigned char>(ch);
        if (std::isdigit(uch)) {
            has_digit = true;
            continue;
        }
        if (ch == '+' || ch == '-' || ch == '.' || std::isspace(uch)) {
            continue;
        }
        return false;
    }
    return has_digit;
}

const char* CelsiusSuffix()
{
    return "\xC2\xB0""C";
}

std::string NormalizeTemperatureText(const std::string& value)
{
    std::string text = Trim(value);
    if (text.empty()) {
        return "";
    }
    if (IsNumericLikeText(text)) {
        return text + CelsiusSuffix();
    }
    if (text.size() > 1 && (text.back() == 'C' || text.back() == 'c')) {
        std::string number = Trim(text.substr(0, text.size() - 1));
        if (IsNumericLikeText(number)) {
            return number + CelsiusSuffix();
        }
    }
    return text;
}

int GetJsonInt(cJSON* object, const char* key, int default_value) {
    if (object == nullptr || key == nullptr) {
        return default_value;
    }
    cJSON* item = cJSON_GetObjectItemCaseSensitive(object, key);
    if (cJSON_IsNumber(item)) {
        return static_cast<int>(item->valuedouble);
    }
    if (cJSON_IsString(item) && item->valuestring != nullptr) {
        char* end = nullptr;
        long value = std::strtol(item->valuestring, &end, 10);
        if (end != item->valuestring) {
            return static_cast<int>(value);
        }
    }
    return default_value;
}

std::string GetJsonTemperatureText(cJSON* object, const char* key) {
    if (object == nullptr || key == nullptr) {
        return "";
    }
    cJSON* item = cJSON_GetObjectItemCaseSensitive(object, key);
    if (item == nullptr) {
        return "";
    }

    if (cJSON_IsString(item) && item->valuestring != nullptr) {
        std::string text = Trim(item->valuestring);
        if (text.empty()) {
            return "";
        }
        return NormalizeTemperatureText(text);
    }

    if (cJSON_IsNumber(item)) {
        char buffer[32];
        double rounded = std::round(item->valuedouble);
        if (std::fabs(item->valuedouble - rounded) < 0.05) {
            std::snprintf(buffer, sizeof(buffer), "%d%s", static_cast<int>(rounded), CelsiusSuffix());
        } else {
            std::snprintf(buffer, sizeof(buffer), "%.1f%s", item->valuedouble, CelsiusSuffix());
        }
        return buffer;
    }

    return "";
}

std::string BuildWeatherDisplayText(const WeatherDisplayAction& action) {
    std::string text;
    auto append_part = [&text](const std::string& value) {
        if (value.empty()) {
            return;
        }
        if (!text.empty()) {
            text.push_back(' ');
        }
        text += value;
    };
    append_part(action.city);
    append_part(action.weather_text);
    append_part(action.temp_text);
    return text;
}

std::string GetActionStringParam(cJSON* action_node, cJSON* parameters, const char* key) {
    std::string value = GetJsonString(parameters, key);
    if (value.empty()) {
        value = GetJsonString(action_node, key);
    }
    return value;
}

int GetActionIntParam(cJSON* action_node, cJSON* parameters, const char* key, int default_value) {
    if (parameters != nullptr && cJSON_GetObjectItemCaseSensitive(parameters, key) != nullptr) {
        return GetJsonInt(parameters, key, default_value);
    }
    return GetJsonInt(action_node, key, default_value);
}

std::string GetActionTemperatureText(cJSON* action_node, cJSON* parameters, const char* key) {
    std::string value = GetJsonTemperatureText(parameters, key);
    if (value.empty()) {
        value = GetJsonTemperatureText(action_node, key);
    }
    return value;
}

cJSON* GetActionParameters(cJSON* action_node) {
    cJSON* parameters = cJSON_GetObjectItemCaseSensitive(action_node, "parameters");
    return cJSON_IsObject(parameters) ? parameters : nullptr;
}

bool PopulateWeatherDisplayAction(cJSON* action_node, WeatherDisplayAction* action) {
    if (action_node == nullptr || action == nullptr) {
        return false;
    }

    cJSON* parameters = GetActionParameters(action_node);
    std::string raw_weather = Trim(GetActionStringParam(action_node, parameters, "weather"));
    if (raw_weather.empty()) {
        raw_weather = Trim(GetActionStringParam(action_node, parameters, "weather_type"));
    }
    if (raw_weather.empty()) {
        raw_weather = Trim(GetActionStringParam(action_node, parameters, "condition"));
    }

    action->condition_id = NormalizeWeatherCondition(raw_weather);
    action->weather_text = HumanizeWeatherText(
        action->condition_id.empty() ? raw_weather : action->condition_id);
    action->city = Trim(GetActionStringParam(action_node, parameters, "city"));
    action->temp_text = GetActionTemperatureText(action_node, parameters, "temp");
    action->duration_ms = GetActionIntParam(action_node, parameters, "duration_ms",
        kDefaultWeatherDisplayDurationMs);
    if (action->duration_ms <= 0) {
        action->duration_ms = kDefaultWeatherDisplayDurationMs;
    }
    return !BuildWeatherDisplayText(*action).empty();
}

void PopulateEmotionAction(cJSON* action_node, const std::string& action_type,
                           DatastreamDisplayActions* actions) {
    if (action_node == nullptr || actions == nullptr || actions->has_emotion_action) {
        return;
    }

    cJSON* parameters = GetActionParameters(action_node);
    std::string emotion_value = GetActionStringParam(action_node, parameters, "emotion_type");
    if (emotion_value.empty()) {
        emotion_value = GetActionStringParam(action_node, parameters, "emotion");
    }
    if (emotion_value.empty()) {
        emotion_value = GetActionStringParam(action_node, parameters, "mood");
    }

    actions->has_emotion_action = true;
    actions->emotion_action_type = action_type;
    actions->emotion = NormalizeEmotionName(emotion_value);
    if (action_type == "stop_emotion") {
        actions->emotion = "neutral";
    }
}

void HandleDatastreamActionObject(cJSON* node, DatastreamDisplayActions* actions) {
    if (!cJSON_IsObject(node) || actions == nullptr) {
        return;
    }

    std::string action_type = NormalizeActionName(GetJsonString(node, "executor"));
    if (action_type.empty()) {
        action_type = NormalizeActionName(GetJsonString(node, "action_type"));
    }
    if (action_type.empty()) {
        action_type = NormalizeActionName(GetJsonString(node, "action"));
    }
    if (action_type.empty()) {
        return;
    }

    actions->saw_action = true;
    if (actions->first_action_type.empty()) {
        actions->first_action_type = action_type;
    }

    if (action_type == "display_weather") {
        actions->has_weather_action = true;
        if (!actions->has_weather) {
            actions->has_weather = PopulateWeatherDisplayAction(node, &actions->weather);
        }
        return;
    }

    if (action_type == "display_emotion" || action_type == "play_emotion" ||
        action_type == "emotion" || action_type == "display_emoji" ||
        action_type == "stop_emotion") {
        PopulateEmotionAction(node, action_type, actions);
    }
}

bool ExtractDatastreamDisplayActionsFromJsonNode(cJSON* node, DatastreamDisplayActions* actions) {
    if (node == nullptr || actions == nullptr) {
        return false;
    }

    if (cJSON_IsString(node) && node->valuestring != nullptr) {
        std::string text = Trim(node->valuestring);
        if (text.empty() || (text.front() != '{' && text.front() != '[')) {
            return false;
        }
        cJSON* nested = cJSON_ParseWithLength(text.c_str(), text.size());
        if (nested == nullptr) {
            return false;
        }
        bool found = ExtractDatastreamDisplayActionsFromJsonNode(nested, actions);
        cJSON_Delete(nested);
        return found;
    }

    bool found = false;
    if (cJSON_IsObject(node)) {
        HandleDatastreamActionObject(node, actions);
        found = actions->saw_action;

        static constexpr const char* kPreferredKeys[] = {"actions", "content", "payload", "message", "data"};
        for (const char* key : kPreferredKeys) {
            cJSON* child = cJSON_GetObjectItemCaseSensitive(node, key);
            if (ExtractDatastreamDisplayActionsFromJsonNode(child, actions)) {
                found = true;
            }
        }

        cJSON* child = nullptr;
        cJSON_ArrayForEach(child, node) {
            if (ExtractDatastreamDisplayActionsFromJsonNode(child, actions)) {
                found = true;
            }
        }
        return found;
    }

    if (cJSON_IsArray(node)) {
        cJSON* child = nullptr;
        cJSON_ArrayForEach(child, node) {
            if (ExtractDatastreamDisplayActionsFromJsonNode(child, actions)) {
                found = true;
            }
        }
        return found;
    }

    return found;
}

bool ParseDatastreamDisplayActions(const std::string& payload, DatastreamDisplayActions* actions) {
    if (actions == nullptr) {
        return false;
    }
    *actions = {};

    std::string text = Trim(payload);
    if (text.empty()) {
        return false;
    }

    cJSON* root = cJSON_ParseWithLength(text.c_str(), text.size());
    if (root == nullptr) {
        return false;
    }
    bool found = ExtractDatastreamDisplayActionsFromJsonNode(root, actions);
    cJSON_Delete(root);
    return found;
}

std::string ToLowerAscii(const std::string& value) {
    std::string lowered = value;
    for (char& ch : lowered) {
        unsigned char uch = static_cast<unsigned char>(ch);
        if (std::isalpha(uch)) {
            ch = static_cast<char>(std::tolower(uch));
        }
    }
    return lowered;
}

bool ContainsAnyKeyword(const std::string& text, std::initializer_list<const char*> keywords) {
    for (const char* keyword : keywords) {
        if (keyword != nullptr && keyword[0] != '\0' && text.find(keyword) != std::string::npos) {
            return true;
        }
    }
    return false;
}

bool IsCompactKeywordCandidate(const std::string& text) {
    if (text.empty() || text.size() > 32) {
        return false;
    }

    int whitespace_count = 0;
    for (char ch : text) {
        if (std::isspace(static_cast<unsigned char>(ch))) {
            ++whitespace_count;
            if (whitespace_count >= 2) {
                return false;
            }
        }
    }
    return true;
}

bool ParseDatastreamEmotionKeyword(const std::string& payload, std::string* action_type, std::string* emotion) {
    if (action_type == nullptr || emotion == nullptr) {
        return false;
    }
    action_type->clear();
    emotion->clear();

    std::string text = Trim(payload);
    if (text.empty()) {
        return false;
    }

    std::string normalized_action = NormalizeActionName(text);
    if (normalized_action == "thinking") {
        *action_type = "display_emotion";
        *emotion = "thinking";
        return true;
    }
    if (normalized_action == "silent" || normalized_action == "idle" || normalized_action == "listening" ||
        normalized_action == "speaking" || normalized_action == "standby" || normalized_action == "neutral") {
        *action_type = "stop_emotion";
        *emotion = "neutral";
        return true;
    }

    std::string exact_emotion = NormalizeEmotionName(text);
    if (!exact_emotion.empty()) {
        *action_type = "display_emotion";
        *emotion = exact_emotion;
        return true;
    }

    std::string lowered = ToLowerAscii(text);
    bool has_directive_markers = ContainsAnyKeyword(lowered, {
        "emotion", "emoji", "mood", "state", "action", "表情", "情绪", "状态", "动作"
    });
    bool looks_like_json = text.find('{') != std::string::npos || text.find('[') != std::string::npos;
    if ((!IsCompactKeywordCandidate(text) && !has_directive_markers) ||
        (looks_like_json && !has_directive_markers)) {
        return false;
    }

    if (ContainsAnyKeyword(lowered, {
            "stop_emotion", "clear_emotion", "clear emotion", "stop emotion", "emotion off",
            "emotion reset", "reset emotion", "stop emoji", "停止表情", "关闭表情", "清除表情", "恢复默认",
            "\"state\":\"silent\"", "\"state\":\"idle\"", "\"state\":\"listening\"", "\"state\":\"speaking\""
        })) {
        *action_type = "stop_emotion";
        *emotion = "neutral";
        return true;
    }

    if (ContainsAnyKeyword(lowered, {"thinking", "think", "ponder", "curious", "思考", "思索", "沉思", "想一想"})) {
        *action_type = "display_emotion";
        *emotion = "thinking";
    } else if (ContainsAnyKeyword(lowered, {"laughing", "laugh", "哈哈", "大笑", "笑出声"})) {
        *action_type = "display_emotion";
        *emotion = "laughing";
    } else if (ContainsAnyKeyword(lowered, {"funny", "搞笑", "有趣", "逗"})) {
        *action_type = "display_emotion";
        *emotion = "funny";
    } else if (ContainsAnyKeyword(lowered, {"happy", "joy", "开心", "高兴", "愉快", "快乐", "兴奋"})) {
        *action_type = "display_emotion";
        *emotion = "happy";
    } else if (ContainsAnyKeyword(lowered, {"sad", "sadness", "伤心", "难过", "沮丧"})) {
        *action_type = "display_emotion";
        *emotion = "sad";
    } else if (ContainsAnyKeyword(lowered, {"crying", "cry", "哭泣", "流泪", "掉眼泪"})) {
        *action_type = "display_emotion";
        *emotion = "crying";
    } else if (ContainsAnyKeyword(lowered, {"angry", "anger", "生气", "愤怒", "恼火"})) {
        *action_type = "display_emotion";
        *emotion = "angry";
    } else if (ContainsAnyKeyword(lowered, {"loving", "love", "喜欢", "温柔", "爱心"})) {
        *action_type = "display_emotion";
        *emotion = "loving";
    } else if (ContainsAnyKeyword(lowered, {"embarrassed", "shy", "尴尬", "害羞", "不好意思"})) {
        *action_type = "display_emotion";
        *emotion = "embarrassed";
    } else if (ContainsAnyKeyword(lowered, {"shocked", "shock", "震惊", "惊吓", "吓到"})) {
        *action_type = "display_emotion";
        *emotion = "shocked";
    } else if (ContainsAnyKeyword(lowered, {"surprised", "surprise", "wow", "惊讶", "惊喜", "哇哦"})) {
        *action_type = "display_emotion";
        *emotion = "surprised";
    } else if (ContainsAnyKeyword(lowered, {"winking", "wink", "眨眼"})) {
        *action_type = "display_emotion";
        *emotion = "winking";
    } else if (ContainsAnyKeyword(lowered, {"cool", "酷", "帅气"})) {
        *action_type = "display_emotion";
        *emotion = "cool";
    } else if (ContainsAnyKeyword(lowered, {"relaxed", "relax", "calm", "放松", "平静", "轻松"})) {
        *action_type = "display_emotion";
        *emotion = "relaxed";
    } else if (ContainsAnyKeyword(lowered, {"delicious", "tasty", "yum", "好吃", "美味"})) {
        *action_type = "display_emotion";
        *emotion = "delicious";
    } else if (ContainsAnyKeyword(lowered, {"kissy", "kiss", "亲亲", "飞吻"})) {
        *action_type = "display_emotion";
        *emotion = "kissy";
    } else if (ContainsAnyKeyword(lowered, {"confident", "proud", "自信", "得意"})) {
        *action_type = "display_emotion";
        *emotion = "confident";
    } else if (ContainsAnyKeyword(lowered, {"sleepy", "sleep", "tired", "困", "疲惫", "想睡"})) {
        *action_type = "display_emotion";
        *emotion = "sleepy";
    } else if (ContainsAnyKeyword(lowered, {"silly", "playful", "调皮", "搞怪"})) {
        *action_type = "display_emotion";
        *emotion = "silly";
    } else if (ContainsAnyKeyword(lowered, {"confused", "confusion", "疑惑", "困惑", "迷糊"})) {
        *action_type = "display_emotion";
        *emotion = "confused";
    } else {
        return false;
    }

    return true;
}

}  // namespace

Application::Application() {
    event_group_ = xEventGroupCreate();

#if CONFIG_USE_DEVICE_AEC && CONFIG_USE_SERVER_AEC
#error "CONFIG_USE_DEVICE_AEC and CONFIG_USE_SERVER_AEC cannot be enabled at the same time"
#elif CONFIG_USE_DEVICE_AEC || CONFIG_BOARD_TYPE_SEEED_STUDIO_SENSECAP_WATCHER
    aec_mode_ = kAecOnDeviceSide;
#elif CONFIG_USE_SERVER_AEC
    aec_mode_ = kAecOnServerSide;
#else
    aec_mode_ = kAecOff;
#endif

    esp_timer_create_args_t clock_timer_args = {
        .callback = [](void* arg) {
            auto* app = static_cast<Application*>(arg);
            xEventGroupSetBits(app->event_group_, MAIN_EVENT_CLOCK_TICK);
        },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "clock_timer",
        .skip_unhandled_events = true,
    };
    esp_timer_create(&clock_timer_args, &clock_timer_handle_);
}

Application::~Application() {
    StopAgoraSessionInternal();
    audio_service_.Stop();
    if (clock_timer_handle_ != nullptr) {
        esp_timer_stop(clock_timer_handle_);
        esp_timer_delete(clock_timer_handle_);
    }
    if (event_group_ != nullptr) {
        vEventGroupDelete(event_group_);
    }
}

bool Application::SetDeviceState(DeviceState state) {
    return state_machine_.TransitionTo(state);
}

void Application::Initialize() {
    auto& board = Board::GetInstance();
    audio_codec_ = board.GetAudioCodec();
    SetDeviceState(kDeviceStateStarting);

    auto display = board.GetDisplay();
    display->SetupUI();
    display->SetChatMessage("system", SystemInfo::GetUserAgent().c_str());

    audio_service_.Initialize(audio_codec_);
    auto& assets = Assets::GetInstance();
    if (!assets.Apply(false)) {
        ESP_LOGI(TAG, "Assets apply skipped or failed; speech models may be unavailable");
    }
    audio_service_.Start();

    AudioServiceCallbacks callbacks;
    callbacks.on_wake_word_detected = [this](const std::string& wake_word) {
        Schedule([this, wake_word]() {
            WakeWordInvoke(wake_word);
        });
    };
    callbacks.on_vad_change = [this](bool) {
        xEventGroupSetBits(event_group_, MAIN_EVENT_VAD_CHANGE);
    };
    audio_service_.SetCallbacks(callbacks);

    state_machine_.AddStateChangeListener([this](DeviceState, DeviceState) {
        xEventGroupSetBits(event_group_, MAIN_EVENT_STATE_CHANGED);
    });

    esp_timer_start_periodic(clock_timer_handle_, 1000000);

    board.SetNetworkEventCallback([this](NetworkEvent event, const std::string& data) {
        auto display = Board::GetInstance().GetDisplay();
        switch (event) {
            case NetworkEvent::Scanning:
                display->ShowNotification(Lang::Strings::SCANNING_WIFI, 30000);
                break;
            case NetworkEvent::Connecting:
                if (!data.empty()) {
                    std::string msg = Lang::Strings::CONNECT_TO;
                    msg += data;
                    msg += "...";
                    display->ShowNotification(msg.c_str(), 30000);
                } else {
                    display->SetStatus(Lang::Strings::REGISTERING_NETWORK);
                }
                break;
            case NetworkEvent::Connected: {
                std::string msg = Lang::Strings::CONNECTED_TO;
                msg += data;
                display->ShowNotification(msg.c_str(), 30000);
                xEventGroupSetBits(event_group_, MAIN_EVENT_NETWORK_CONNECTED);
                break;
            }
            case NetworkEvent::Disconnected:
                xEventGroupSetBits(event_group_, MAIN_EVENT_NETWORK_DISCONNECTED);
                break;
            case NetworkEvent::WifiConfigModeEnter:
                ShowWifiConfigPrompt();
                break;
            case NetworkEvent::WifiConfigModeExit:
                break;
            case NetworkEvent::ModemDetecting:
                display->SetStatus(Lang::Strings::DETECTING_MODULE);
                break;
            case NetworkEvent::ModemErrorNoSim:
                Alert(Lang::Strings::ERROR, Lang::Strings::PIN_ERROR, "triangle_exclamation", Lang::Sounds::OGG_ERR_PIN);
                break;
            case NetworkEvent::ModemErrorRegDenied:
                Alert(Lang::Strings::ERROR, Lang::Strings::REG_ERROR, "triangle_exclamation", Lang::Sounds::OGG_ERR_REG);
                break;
            case NetworkEvent::ModemErrorInitFailed:
                Alert(Lang::Strings::ERROR, Lang::Strings::MODEM_INIT_ERROR, "triangle_exclamation", Lang::Sounds::OGG_EXCLAMATION);
                break;
            case NetworkEvent::ModemErrorTimeout:
                display->SetStatus(Lang::Strings::REGISTERING_NETWORK);
                break;
        }
    });

    board.StartNetwork();
    display->UpdateStatusBar(true);
}

void Application::Run() {
    vTaskPrioritySet(nullptr, 10);

    const EventBits_t all_events =
        MAIN_EVENT_SCHEDULE |
        MAIN_EVENT_VAD_CHANGE |
        MAIN_EVENT_ERROR |
        MAIN_EVENT_CLOCK_TICK |
        MAIN_EVENT_NETWORK_CONNECTED |
        MAIN_EVENT_NETWORK_DISCONNECTED |
        MAIN_EVENT_TOGGLE_CHAT |
        MAIN_EVENT_START_LISTENING |
        MAIN_EVENT_STOP_LISTENING |
        MAIN_EVENT_STATE_CHANGED;

    while (true) {
        EventBits_t bits = xEventGroupWaitBits(event_group_, all_events, pdTRUE, pdFALSE, portMAX_DELAY);

        if (bits & MAIN_EVENT_ERROR) {
            Alert(Lang::Strings::ERROR, last_error_message_.c_str(), "sad", Lang::Sounds::OGG_EXCLAMATION);
        }
        if (bits & MAIN_EVENT_NETWORK_CONNECTED) {
            HandleNetworkConnectedEvent();
        }
        if (bits & MAIN_EVENT_NETWORK_DISCONNECTED) {
            HandleNetworkDisconnectedEvent();
        }
        if (bits & MAIN_EVENT_STATE_CHANGED) {
            HandleStateChangedEvent();
        }
        if (bits & MAIN_EVENT_TOGGLE_CHAT) {
            HandleToggleChatEvent();
        }
        if (bits & MAIN_EVENT_START_LISTENING) {
            HandleStartListeningEvent();
        }
        if (bits & MAIN_EVENT_STOP_LISTENING) {
            HandleStopListeningEvent();
        }
        if (bits & MAIN_EVENT_VAD_CHANGE) {
            Board::GetInstance().GetLed()->OnStateChanged();
        }
        if (bits & MAIN_EVENT_SCHEDULE) {
            std::unique_lock<std::mutex> lock(mutex_);
            auto tasks = std::move(main_tasks_);
            lock.unlock();
            for (auto& task : tasks) {
                task();
            }
        }
        if (bits & MAIN_EVENT_CLOCK_TICK) {
            ++clock_ticks_;
            Board::GetInstance().GetDisplay()->UpdateStatusBar();
        }
    }
}

void Application::HandleNetworkConnectedEvent() {
    network_connected_ = true;
    if (GetDeviceState() == kDeviceStateStarting || GetDeviceState() == kDeviceStateWifiConfiguring) {
        SetDeviceState(kDeviceStateIdle);
    }
    Board::GetInstance().GetDisplay()->UpdateStatusBar(true);
}

void Application::HandleNetworkDisconnectedEvent() {
    network_connected_ = false;
    StopAgoraSessionInternal();
    if (GetDeviceState() != kDeviceStateWifiConfiguring) {
        SetDeviceState(kDeviceStateStarting);
    }
    Board::GetInstance().GetDisplay()->UpdateStatusBar(true);
}

void Application::HandleStateChangedEvent() {
    clock_ticks_ = 0;

    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();
    board.GetLed()->OnStateChanged();

    switch (GetDeviceState()) {
        case kDeviceStateStarting:
            display->SetStatus(SystemInfo::GetUserAgent().c_str());
            display->SetEmotion("neutral");
            break;
        case kDeviceStateIdle:
            display->SetStatus(Lang::Strings::STANDBY);
            display->SetChatMessage("system", "");
            display->SetChatMessage("subtitle", "");
            display->SetEmotion("neutral");
            break;
        case kDeviceStateConnecting:
            display->SetStatus(Lang::Strings::CONNECTING);
            display->SetChatMessage("system", "");
            display->SetChatMessage("subtitle", "Starting conversation");
            display->SetEmotion("neutral");
            break;
        case kDeviceStateListening:
            display->SetStatus(Lang::Strings::LISTENING);
            display->SetChatMessage("system", "");
            display->SetEmotion("neutral");
            break;
        case kDeviceStateSpeaking:
            display->SetStatus(Lang::Strings::LISTENING);
            display->SetChatMessage("system", "");
            display->SetChatMessage("subtitle", "Stopping conversation");
            display->SetEmotion("neutral");
            break;
        case kDeviceStateWifiConfiguring:
            display->SetStatus(Lang::Strings::WIFI_CONFIG_MODE);
            display->SetChatMessage("system", "");
            ShowWifiConfigPrompt();
            display->SetEmotion("neutral");
            break;
        case kDeviceStateFatalError:
            display->SetStatus(Lang::Strings::ERROR);
            display->SetEmotion("sad");
            break;
        default:
            break;
    }

    audio_service_.EnableWakeWordDetection(
        GetDeviceState() == kDeviceStateIdle && !session_running_ && !session_command_pending_);
}

void Application::ShowWifiConfigPrompt() {
    if (GetDeviceState() != kDeviceStateWifiConfiguring) {
        return;
    }

    auto& wifi_manager = WifiManager::GetInstance();
    std::string hint = Lang::Strings::CONNECT_TO_HOTSPOT;
    hint += wifi_manager.GetApSsid();
    hint += Lang::Strings::ACCESS_VIA_BROWSER;
    hint += wifi_manager.GetApWebUrl();

    auto display = Board::GetInstance().GetDisplay();
    display->SetChatMessage("subtitle", hint.c_str());
    display->ShowNotification(hint.c_str(), 30000);
}

void Application::Schedule(std::function<void()>&& callback) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        main_tasks_.push_back(std::move(callback));
    }
    xEventGroupSetBits(event_group_, MAIN_EVENT_SCHEDULE);
}

void Application::Alert(const char* status, const char* message, const char* emotion, const std::string_view& sound) {
    std::string status_copy = status ? status : "";
    std::string message_copy = message ? message : "";
    std::string emotion_copy = emotion ? emotion : "";
    std::string sound_copy(sound);

    Schedule([this, status_copy, message_copy, emotion_copy, sound_copy]() {
        auto display = Board::GetInstance().GetDisplay();
        if (!status_copy.empty()) {
            display->SetStatus(status_copy.c_str());
        }
        if (!emotion_copy.empty()) {
            display->SetEmotion(emotion_copy.c_str());
        }
        if (!message_copy.empty()) {
            display->ShowNotification(message_copy, 30000);
        }
        if (!sound_copy.empty()) {
            audio_service_.PlaySound(sound_copy);
        }
    });
}

void Application::DismissAlert() {
    Schedule([this]() {
        HandleStateChangedEvent();
    });
}

void Application::AbortSpeaking(AbortReason reason) {
    (void)reason;
    StopListening();
}

void Application::HandleToggleChatEvent() {
    bool session_active = session_running_ || agora_rtc_session_.is_started() || agora_rtc_session_.is_in_channel();
    ESP_LOGI(TAG,
        "HandleToggleChatEvent: state=%d session_running=%d rtc_started=%d rtc_in_channel=%d pending=%d stop_requested=%d",
        static_cast<int>(GetDeviceState()), session_running_.load(), agora_rtc_session_.is_started(),
        agora_rtc_session_.is_in_channel(), session_command_pending_.load(), stop_requested_.load());
    if (session_active || stop_requested_) {
        ESP_LOGI(TAG, "HandleToggleChatEvent: route to stop");
        StopSessionAsync();
        return;
    }
    if (session_command_pending_) {
        ESP_LOGI(TAG, "HandleToggleChatEvent: start pending, remember stop request");
        stop_requested_ = true;
        return;
    }
    if (GetDeviceState() == kDeviceStateIdle) {
        ESP_LOGI(TAG, "HandleToggleChatEvent: route to start");
        StartSessionAsync();
    }
}

void Application::HandleStartListeningEvent() {
    bool session_active = session_running_ || agora_rtc_session_.is_started() || agora_rtc_session_.is_in_channel();
    if (!session_active && !session_command_pending_ && GetDeviceState() == kDeviceStateIdle) {
        StartSessionAsync();
    }
}

void Application::HandleStopListeningEvent() {
    bool session_active = session_running_ || agora_rtc_session_.is_started() || agora_rtc_session_.is_in_channel();
    if (session_active || session_command_pending_ || stop_requested_) {
        StopSessionAsync();
    }
}

void Application::ToggleChatState() {
    ESP_LOGI(TAG, "ToggleChatState requested");
    xEventGroupSetBits(event_group_, MAIN_EVENT_TOGGLE_CHAT);
}

void Application::StartListening() {
    xEventGroupSetBits(event_group_, MAIN_EVENT_START_LISTENING);
}

void Application::StopListening() {
    xEventGroupSetBits(event_group_, MAIN_EVENT_STOP_LISTENING);
}

void Application::Reboot() {
    StopAgoraSessionInternal();
    audio_service_.Stop();
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
}

void Application::WakeWordInvoke(const std::string& wake_word) {
    Schedule([wake_word]() {
        Board::GetInstance().GetDisplay()->SetChatMessage("user", wake_word.c_str());
    });
    StartListening();
}

bool Application::CanEnterSleepMode() {
    return GetDeviceState() == kDeviceStateIdle && !session_command_pending_ && audio_service_.IsIdle();
}

void Application::SetAecMode(AecMode mode) {
    aec_mode_ = mode;
}

void Application::PlaySound(const std::string_view& sound) {
    audio_service_.PlaySound(sound);
}

void Application::ResetConversationSession() {
    Schedule([this]() {
        StopAgoraSessionInternal();
        if (GetDeviceState() != kDeviceStateWifiConfiguring) {
            SetDeviceState(network_connected_ ? kDeviceStateIdle : kDeviceStateStarting);
        }
    });
}

void Application::SetError(const std::string& error) {
    last_error_message_ = error;
    ESP_LOGE(TAG, "%s", error.c_str());
    xEventGroupSetBits(event_group_, MAIN_EVENT_ERROR);
}

void Application::StartSessionAsync() {
    bool already_pending = session_command_pending_.exchange(true);
    if (already_pending) {
        ESP_LOGI(TAG, "StartSessionAsync ignored because another session command is pending");
        return;
    }
    ESP_LOGI(TAG, "StartSessionAsync requested");
    stop_requested_ = false;

    SetDeviceState(kDeviceStateConnecting);

    auto camera = Board::GetInstance().GetCamera();
    if (camera != nullptr) {
        camera->SetSessionActive(true);
    }

    audio_service_.EnableWakeWordDetection(false);
    audio_service_.EnableDeviceAec(false);
    audio_service_.EnableVoiceProcessing(false);
    if (audio_codec_ != nullptr) {
        audio_codec_->EnableSoftwareInputReference(false);
        if (audio_codec_->input_enabled()) {
            audio_codec_->EnableInput(false);
        }
        if (audio_codec_->output_enabled()) {
            audio_codec_->EnableOutput(false);
        }
    }
    vTaskDelay(pdMS_TO_TICKS(20));

    auto* task_args = new SessionTaskArgs{this};
    ESP_LOGI(TAG, "Creating dynamic agora_agent_start task: free internal=%u free psram=%u stack_bytes=%u",
        static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
        static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)),
        static_cast<unsigned>(kAgoraAgentTaskStackBytes));
    TaskHandle_t task = nullptr;
    BaseType_t create_ok = xTaskCreatePinnedToCore([](void* arg) {
        auto* task_args = static_cast<SessionTaskArgs*>(arg);
        ESP_LOGI(TAG, "agora_agent_start begin, stack_hw=%u",
            static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
        bool ok = task_args->app->StartAgoraAgentInternal();
        task_args->app->Schedule([app = task_args->app, ok]() {
            if (!ok) {
                app->StopAgoraSessionInternal();
                app->session_command_pending_ = false;
                app->stop_requested_ = false;
                app->SetDeviceState(app->network_connected_ ? kDeviceStateIdle : kDeviceStateStarting);
                return;
            }
            app->StartAgoraRtcTask();
        });
        delete task_args;
        vTaskDelete(nullptr);
    }, "agora_agent_start", kAgoraAgentTaskStackBytes, task_args, 3, &task, 1);

    if (create_ok != pdPASS) {
        delete task_args;
        session_command_pending_ = false;
        stop_requested_ = false;
        SetDeviceState(network_connected_ ? kDeviceStateIdle : kDeviceStateStarting);
        SetError("Failed to create agora_agent_start task");
        return;
    }
    ESP_LOGI(TAG, "agora_agent_start task created");
}

void Application::StartAgoraRtcTask() {
    auto* task_args = new SessionTaskArgs{this};
    ESP_LOGI(TAG, "Creating dynamic agora_rtc_start task: free internal=%u free psram=%u stack_bytes=%u",
        static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
        static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)),
        static_cast<unsigned>(kAgoraRtcTaskStackBytes));
    TaskHandle_t task = nullptr;
    BaseType_t create_ok = xTaskCreatePinnedToCore([](void* arg) {
        auto* task_args = static_cast<SessionTaskArgs*>(arg);
        ESP_LOGI(TAG, "agora_rtc_start begin, stack_hw=%u",
            static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
        bool ok = task_args->app->StartAgoraSessionInternal();
        task_args->app->Schedule([app = task_args->app, ok]() {
            app->session_command_pending_ = false;
            if (!ok) {
                app->StopAgoraSessionInternal();
                app->stop_requested_ = false;
                app->SetDeviceState(app->network_connected_ ? kDeviceStateIdle : kDeviceStateStarting);
                return;
            }
            app->session_running_ = true;
            if (app->stop_requested_.exchange(false)) {
                app->StopSessionAsync();
                return;
            }
            app->SetDeviceState(kDeviceStateListening);
        });
        delete task_args;
        vTaskDelete(nullptr);
    }, "agora_rtc_start", kAgoraRtcTaskStackBytes, task_args, 3, &task, 1);

    if (create_ok != pdPASS) {
        delete task_args;
        session_command_pending_ = false;
        stop_requested_ = false;
        SetDeviceState(network_connected_ ? kDeviceStateIdle : kDeviceStateStarting);
        SetError("Failed to create agora_rtc_start task");
        return;
    }
    ESP_LOGI(TAG, "agora_rtc_start task created");
}

bool Application::StartAgoraAgentInternal() {
    ESP_LOGI(TAG, "StartAgoraAgentInternal begin, stack_hw=%u",
        static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
    if (!network_connected_) {
        SetError("Network is not connected");
        return false;
    }

    last_voiceprint_sample_url_.clear();
    if (VoiceprintUploader::IsEnabled()) {
        last_voiceprint_sample_url_ = LoadVoiceprintSampleUrl();
        if (!last_voiceprint_sample_url_.empty()) {
            ESP_LOGI(TAG, "Use cached voiceprint sample URL: %s", last_voiceprint_sample_url_.c_str());
        } else {
            auto report_voiceprint_status = [this](const char* message) {
                std::string message_copy = message ? message : "";
                if (message_copy.empty()) {
                    return;
                }
                Schedule([message = std::move(message_copy)]() {
                    auto display = Board::GetInstance().GetDisplay();
                    if (display != nullptr) {
                        display->SetChatMessage("subtitle", message.c_str());
                        display->ShowNotification(message.c_str(), 3000);
                    }
                });
            };
            std::string sample_url;
            std::string voiceprint_error;
            if (VoiceprintUploader::CaptureAndUpload(audio_service_, audio_codec_, &sample_url,
                    &voiceprint_error, report_voiceprint_status)) {
                last_voiceprint_sample_url_ = sample_url;
                SaveVoiceprintSampleUrl(last_voiceprint_sample_url_);
                ESP_LOGI(TAG, "Voiceprint sample ready and saved: %s", last_voiceprint_sample_url_.c_str());
            } else {
                ESP_LOGW(TAG, "Voiceprint sample unavailable, continuing conversation start: %s",
                    voiceprint_error.empty() ? "unknown error" : voiceprint_error.c_str());
            }
        }
    }

    if (!agora_agent_manager_.Initialize()) {
        SetError(agora_agent_manager_.last_error());
        return false;
    }
    ESP_LOGI(TAG, "Conversation config initialized, stack_hw=%u",
        static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
    if (!agora_agent_manager_.Start()) {
        SetError(agora_agent_manager_.last_error());
        return false;
    }
    ESP_LOGI(TAG, "Conversation started, stack_hw=%u",
        static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
    return true;
}

void Application::StopSessionAsync() {
    bool session_active = session_running_ || agora_rtc_session_.is_started() || agora_rtc_session_.is_in_channel();
    ESP_LOGI(TAG,
        "StopSessionAsync requested: state=%d session_running=%d rtc_started=%d rtc_in_channel=%d pending=%d stop_requested=%d conversation_running=%d conversation_id=%s",
        static_cast<int>(GetDeviceState()), session_running_.load(), agora_rtc_session_.is_started(),
        agora_rtc_session_.is_in_channel(), session_command_pending_.load(), stop_requested_.load(),
        agora_agent_manager_.is_running(),
        agora_agent_manager_.conversation_id().empty() ? "<empty>" : agora_agent_manager_.conversation_id().c_str());
    stop_requested_ = true;
    bool already_pending = session_command_pending_.exchange(true);
    if (already_pending) {
        if (!session_active && !agora_agent_manager_.is_running()) {
            ESP_LOGW(TAG, "StopSessionAsync clearing stale pending command with no active session");
            session_command_pending_ = false;
            stop_requested_ = false;
            if (GetDeviceState() == kDeviceStateConnecting) {
                SetDeviceState(network_connected_ ? kDeviceStateIdle : kDeviceStateStarting);
            }
            return;
        }
        ESP_LOGI(TAG, "StopSessionAsync deferred because another session command is pending");
        return;
    }
    if (!session_active) {
        ESP_LOGW(TAG, "StopSessionAsync proceeding even though session_active=false");
    }

    session_running_ = false;
    SetDeviceState(network_connected_ ? kDeviceStateIdle : kDeviceStateStarting);
    Board::GetInstance().GetDisplay()->SetChatMessage("subtitle", "");
    ESP_LOGI(TAG, "StopSessionAsync: stopping local conversation session inline");
    StopAgoraSessionInternal();
    session_command_pending_ = false;
    stop_requested_ = false;
    if (!network_connected_ && GetDeviceState() != kDeviceStateWifiConfiguring) {
        SetDeviceState(kDeviceStateStarting);
    }
    ESP_LOGI(TAG, "StopSessionAsync completed");
}

bool Application::StartAgoraSessionInternal() {
    ESP_LOGI(TAG, "StartAgoraSessionInternal begin");
    if (!network_connected_) {
        SetError("Network is not connected");
        return false;
    }

    // Release wake-word/audio-path resources before initializing Agora RTC.
    audio_service_.EnableWakeWordDetection(false);
    audio_service_.EnableDeviceAec(false);
    audio_service_.EnableVoiceProcessing(false);
    if (audio_codec_ != nullptr) {
        audio_codec_->EnableSoftwareInputReference(false);
        if (audio_codec_->input_enabled()) {
            audio_codec_->EnableInput(false);
        }
        if (audio_codec_->output_enabled()) {
            audio_codec_->EnableOutput(false);
        }
    }
    vTaskDelay(pdMS_TO_TICKS(50));
    ESP_LOGI(TAG, "Before Agora RTC init: input_enabled=%d output_enabled=%d",
        audio_codec_ != nullptr ? audio_codec_->input_enabled() : 0,
        audio_codec_ != nullptr ? audio_codec_->output_enabled() : 0);
    SystemInfo::PrintHeapStats();

    auto config = agora_agent_manager_.config();
    ESP_LOGI(TAG, "Conversation session info: channel=%s conversation_id=%s join_uid=%u agent_uid_hint=%u",
        config.channel_name.c_str(),
        agora_agent_manager_.conversation_id().empty() ? "<empty>" : agora_agent_manager_.conversation_id().c_str(),
        static_cast<unsigned>(config.remote_rtc_uid),
        static_cast<unsigned>(config.agent_rtc_uid));
    bool use_device_aec = false;
    if (aec_mode_ == kAecOnDeviceSide && audio_codec_ != nullptr && audio_codec_->supports_device_aec()) {
        if (!audio_codec_->has_hardware_input_reference()) {
            audio_codec_->EnableSoftwareInputReference(true);
        }
        use_device_aec = audio_codec_->has_aec_reference();
        if (use_device_aec) {
            ESP_LOGI(TAG, "Device-side AEC reference selected: %s",
                audio_codec_->using_software_input_reference() ? "software speaker tap" : "hardware input reference");
        }
    }
    agora_rtc_session_.SetIncomingAudioCallback([this](const void* data, size_t len) {
        agora_audio_bridge_.PushIncomingPcm(data, len);
    });
    agora_rtc_session_.SetStreamMessageCallback([this](uint32_t uid, const std::string& payload) {
        ESP_LOGI(TAG, "Queue datastream payload for UI: uid=%lu payload=%s",
            static_cast<unsigned long>(uid), payload.c_str());
        std::string payload_copy = payload;
        Schedule([uid, payload = std::move(payload_copy)]() {
            ESP_LOGI(TAG, "Datastream event on main loop: uid=%lu payload=%s",
                static_cast<unsigned long>(uid), payload.c_str());
            auto display = Board::GetInstance().GetDisplay();
            DatastreamDisplayActions actions;
            bool parsed = ParseDatastreamDisplayActions(payload, &actions);
            bool displayed = false;

#if CONFIG_USE_WEATHER_MESSAGE_STYLE
            if (actions.has_weather) {
                std::string weather_text = BuildWeatherDisplayText(actions.weather);
                ESP_LOGI(TAG, "Datastream weather action: uid=%lu condition=%s city=%s temp=%s duration_ms=%d",
                    static_cast<unsigned long>(uid),
                    actions.weather.condition_id.empty() ? "<none>" : actions.weather.condition_id.c_str(),
                    actions.weather.city.empty() ? "<none>" : actions.weather.city.c_str(),
                    actions.weather.temp_text.empty() ? "<none>" : actions.weather.temp_text.c_str(),
                    actions.weather.duration_ms);
                if (display != nullptr && !weather_text.empty()) {
                    display->ShowWeather(actions.weather.condition_id.c_str(), weather_text.c_str(),
                        actions.weather.city.c_str(), actions.weather.temp_text.c_str(),
                        actions.weather.duration_ms);
                    displayed = true;
                }
            }
#endif

            std::string action_type = actions.emotion_action_type;
            std::string emotion = actions.emotion;
            bool has_emotion_action = actions.has_emotion_action;
            if (!has_emotion_action && !actions.saw_action) {
                has_emotion_action = ParseDatastreamEmotionKeyword(payload, &action_type, &emotion);
                parsed = parsed || has_emotion_action;
            }
            if (!has_emotion_action) {
                if (!parsed) {
                    ESP_LOGW(TAG, "Failed to parse queued datastream payload: uid=%lu payload=%s",
                        static_cast<unsigned long>(uid), payload.c_str());
                } else if (!displayed && !actions.first_action_type.empty()) {
                    ESP_LOGI(TAG, "Ignoring unsupported datastream action: uid=%lu action=%s payload=%s",
                        static_cast<unsigned long>(uid), actions.first_action_type.c_str(), payload.c_str());
                } else if (!displayed) {
                    ESP_LOGW(TAG, "Datastream payload parsed but nothing was displayed: uid=%lu payload=%s",
                        static_cast<unsigned long>(uid), payload.c_str());
                }
                return;
            }

            if (action_type == "stop_emotion") {
                emotion = "neutral";
            } else if (action_type != "display_emotion" && action_type != "play_emotion" &&
                       action_type != "emotion" && action_type != "display_emoji") {
                if (!displayed) {
                    ESP_LOGI(TAG, "Ignoring unsupported datastream action: uid=%lu action=%s payload=%s",
                        static_cast<unsigned long>(uid), action_type.c_str(), payload.c_str());
                }
                return;
            }

            if (emotion.empty()) {
                if (!displayed) {
                    ESP_LOGW(TAG, "Unsupported datastream emotion payload: uid=%lu action=%s payload=%s",
                        static_cast<unsigned long>(uid), action_type.c_str(), payload.c_str());
                }
                return;
            }

            ESP_LOGI(TAG, "Datastream emotion action: uid=%lu action=%s emotion=%s",
                static_cast<unsigned long>(uid), action_type.c_str(), emotion.c_str());
            if (display != nullptr) {
                ESP_LOGI(TAG, "Applying emotion on main loop: %s", emotion.c_str());
                display->SetEmotion(emotion.c_str());
                displayed = true;
            }

            if (!displayed) {
                ESP_LOGW(TAG, "Datastream payload parsed but nothing was displayed: uid=%lu payload=%s",
                    static_cast<unsigned long>(uid), payload.c_str());
            }
        });
    });
    if (!agora_rtc_session_.Start(config)) {
        SetError(agora_rtc_session_.last_error());
        agora_agent_manager_.Stop();
        return false;
    }
    ESP_LOGI(TAG, "Agora RTC start requested");

    for (int i = 0; i < 100 && !agora_rtc_session_.is_in_channel(); ++i) {
        if (!network_connected_) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (!agora_rtc_session_.is_in_channel()) {
        if (!agora_rtc_session_.last_error().empty()) {
            SetError(agora_rtc_session_.last_error());
            StopAgoraSessionInternal();
            return false;
        }
        SetError("Agora RTC join callback timed out");
        StopAgoraSessionInternal();
        return false;
    }
    ESP_LOGI(TAG, "Agora RTC join callback received; starting audio pipeline");

    auto camera = Board::GetInstance().GetCamera();
    audio_service_.ResetDecoder();
    audio_service_.SetAudioHardwareBusy(true);
    if (!agora_audio_bridge_.Start(audio_codec_, &audio_service_, &agora_rtc_session_,
            config.pcm_sample_rate, config.pcm_frame_bytes, use_device_aec)) {
        if (camera != nullptr) {
            camera->SetSessionActive(false);
        }
        if (use_device_aec) {
            audio_service_.EnableDeviceAec(false);
            audio_service_.EnableVoiceProcessing(false);
        }
        audio_service_.SetAudioHardwareBusy(false);
        SetError("Failed to start Agora audio bridge");
        StopAgoraSessionInternal();
        return false;
    }
    ESP_LOGI(TAG, "Agora audio bridge started");

    if (use_device_aec) {
        ESP_LOGI(TAG, "Enabling device-side AEC for Agora uplink");
        if (!audio_service_.EnableVoiceProcessing(true) || !audio_service_.EnableDeviceAec(true)) {
            ESP_LOGW(TAG, "Device-side AEC unavailable, falling back to raw Agora uplink");
            audio_service_.EnableDeviceAec(false);
            audio_service_.EnableVoiceProcessing(false);
            if (audio_codec_ != nullptr) {
                audio_codec_->EnableSoftwareInputReference(false);
            }
            agora_audio_bridge_.Stop();
            use_device_aec = false;
            if (!agora_audio_bridge_.Start(audio_codec_, &audio_service_, &agora_rtc_session_,
                    config.pcm_sample_rate, config.pcm_frame_bytes, false)) {
                if (camera != nullptr) {
                    camera->SetSessionActive(false);
                }
                audio_service_.SetAudioHardwareBusy(false);
                SetError("Failed to restart Agora audio bridge after AEC fallback");
                StopAgoraSessionInternal();
                return false;
            }
            ESP_LOGI(TAG, "Agora audio bridge restarted with raw uplink");
        }
    }

    last_error_message_.clear();
    return true;
}

void Application::StopAgoraSessionInternal() {
    ESP_LOGI(TAG,
        "StopAgoraSessionInternal begin: session_running=%d rtc_started=%d rtc_in_channel=%d conversation_running=%d conversation_id=%s",
        session_running_.load(), agora_rtc_session_.is_started(), agora_rtc_session_.is_in_channel(),
        agora_agent_manager_.is_running(),
        agora_agent_manager_.conversation_id().empty() ? "<empty>" : agora_agent_manager_.conversation_id().c_str());
    auto camera = Board::GetInstance().GetCamera();
    if (camera != nullptr) {
        camera->SetSessionActive(false);
    }
    agora_video_bridge_.Stop();
    agora_audio_bridge_.Stop();
    audio_service_.EnableDeviceAec(false);
    audio_service_.EnableVoiceProcessing(false);
    if (audio_codec_ != nullptr) {
        audio_codec_->EnableSoftwareInputReference(false);
    }
    agora_rtc_session_.Stop();
    agora_rtc_session_.SetIncomingAudioCallback(nullptr);
    agora_rtc_session_.SetStreamMessageCallback(nullptr);
    audio_service_.SetAudioHardwareBusy(false);
    ESP_LOGI(TAG, "StopAgoraSessionInternal: local RTC/audio stopped, requesting conversation stop");
    ESP_LOGI(TAG, "Conversation stop info: channel=%s conversation_id=%s",
        agora_agent_manager_.config().channel_name.c_str(),
        agora_agent_manager_.conversation_id().empty() ? "<empty>" : agora_agent_manager_.conversation_id().c_str());
    agora_agent_manager_.StopAsync();
    session_running_ = false;
    ESP_LOGI(TAG, "StopAgoraSessionInternal end");
}
