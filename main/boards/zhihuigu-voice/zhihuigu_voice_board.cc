#include "wifi_board.h"
#include "box_audio_codec_lite.h"
#include "display/oled_display.h"
#include "application.h"
#include "button.h"
#include "config.h"
#include "sleep_timer.h"
#include "led/gpio_led.h"
#include "assets/lang_config.h"

#include <esp_log.h>
#include <esp_idf_version.h>
#include <button_adc.h>
#include <esp_sleep.h>
#include <driver/gpio.h>
#include <driver/i2c_master.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_vendor.h>

#define TAG "ZhihuiguVoiceBoard"

/* ADC Buttons: 前三个按键挂在 ADC1 通道 0（GPIO1）的分压网络上 */
typedef enum {
    BSP_ADC_BUTTON_PREV,
    BSP_ADC_BUTTON_ENTER,
    BSP_ADC_BUTTON_NEXT,
    BSP_ADC_BUTTON_NUM
} bsp_adc_button_t;

class ZhihuiguVoiceBoard : public WifiBoard {
private:
    i2c_master_bus_handle_t i2c_bus_ = nullptr;
    i2c_master_bus_handle_t display_i2c_bus_ = nullptr;
    esp_lcd_panel_io_handle_t panel_io_ = nullptr;
    esp_lcd_panel_handle_t panel_ = nullptr;
    Display* display_ = nullptr;
    Button boot_button_;
    Button* adc_button_[BSP_ADC_BUTTON_NUM];
    adc_oneshot_unit_handle_t bsp_adc_handle = NULL;
    SleepTimer* sleep_timer_ = nullptr;

