#include "emote_display.h"

// Standard C++ headers
#include <cstring>
#include <memory>
#include <algorithm>
#include <cinttypes>

// Standard C headers
#include <sys/time.h>
#include <time.h>

// ESP-IDF headers
#include <esp_log.h>
#include <esp_lcd_panel_io.h>
#include <esp_timer.h>
#include <lvgl.h>

// FreeRTOS headers
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// Project headers
#include "assets/lang_config.h"
#include "application.h"
#include "board.h"
#include "gfx.h"
#include "expression_emote.h"
#include "system_info.h"


namespace emote {

// ============================================================================
// Constants and Type Definitions
// ============================================================================

static const char* TAG = "EmoteDisplay";

// ============================================================================
// Forward Declarations
// ============================================================================

class EmoteDisplay;

// ============================================================================
// Helper Functions
// ============================================================================

static bool OnFlushIoReady(const esp_lcd_panel_io_handle_t panel_io,
    esp_lcd_panel_io_event_data_t* const edata, void* user_ctx)
{
    emote_handle_t handle = static_cast<emote_handle_t>(user_ctx);
    if (handle) {
        emote_notify_flush_finished(handle);
    }
    return true;
}

// Flush callback for emote
static void OnFlushCallback(int x_start, int y_start, int x_end, int y_end, const void* data, emote_handle_t handle)
{
    esp_lcd_panel_handle_t panel = (esp_lcd_panel_handle_t)emote_get_user_data(handle);
    if (panel != nullptr) {
        esp_lcd_panel_draw_bitmap(panel, x_start, y_start, x_end, y_end, data);
    }
}

// ============================================================================
// Graphics Initialization Functions
// ============================================================================

static emote_handle_t InitializeEmote(const esp_lcd_panel_handle_t panel, const int width, const int height)
{
    if (!panel) {
        ESP_LOGE(TAG, "Invalid panel");
        return nullptr;
    }

    emote_config_t emote_cfg = {
        .flags = {
            .swap = true,
            .double_buffer = true,
            .buff_dma = false,
        },
        .gfx_emote = {
            .h_res = width,
            .v_res = height,
            .fps = 30,
        },
        .buffers = {
            .buf_pixels = static_cast<size_t>(width * 16),
        },
        .task = {
            .task_priority = 5,
            .task_stack = 6 * 1024,
            .task_affinity = 0,
            .task_stack_in_ext = false,
        },
        .flush_cb = OnFlushCallback,
        .user_data = (void*)panel,
    };

    emote_handle_t emote_handle = emote_init(&emote_cfg);
    if (!emote_handle) {
        ESP_LOGE(TAG, "Failed to initialize emote");
        return nullptr;
    }

    return emote_handle;
}

static std::string GetWeatherAnimationName(const char* condition)
{
    if (condition == nullptr || condition[0] == '\0') {
        return "weather-cloudy";
    }
    return std::string("weather-") + condition;
}

static bool HasText(const char* text)
{
    return text != nullptr && text[0] != '\0';
}

// ============================================================================
// EmoteDisplay Class Implementation
// ============================================================================

EmoteDisplay::EmoteDisplay(const esp_lcd_panel_handle_t panel, const esp_lcd_panel_io_handle_t panel_io,
                           const int width, const int height)
{
    width_ = width;
    height_ = height;
    emote_handle_ = InitializeEmote(panel, width, height);

    const esp_timer_create_args_t weather_timer_args = {
        .callback = &EmoteDisplay::WeatherTimerCallback,
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "weather_ui",
        .skip_unhandled_events = true,
    };
    esp_err_t err = esp_timer_create(&weather_timer_args, &weather_timer_);
    if (err != ESP_OK) {
        weather_timer_ = nullptr;
        ESP_LOGE(TAG, "Failed to create weather timer: %s", esp_err_to_name(err));
    }

    const esp_lcd_panel_io_callbacks_t cbs = {
        .on_color_trans_done = OnFlushIoReady,
    };
    esp_lcd_panel_io_register_event_callbacks(panel_io, &cbs, emote_handle_);
}

EmoteDisplay::~EmoteDisplay()
{
    StopWeatherTimer();
    if (weather_timer_ != nullptr) {
        esp_timer_delete(weather_timer_);
        weather_timer_ = nullptr;
    }

    if (emote_handle_) {
        emote_deinit(emote_handle_);
        emote_handle_ = nullptr;
    }
}

void EmoteDisplay::SetEmotion(const char* const emotion)
{
    ESP_LOGI(TAG, "SetEmotion: %s", emotion);
    StopWeatherTimer();
    HideWeatherSideLabels();
    StopAnimDialog();
    if (emote_handle_ && emotion && strlen(emotion) > 0) {
        emote_set_anim_emoji(emote_handle_, emotion);
    }
}

void EmoteDisplay::SetChatMessage(const char* const role, const char* const content)
{
    ESP_LOGI(TAG, "SetChatMessage: %s, %s", role, content);
    StopWeatherTimer();
    HideWeatherSideLabels();
    StopAnimDialog();
    if (emote_handle_ && content && strlen(content) > 0) {
        if (std::strcmp(role, "system") == 0) {
            size_t len = strlen(content);
            char* new_content = new char[len + 1];
            strcpy(new_content, content);
            std::replace(new_content, new_content + len, static_cast<char>(0x0A), static_cast<char>(0x20));
            emote_set_event_msg(emote_handle_, EMOTE_MGR_EVT_SYS, new_content);
            delete[] new_content;
        } else {
            emote_set_event_msg(emote_handle_, EMOTE_MGR_EVT_SPEAK, content);
        }
    }
}

void EmoteDisplay::SetStatus(const char* const status)
{
    ESP_LOGI(TAG, "SetStatus: %s", status);
    StopWeatherTimer();
    HideWeatherSideLabels();
    StopAnimDialog();
    if (emote_handle_ && status && strlen(status) > 0) {
        if (std::strcmp(status, Lang::Strings::LISTENING) == 0) {
            emote_set_event_msg(emote_handle_, EMOTE_MGR_EVT_LISTEN, NULL);
        } else if (std::strcmp(status, Lang::Strings::STANDBY) == 0) {
            emote_set_event_msg(emote_handle_, EMOTE_MGR_EVT_IDLE, NULL);
        } else if (std::strcmp(status, Lang::Strings::SPEAKING) == 0) {
            emote_set_event_msg(emote_handle_, EMOTE_MGR_EVT_SPEAK, NULL);
        } else if (std::strcmp(status, Lang::Strings::ERROR) == 0) {
            emote_set_event_msg(emote_handle_, EMOTE_MGR_EVT_SET, NULL);
        } else {
            emote_set_event_msg(emote_handle_, EMOTE_MGR_EVT_SYS, status);
        }
    }
}

void EmoteDisplay::ShowNotification(const char* notification, int duration_ms)
{
    (void)duration_ms;
    ESP_LOGI(TAG, "ShowNotification: %s", notification);
    StopWeatherTimer();
    HideWeatherSideLabels();
    StopAnimDialog();
    if (emote_handle_ && notification && strlen(notification) > 0) {
        emote_set_event_msg(emote_handle_, EMOTE_MGR_EVT_SYS, notification);
    }
}

void EmoteDisplay::ShowWeather(const char* condition, const char* summary,
    const char* city, const char* temperature, int duration_ms)
{
    int effective_duration_ms = duration_ms > 0 ? duration_ms : 3000;
    ESP_LOGI(TAG, "ShowWeather: condition=%s summary=%s city=%s temperature=%s duration_ms=%d",
        condition ? condition : "",
        summary ? summary : "",
        city ? city : "",
        temperature ? temperature : "",
        effective_duration_ms);

    std::string weather_emote = GetWeatherAnimationName(condition);
    if (!InsertAnimDialog(weather_emote.c_str(), effective_duration_ms)) {
        ESP_LOGW(TAG, "Weather animation not found: %s", weather_emote.c_str());
    }

    ShowWeatherSideLabels(city, temperature);
    RestartWeatherTimer(effective_duration_ms);
}

void EmoteDisplay::UpdateStatusBar(bool update_all)
{
    ESP_LOGD(TAG, "UpdateStatusBar: %s", update_all ? "true" : "false");
    if (!emote_handle_) {
        return;
    }
}

void EmoteDisplay::SetPowerSaveMode(bool on)
{
    ESP_LOGI(TAG, "SetPowerSaveMode: %s", on ? "ON" : "OFF");
    if (!emote_handle_) {
        return;
    }
}

void EmoteDisplay::SetPreviewImage(const void* image)
{
    if (image) {
        ESP_LOGI(TAG, "SetPreviewImage: Preview image not supported, using default icon");
    }
}

void EmoteDisplay::SetTheme(Theme* const theme)
{
    ESP_LOGI(TAG, "SetTheme: %p", theme);
}

bool EmoteDisplay::Lock(const int timeout_ms)
{
    (void)timeout_ms;
    return true;
}

void EmoteDisplay::Unlock()
{
}

void EmoteDisplay::WeatherTimerCallback(void* arg)
{
    auto* display = static_cast<EmoteDisplay*>(arg);
    if (display != nullptr) {
        display->RestoreStatusForCurrentState();
    }
}

void EmoteDisplay::RestartWeatherTimer(int duration_ms)
{
    if (weather_timer_ == nullptr || duration_ms <= 0) {
        return;
    }

    StopWeatherTimer();
    esp_err_t err = esp_timer_start_once(weather_timer_, static_cast<uint64_t>(duration_ms) * 1000ULL);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to start weather timer: %s", esp_err_to_name(err));
    }
}

