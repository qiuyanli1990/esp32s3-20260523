#include "wifi_board.h"
#include "korvo2_audio_codec.h"
#include "application.h"
#include "button.h"
#include "config.h"
#include "display/display.h"
#include "assets/lang_config.h"

#include <driver/i2c_master.h>
#include <esp_log.h>
#include <esp_io_expander_tca9554.h>

#define TAG "esp32s3_korvo_2"

typedef enum {
    BSP_ADC_BUTTON_REC,
    BSP_ADC_BUTTON_VOL_MUTE,
    BSP_ADC_BUTTON_PLAY,
    BSP_ADC_BUTTON_SET,
    BSP_ADC_BUTTON_VOL_DOWN,
    BSP_ADC_BUTTON_VOL_UP,
    BSP_ADC_BUTTON_NUM
} bsp_adc_button_t;

class Esp32S3Korvo2Board : public WifiBoard {
private:
    Button boot_button_;
    Button* adc_button_[BSP_ADC_BUTTON_NUM] = {};
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
    adc_oneshot_unit_handle_t adc_handle_ = nullptr;
#endif
    i2c_master_bus_handle_t i2c_bus_ = nullptr;
    esp_io_expander_handle_t io_expander_ = nullptr;

    void InitializeI2c() {
        i2c_master_bus_config_t i2c_bus_cfg = {
            .i2c_port = I2C_NUM_1,
            .sda_io_num = AUDIO_CODEC_I2C_SDA_PIN,
            .scl_io_num = AUDIO_CODEC_I2C_SCL_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .trans_queue_depth = 0,
            .flags = {
                .enable_internal_pullup = 1,
            },
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_cfg, &i2c_bus_));
    }

    void InitializeAudioControlExpander() {
        esp_err_t ret = esp_io_expander_new_i2c_tca9554(
            i2c_bus_, ESP_IO_EXPANDER_I2C_TCA9554_ADDRESS_000, &io_expander_);
        if (ret != ESP_OK) {
            ret = esp_io_expander_new_i2c_tca9554(
                i2c_bus_, ESP_IO_EXPANDER_I2C_TCA9554A_ADDRESS_000, &io_expander_);
        }
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "TCA9554 not found, continuing with direct codec GPIO control");
            return;
        }

        ESP_ERROR_CHECK(esp_io_expander_set_dir(io_expander_,
            IO_EXPANDER_PIN_NUM_0 | IO_EXPANDER_PIN_NUM_5, IO_EXPANDER_OUTPUT));

        // P0 drives PA_CTRL on Korvo-2. P5 drives PERI_PWR_ON and is active low.
        ESP_ERROR_CHECK(esp_io_expander_set_level(io_expander_, IO_EXPANDER_PIN_NUM_0, 1));
        ESP_ERROR_CHECK(esp_io_expander_set_level(io_expander_, IO_EXPANDER_PIN_NUM_5, 0));
        ESP_LOGI(TAG, "Audio control expander initialized: PA enabled, peripheral power enabled");
    }

    void ChangeVolume(int delta) {
        auto codec = GetAudioCodec();
        int volume = codec->output_volume() + delta;
        if (volume > 100) {
            volume = 100;
        }
        if (volume < 0) {
            volume = 0;
        }
        codec->SetOutputVolume(volume);
        GetDisplay()->ShowNotification(Lang::Strings::VOLUME + std::to_string(volume));
    }

    void ToggleMute() {
        auto codec = GetAudioCodec();
        int volume = codec->output_volume() > 1 ? 0 : 50;
        codec->SetOutputVolume(volume);
        GetDisplay()->ShowNotification(Lang::Strings::VOLUME + std::to_string(volume));
    }

    void InitializeButtons() {
        boot_button_.OnClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting) {
                EnterWifiConfigMode();
                return;
            }
            app.ToggleChatState();
        });

#if CONFIG_USE_DEVICE_AEC
        boot_button_.OnDoubleClick([]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateIdle) {
                app.SetAecMode(app.GetAecMode() == kAecOff ? kAecOnDeviceSide : kAecOff);
            }
        });