    void InitializeI2c() {
        // 音频编解码器 I2C（I2C1，SDA=8，SCL=18）
        i2c_master_bus_config_t i2c_bus_cfg = {
            .i2c_port = (i2c_port_t)1,
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

    void InitializeDisplayI2c() {
        // OLED 显示 I2C（I2C0，SDA=6，SCL=7）
        i2c_master_bus_config_t bus_config = {
            .i2c_port = (i2c_port_t)0,
            .sda_io_num = DISPLAY_SDA_PIN,
            .scl_io_num = DISPLAY_SCL_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .trans_queue_depth = 0,
            .flags = {
                .enable_internal_pullup = 1,
            },
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&bus_config, &display_i2c_bus_));
    }

    void InitializeSsd1306Display() {
        // SSD1306 config
        esp_lcd_panel_io_i2c_config_t io_config = {
            .dev_addr = 0x3C,
            .scl_speed_hz = 400 * 1000,
            .control_phase_bytes = 1,
            .dc_bit_offset = 6,
            .lcd_cmd_bits = 8,
            .lcd_param_bits = 8,
            .on_color_trans_done = nullptr,
            .user_ctx = nullptr,
            .flags = {
                .dc_low_on_data = 0,
                .disable_control_phase = 0,
            },
        };

        ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c(display_i2c_bus_, &io_config, &panel_io_));

        ESP_LOGI(TAG, "Install SSD1306 driver");
        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = GPIO_NUM_NC;
        panel_config.bits_per_pixel = 1;

        esp_lcd_panel_ssd1306_config_t ssd1306_config = {
            .height = static_cast<uint8_t>(DISPLAY_HEIGHT),
        };
        panel_config.vendor_config = &ssd1306_config;

        ESP_ERROR_CHECK(esp_lcd_new_panel_ssd1306(panel_io_, &panel_config, &panel_));
        ESP_LOGI(TAG, "SSD1306 driver installed");

        // Reset the display
        ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_));
        if (esp_lcd_panel_init(panel_) != ESP_OK) {
            ESP_LOGE(TAG, "Failed to initialize display");
            display_ = new NoDisplay();
            return;
        }

        // Set the display to on
        ESP_LOGI(TAG, "Turning display on");
        ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_, true));

        display_ = new OledDisplay(panel_io_, panel_, DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);
    }

    void InitializeGpio() {
        // GPIO39: AUX 电源使能（高电平打开）
        gpio_config_t io_aux = {
            .pin_bit_mask = (1ULL << AUX_POWER_CTRL_PIN),
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        ESP_ERROR_CHECK(gpio_config(&io_aux));
        gpio_set_level(AUX_POWER_CTRL_PIN, 1);
        ESP_LOGI(TAG, "AUX power enabled: GPIO%d high", static_cast<int>(AUX_POWER_CTRL_PIN));

        // GPIO45: OLED 电源使能（低电平打开）
        gpio_config_t io_oled = {
            .pin_bit_mask = (1ULL << OLED_POWER_CTRL_PIN),
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        ESP_ERROR_CHECK(gpio_config(&io_oled));
        gpio_set_level(OLED_POWER_CTRL_PIN, 0);
        ESP_LOGI(TAG, "OLED power enabled: GPIO%d low", static_cast<int>(OLED_POWER_CTRL_PIN));
    }

    void HandleUserActivity() {
        if (sleep_timer_) {
            sleep_timer_->WakeUp();
        }
    }

    void ChangeVol(int val) {
        auto codec = GetAudioCodec();
        auto volume = codec->output_volume() + val;
        if (volume > 100) {
            volume = 100;
        }
        if (volume < 0) {
            volume = 0;
        }
        codec->SetOutputVolume(volume);
        GetDisplay()->ShowNotification(Lang::Strings::VOLUME + std::to_string(volume));
    }

    void ToggleState() {
        auto& app = Application::GetInstance();
        if (app.GetDeviceState() == kDeviceStateStarting) {
            EnterWifiConfigMode();
            return;
        }
        app.ToggleChatState();
    }

    void InitializeButtons() {
        /* Initialize ADC: 前三个按键是 ADC 按键而非 GPIO 按键 */
        button_adc_config_t adc_cfg = {};
        adc_cfg.adc_channel = ADC_CHANNEL_0; // ADC1 channel 0 is GPIO1
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
        const adc_oneshot_unit_init_cfg_t init_config1 = {
            .unit_id = ADC_UNIT_1,
        };
        adc_oneshot_new_unit(&init_config1, &bsp_adc_handle);
        adc_cfg.adc_handle = &bsp_adc_handle;
#endif
        adc_cfg.button_index = BSP_ADC_BUTTON_PREV;
        adc_cfg.min = 2310; // middle is 2410mV
        adc_cfg.max = 2510;
        adc_button_[0] = new AdcButton(adc_cfg);

        adc_cfg.button_index = BSP_ADC_BUTTON_ENTER;
        adc_cfg.min = 1880; // middle is 1980mV
        adc_cfg.max = 2080;
        adc_button_[1] = new AdcButton(adc_cfg);

        adc_cfg.button_index = BSP_ADC_BUTTON_NEXT;
        adc_cfg.min = 720; // middle is 820mV
        adc_cfg.max = 920;
        adc_button_[2] = new AdcButton(adc_cfg);

        auto volume_up_button = adc_button_[BSP_ADC_BUTTON_NEXT];
        volume_up_button->OnClick([this]() {
            HandleUserActivity();
            ChangeVol(10);
        });
        volume_up_button->OnLongPress([this]() {
            HandleUserActivity();
            GetAudioCodec()->SetOutputVolume(100);
            GetDisplay()->ShowNotification(Lang::Strings::MAX_VOLUME);
        });

        auto volume_down_button = adc_button_[BSP_ADC_BUTTON_PREV];
        volume_down_button->OnClick([this]() {
            HandleUserActivity();
            ChangeVol(-10);
        });
        volume_down_button->OnLongPress([this]() {
            HandleUserActivity();
            GetAudioCodec()->SetOutputVolume(0);
            GetDisplay()->ShowNotification(Lang::Strings::MUTED);
        });

        auto break_button = adc_button_[BSP_ADC_BUTTON_ENTER];
        break_button->OnClick([this]() {
            HandleUserActivity();
            ToggleState();
        });

        // BOOT 按键（GPIO0）：按住说话
        boot_button_.OnPressDown([this]() {
            HandleUserActivity();
            Application::GetInstance().StartListening();
        });
        boot_button_.OnPressUp([this]() {
            HandleUserActivity();
            Application::GetInstance().StopListening();
        });
    }

    void InitializePowerSaveTimer() {
        // 30 秒无交互进入轻睡眠；BOOT 按键可随时唤醒
        sleep_timer_ = new SleepTimer(30);
        sleep_timer_->OnEnterLightSleepMode([this]() {
            ESP_LOGI(TAG, "Entering light sleep, turning off AUX power");
            // 断开 AUX 电源
            gpio_set_level(AUX_POWER_CTRL_PIN, 0);
            // OLED 进入显示待机
            GetDisplay()->SetPowerSaveMode(true);
            // 使能 BOOT 按键（GPIO0）低电平唤醒轻睡眠
            gpio_wakeup_enable(BOOT_BUTTON_GPIO, GPIO_INTR_LOW_LEVEL);
            esp_sleep_enable_gpio_wakeup();
        });
        sleep_timer_->OnExitLightSleepMode([this]() {
            ESP_LOGI(TAG, "Exiting light sleep, restoring AUX power");
            // 恢复 OLED 显示
            GetDisplay()->SetPowerSaveMode(false);
            // 重新打开 AUX 电源
            gpio_set_level(AUX_POWER_CTRL_PIN, 1);
        });
        sleep_timer_->SetEnabled(true);
    }

public:
    ZhihuiguVoiceBoard() : boot_button_(BOOT_BUTTON_GPIO) {
        ESP_LOGI(TAG, "Initializing zhihuigu-voice board");
        InitializeGpio();  // 先打开外设电源
        InitializeI2c();
        InitializeDisplayI2c();
        InitializeSsd1306Display();
        InitializeButtons();
        InitializePowerSaveTimer();
        auto codec = GetAudioCodec();
        codec->SetOutputVolume(80);
    }

    ~ZhihuiguVoiceBoard() {
        for (int i = 0; i < BSP_ADC_BUTTON_NUM; i++) {
            if (adc_button_[i]) {
                delete adc_button_[i];
                adc_button_[i] = nullptr;
            }
        }
    }

    virtual Led* GetLed() override {
        // GPIO38: 低电平点亮的状态 LED，随设备状态变化（聆听时呼吸等）
        static GpioLed led(BUILTIN_LED_GPIO, 1);
        return &led;
    }

    virtual AudioCodec* GetAudioCodec() override {
        static BoxAudioCodecLite audio_codec(
            i2c_bus_,
            AUDIO_INPUT_SAMPLE_RATE,
            AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_MCLK,
            AUDIO_I2S_GPIO_BCLK,
            AUDIO_I2S_GPIO_WS,
            AUDIO_I2S_GPIO_DOUT,
            AUDIO_I2S_GPIO_DIN,
            AUDIO_CODEC_PA_PIN,
            AUDIO_INPUT_REFERENCE);
        return &audio_codec;
    }

    virtual Display* GetDisplay() override {
        return display_;
    }

    virtual void SetPowerSaveLevel(PowerSaveLevel level) override {
        if (level != PowerSaveLevel::LOW_POWER) {
            sleep_timer_->WakeUp();
        }
        WifiBoard::SetPowerSaveLevel(level);
    }
};

DECLARE_BOARD(ZhihuiguVoiceBoard);