void EmoteDisplay::StopWeatherTimer()
{
    if (weather_timer_ == nullptr) {
        return;
    }

    esp_err_t err = esp_timer_stop(weather_timer_);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "Failed to stop weather timer: %s", esp_err_to_name(err));
    }
}

void EmoteDisplay::ShowWeatherSideLabels(const char* city, const char* temperature)
{
    if (!emote_handle_) {
        return;
    }

    bool has_city = HasText(city);
    bool has_temperature = HasText(temperature);
    if (!has_city && !has_temperature) {
        HideWeatherSideLabels();
        return;
    }

    if (weather_city_label_ == nullptr) {
        weather_city_label_ = emote_create_obj_by_type(emote_handle_, EMOTE_OBJ_TYPE_LABEL, "weather_city_label");
    }
    if (weather_temperature_label_ == nullptr) {
        weather_temperature_label_ = emote_create_obj_by_type(emote_handle_, EMOTE_OBJ_TYPE_LABEL, "weather_temperature_label");
    }

    if (weather_city_label_ == nullptr && weather_temperature_label_ == nullptr) {
        ESP_LOGW(TAG, "Failed to create weather side labels");
        return;
    }

    int screen_width = width_ > 0 ? width_ : 320;
    int label_width = std::max(72, std::min(screen_width >= 480 ? 180 : 108, (screen_width - 120) / 2));
    int label_height = height_ >= 360 ? 36 : 30;
    int x_offset = 40 + 12 + label_width / 2;
    int y_offset = 20;

    esp_err_t err = emote_lock(emote_handle_);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to lock emote for weather labels: %s", esp_err_to_name(err));
        return;
    }

    if (weather_city_label_ != nullptr) {
        gfx_obj_set_size(weather_city_label_, label_width, label_height);
        gfx_obj_align(weather_city_label_, GFX_ALIGN_CENTER, -x_offset, y_offset);
        gfx_label_set_color(weather_city_label_, GFX_COLOR_HEX(0xFFFFFF));
        gfx_label_set_text_align(weather_city_label_, GFX_TEXT_ALIGN_RIGHT);
        gfx_label_set_long_mode(weather_city_label_, GFX_LABEL_LONG_CLIP);
        gfx_label_set_text(weather_city_label_, has_city ? city : "");
        gfx_obj_set_visible(weather_city_label_, has_city);
    }

    if (weather_temperature_label_ != nullptr) {
        gfx_obj_set_size(weather_temperature_label_, label_width, label_height);
        gfx_obj_align(weather_temperature_label_, GFX_ALIGN_CENTER, x_offset, y_offset);
        gfx_label_set_color(weather_temperature_label_, GFX_COLOR_HEX(0xFFFFFF));
        gfx_label_set_text_align(weather_temperature_label_, GFX_TEXT_ALIGN_LEFT);
        gfx_label_set_long_mode(weather_temperature_label_, GFX_LABEL_LONG_CLIP);
        gfx_label_set_text(weather_temperature_label_, has_temperature ? temperature : "");
        gfx_obj_set_visible(weather_temperature_label_, has_temperature);
    }

    emote_unlock(emote_handle_);
}

