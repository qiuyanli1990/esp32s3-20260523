#pragma once

#include "display.h"
#include <memory>
#include <string>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include "expression_emote.h"

namespace emote {

class EmoteDisplay : public Display {
public:
    EmoteDisplay(esp_lcd_panel_handle_t panel, esp_lcd_panel_io_handle_t panel_io, int width, int height);
    virtual ~EmoteDisplay();

    virtual void SetEmotion(const char* emotion) override;
    virtual void SetStatus(const char* status) override;
    virtual void SetChatMessage(const char* role, const char* content) override;
    virtual void SetTheme(Theme* theme) override;
    virtual void ShowNotification(const char* notification, int duration_ms = 3000) override;
    using Display::ShowWeather;
    virtual void ShowWeather(const char* condition, const char* summary,
        const char* city, const char* temperature, int duration_ms = 3000) override;
    virtual void UpdateStatusBar(bool update_all = false) override;
    virtual void SetPowerSaveMode(bool on) override;
    virtual void SetPreviewImage(const void* image);

    bool StopAnimDialog();
    bool InsertAnimDialog(const char* emoji_name, uint32_t duration_ms);

    void RefreshAll();

    // Get emote handle for internal use
    emote_handle_t GetEmoteHandle() const { return emote_handle_; }

private:
    virtual bool Lock(int timeout_ms = 0) override;
    virtual void Unlock() override;

    static void WeatherTimerCallback(void* arg);
    void RestartWeatherTimer(int duration_ms);
    void StopWeatherTimer();
    void ShowWeatherSideLabels(const char* city, const char* temperature);
    void HideWeatherSideLabels();
    void RestoreStatusForCurrentState();

    emote_handle_t emote_handle_ = nullptr;
    esp_timer_handle_t weather_timer_ = nullptr;
    gfx_obj_t* weather_city_label_ = nullptr;
    gfx_obj_t* weather_temperature_label_ = nullptr;

};

} // namespace emote