#endif

        button_adc_config_t adc_cfg = {};
        adc_cfg.adc_channel = BUTTON_ADC_CHANNEL;
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
        const adc_oneshot_unit_init_cfg_t init_config = {
            .unit_id = ADC_UNIT_1,
        };
        ESP_ERROR_CHECK(adc_oneshot_new_unit(&init_config, &adc_handle_));
        adc_cfg.adc_handle = &adc_handle_;
#endif

        adc_cfg.button_index = BSP_ADC_BUTTON_REC;
        adc_cfg.min = 2310;
        adc_cfg.max = 2510;
        adc_button_[BSP_ADC_BUTTON_REC] = new AdcButton(adc_cfg);

        adc_cfg.button_index = BSP_ADC_BUTTON_VOL_MUTE;
        adc_cfg.min = 1880;
        adc_cfg.max = 2080;
        adc_button_[BSP_ADC_BUTTON_VOL_MUTE] = new AdcButton(adc_cfg);

        adc_cfg.button_index = BSP_ADC_BUTTON_PLAY;
        adc_cfg.min = 1550;
        adc_cfg.max = 1750;
        adc_button_[BSP_ADC_BUTTON_PLAY] = new AdcButton(adc_cfg);

        adc_cfg.button_index = BSP_ADC_BUTTON_SET;
        adc_cfg.min = 1015;
        adc_cfg.max = 1215;
        adc_button_[BSP_ADC_BUTTON_SET] = new AdcButton(adc_cfg);

        adc_cfg.button_index = BSP_ADC_BUTTON_VOL_DOWN;
        adc_cfg.min = 720;
        adc_cfg.max = 920;
        adc_button_[BSP_ADC_BUTTON_VOL_DOWN] = new AdcButton(adc_cfg);

        adc_cfg.button_index = BSP_ADC_BUTTON_VOL_UP;
        adc_cfg.min = 280;
        adc_cfg.max = 480;
        adc_button_[BSP_ADC_BUTTON_VOL_UP] = new AdcButton(adc_cfg);

        adc_button_[BSP_ADC_BUTTON_REC]->OnClick([]() {
            Application::GetInstance().ToggleChatState();
        });
        adc_button_[BSP_ADC_BUTTON_SET]->OnClick([this]() {
            EnterWifiConfigMode();
        });
        adc_button_[BSP_ADC_BUTTON_VOL_MUTE]->OnClick([this]() {
            ToggleMute();
        });
        adc_button_[BSP_ADC_BUTTON_VOL_DOWN]->OnClick([this]() {
            ChangeVolume(-10);
        });
        adc_button_[BSP_ADC_BUTTON_VOL_UP]->OnClick([this]() {
            ChangeVolume(10);
        });
        adc_button_[BSP_ADC_BUTTON_VOL_DOWN]->OnLongPress([this]() {
            GetAudioCodec()->SetOutputVolume(0);
            GetDisplay()->ShowNotification(Lang::Strings::MUTED);
        });
        adc_button_[BSP_ADC_BUTTON_VOL_UP]->OnLongPress([this]() {
            GetAudioCodec()->SetOutputVolume(100);
            GetDisplay()->ShowNotification(Lang::Strings::MAX_VOLUME);
        });
    }

public:
    Esp32S3Korvo2Board() : boot_button_(BOOT_BUTTON_GPIO) {
        ESP_LOGI(TAG, "Initializing ESP32-S3-Korvo-2 audio-only board");
        InitializeI2c();
        InitializeAudioControlExpander();
        InitializeButtons();
    }

    virtual AudioCodec* GetAudioCodec() override {
        static Korvo2AudioCodec audio_codec(
            i2c_bus_,
            AUDIO_INPUT_SAMPLE_RATE,
            AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_MCLK,
            AUDIO_I2S_GPIO_BCLK,
            AUDIO_I2S_GPIO_WS,
            AUDIO_I2S_GPIO_DOUT,
            AUDIO_I2S_GPIO_DIN,
            AUDIO_CODEC_PA_PIN,
            AUDIO_CODEC_ES8311_ADDR,
            AUDIO_CODEC_ES7210_ADDR,
            AUDIO_INPUT_REFERENCE,
            AUDIO_INPUT_MIC_CHANNEL_MASK,
            AUDIO_INPUT_REFERENCE_CHANNEL_MASK);
        return &audio_codec;
    }
};

DECLARE_BOARD(Esp32S3Korvo2Board);