void EmoteDisplay::HideWeatherSideLabels()
{
    if (!emote_handle_ || (weather_city_label_ == nullptr && weather_temperature_label_ == nullptr)) {
        return;
    }

    esp_err_t err = emote_lock(emote_handle_);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to lock emote for hiding weather labels: %s", esp_err_to_name(err));
        return;
    }

    if (weather_city_label_ != nullptr) {
        gfx_label_set_text(weather_city_label_, "");
        gfx_obj_set_visible(weather_city_label_, false);
    }
    if (weather_temperature_label_ != nullptr) {
        gfx_label_set_text(weather_temperature_label_, "");
        gfx_obj_set_visible(weather_temperature_label_, false);
    }

    emote_unlock(emote_handle_);
}

void EmoteDisplay::RestoreStatusForCurrentState()
{
    auto device_state = Application::GetInstance().GetDeviceState();
    switch (device_state) {
    case kDeviceStateStarting:
        SetStatus(SystemInfo::GetUserAgent().c_str());
        break;
    case kDeviceStateIdle:
        SetStatus(Lang::Strings::STANDBY);
        break;
    case kDeviceStateConnecting:
        SetStatus(Lang::Strings::CONNECTING);
        break;
    case kDeviceStateListening:
    case kDeviceStateSpeaking:
        SetStatus(Lang::Strings::LISTENING);
        break;
    case kDeviceStateWifiConfiguring:
        SetStatus(Lang::Strings::WIFI_CONFIG_MODE);
        break;
    case kDeviceStateFatalError:
        SetStatus(Lang::Strings::ERROR);
        break;
    default:
        break;
    }
}

bool EmoteDisplay::StopAnimDialog()
{
    ESP_LOGI(TAG, "StopAnimDialog");
    if (emote_handle_) {
        return emote_stop_anim_dialog(emote_handle_);
    }
    return false;
}

bool EmoteDisplay::InsertAnimDialog(const char* emoji_name, uint32_t duration_ms)
{
    ESP_LOGI(TAG, "InsertAnimDialog: %s, %" PRIu32, emoji_name, duration_ms);
    if (emote_handle_ && emoji_name) {
        return emote_insert_anim_dialog(emote_handle_, emoji_name, duration_ms);
    }
    return false;
}

void EmoteDisplay::RefreshAll()
{
    if (emote_handle_) {
        emote_notify_all_refresh(emote_handle_);
        return;
    }
}

} // namespace emote
