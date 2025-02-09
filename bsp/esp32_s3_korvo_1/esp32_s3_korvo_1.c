/*
 * SPDX-FileCopyrightText: 2023-2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "driver/i2c.h"
#include "esp_err.h"
#include "esp_log.h"

#include "iot_button.h"
#include "bsp/esp-bsp.h"
#include "bsp_err_check.h"
#include "esp_spiffs.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "led_indicator.h"
#include "esp_vfs_fat.h"

static const char *TAG = "S3-Korvo-1";

static bool i2c_initialized = false;
sdmmc_card_t *bsp_sdcard = NULL;    // Global uSD card handler

esp_err_t bsp_i2c_init(void)
{
    /* I2C was initialized before */
    if (i2c_initialized) {
        return ESP_OK;
    }

    const i2c_config_t i2c_conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = BSP_I2C_SDA,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_io_num = BSP_I2C_SCL,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = CONFIG_BSP_I2C_CLK_SPEED_HZ
    };
    BSP_ERROR_CHECK_RETURN_ERR(i2c_param_config(BSP_I2C_NUM, &i2c_conf));
    BSP_ERROR_CHECK_RETURN_ERR(i2c_driver_install(BSP_I2C_NUM, i2c_conf.mode, 0, 0, 0));

    i2c_initialized = true;

    return ESP_OK;
}

esp_err_t bsp_i2c_deinit(void)
{
    BSP_ERROR_CHECK_RETURN_ERR(i2c_driver_delete(BSP_I2C_NUM));
    i2c_initialized = false;
    return ESP_OK;
}

esp_codec_dev_handle_t bsp_audio_codec_speaker_init(void)
{
    const audio_codec_data_if_t *i2s_data_if = bsp_audio_get_codec_itf_spk();
    if (i2s_data_if == NULL) {
        /* Initilize I2C */
        BSP_ERROR_CHECK_RETURN_NULL(bsp_i2c_init());
        /* Configure I2S peripheral and Power Amplifier */
        BSP_ERROR_CHECK_RETURN_NULL(bsp_audio_init(NULL));
        i2s_data_if = bsp_audio_get_codec_itf_spk();
    }
    assert(i2s_data_if);

    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();

    audio_codec_i2c_cfg_t i2c_cfg = {
        .port = BSP_I2C_NUM,
        .addr = ES8311_CODEC_DEFAULT_ADDR,
    };
    const audio_codec_ctrl_if_t *i2c_ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    BSP_NULL_CHECK(i2c_ctrl_if, NULL);

    esp_codec_dev_hw_gain_t gain = {
        .pa_voltage = 5.0,
        .codec_dac_voltage = 3.3,
    };

    es8311_codec_cfg_t es8311_cfg = {
        .ctrl_if = i2c_ctrl_if,
        .gpio_if = gpio_if,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC,
        .pa_pin = BSP_POWER_AMP_IO,
        .pa_reverted = false,
        .master_mode = false,
        .use_mclk = false,
        .digital_mic = false,
        .invert_mclk = false,
        .invert_sclk = false,
        .hw_gain = gain,
    };
    const audio_codec_if_t *es8311_dev = es8311_codec_new(&es8311_cfg);
    BSP_NULL_CHECK(es8311_dev, NULL);

    esp_codec_dev_cfg_t codec_dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_OUT,
        .codec_if = es8311_dev,
        .data_if = i2s_data_if,
    };
    return esp_codec_dev_new(&codec_dev_cfg);
}

esp_codec_dev_handle_t bsp_audio_codec_microphone_init(void)
{
    const audio_codec_data_if_t *i2s_data_if = bsp_audio_get_codec_itf_mic();
    if (i2s_data_if == NULL) {
        /* Initilize I2C */
        BSP_ERROR_CHECK_RETURN_NULL(bsp_i2c_init());
        /* Configure I2S peripheral and Power Amplifier */
        BSP_ERROR_CHECK_RETURN_NULL(bsp_audio_init(NULL));
        i2s_data_if = bsp_audio_get_codec_itf_mic();
    }
    assert(i2s_data_if);

    audio_codec_i2c_cfg_t i2c_cfg = {
        .port = BSP_I2C_NUM,
        .addr = ES7210_CODEC_DEFAULT_ADDR,
    };
    const audio_codec_ctrl_if_t *i2c_ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    BSP_NULL_CHECK(i2c_ctrl_if, NULL);

    es7210_codec_cfg_t es7210_cfg = {
        .ctrl_if = i2c_ctrl_if,
        .mic_selected = ES7120_SEL_MIC1 | ES7120_SEL_MIC2,
    };
    const audio_codec_if_t *es7210_dev = es7210_codec_new(&es7210_cfg);
    BSP_NULL_CHECK(es7210_dev, NULL);

    esp_codec_dev_cfg_t codec_es7210_dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_IN,
        .codec_if = es7210_dev,
        .data_if = i2s_data_if,
    };
    return esp_codec_dev_new(&codec_es7210_dev_cfg);
}

/**
 * @brief led configuration structure
 *
 * This configuration is used by default in bsp_led_init()
 */

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
static adc_oneshot_unit_handle_t bsp_adc_handle = NULL;
#endif

extern blink_step_t const *bsp_led_blink_defaults_lists[];

static const button_config_t bsp_button_config[BSP_BUTTON_NUM] = {
    {
        .type = BUTTON_TYPE_ADC,
        .adc_button_config.adc_channel = ADC_CHANNEL_7, // ADC1 channel 7 is GPIO8
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
        .adc_button_config.adc_handle = &bsp_adc_handle,
#endif
        .adc_button_config.button_index = BSP_BUTTON_REC,
        .adc_button_config.min = 2310, // middle is 2410mV
        .adc_button_config.max = 2510,
    },
    {
        .type = BUTTON_TYPE_ADC,
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
        .adc_button_config.adc_handle = &bsp_adc_handle,
#endif
        .adc_button_config.adc_channel = ADC_CHANNEL_7, // ADC1 channel 7 is GPIO8
        .adc_button_config.button_index = BSP_BUTTON_MODE,
        .adc_button_config.min = 1880, // middle is 1980mV
        .adc_button_config.max = 2080,
    },
    {
        .type = BUTTON_TYPE_ADC,
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
        .adc_button_config.adc_handle = &bsp_adc_handle,
#endif
        .adc_button_config.adc_channel = ADC_CHANNEL_7, // ADC1 channel 7 is GPIO8
        .adc_button_config.button_index = BSP_BUTTON_PLAY,
        .adc_button_config.min = 1560, // middle is 1660mV
        .adc_button_config.max = 1760,
    },
    {
        .type = BUTTON_TYPE_ADC,
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
        .adc_button_config.adc_handle = &bsp_adc_handle,
#endif
        .adc_button_config.adc_channel = ADC_CHANNEL_7, // ADC1 channel 7 is GPIO8
        .adc_button_config.button_index = BSP_BUTTON_SET,
        .adc_button_config.min = 1010, // middle is 1100mV
        .adc_button_config.max = 1210,
    },
    {
        .type = BUTTON_TYPE_ADC,
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
        .adc_button_config.adc_handle = &bsp_adc_handle,
#endif
        .adc_button_config.adc_channel = ADC_CHANNEL_7, // ADC1 channel 7 is GPIO8
        .adc_button_config.button_index = BSP_BUTTON_VOLDOWN,
        .adc_button_config.min = 720, // middle is 820mV
        .adc_button_config.max = 920,
    },
    {
        .type = BUTTON_TYPE_ADC,
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
        .adc_button_config.adc_handle = &bsp_adc_handle,
#endif
        .adc_button_config.adc_channel = ADC_CHANNEL_7, // ADC1 channel 7 is GPIO8
        .adc_button_config.button_index = BSP_BUTTON_VOLUP,
        .adc_button_config.min = 280, // middle is 380mV
        .adc_button_config.max = 480,
    }
};

esp_err_t bsp_iot_button_create(button_handle_t btn_array[], int *btn_cnt, int btn_array_size)
{
    esp_err_t ret = ESP_OK;
    if ((btn_array_size < BSP_BUTTON_NUM) ||
            (btn_array == NULL)) {
        return ESP_ERR_INVALID_ARG;
    }
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
    /* Initialize ADC and get ADC handle */
    BSP_ERROR_CHECK_RETURN_NULL(bsp_adc_initialize());
    bsp_adc_handle = bsp_adc_get_handle();
#endif

    if (btn_cnt) {
        *btn_cnt = 0;
    }
    for (int i = 0; i < BSP_BUTTON_NUM; i++) {
        btn_array[i] = iot_button_create(&bsp_button_config[i]);
        if (btn_array[i] == NULL) {
            ret = ESP_FAIL;
            break;
        }
        if (btn_cnt) {
            (*btn_cnt)++;
        }
    }
    return ret;
}

static const led_strip_config_t bsp_leds_rgb_strip_config = {
    .strip_gpio_num = BSP_LED_RGB_GPIO,   // The GPIO that connected to the LED strip's data line
    .max_leds = BSP_LED_NUM,                  // The number of LEDs in the strip,
    .led_model = LED_MODEL_WS2812,            // LED strip model
    .flags.invert_out = false,                // whether to invert the output signal
};

static const led_strip_rmt_config_t bsp_leds_rgb_rmt_config = {
#if ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(5, 0, 0)
    .rmt_channel = 0,
#else
    .clk_src = RMT_CLK_SRC_DEFAULT,        // different clock source can lead to different power consumption
    .resolution_hz = 10 * 1000 * 1000,     // RMT counter clock frequency = 10MHz
    .flags.with_dma = false,               // DMA feature is available on ESP target like ESP32-S3
#endif
};

static led_indicator_strips_config_t bsp_leds_rgb_config = {
    .is_active_level_high = 1,
    .led_strip_cfg = bsp_leds_rgb_strip_config,
    .led_strip_driver = LED_STRIP_RMT,
    .led_strip_rmt_cfg = bsp_leds_rgb_rmt_config,
};

static const led_indicator_config_t bsp_leds_config = {
    .mode = LED_STRIPS_MODE,
    .led_indicator_strips_config = &bsp_leds_rgb_config,
    .blink_lists = bsp_led_blink_defaults_lists,
    .blink_list_num = BSP_LED_MAX,
};

esp_err_t bsp_led_indicator_create(led_indicator_handle_t led_array[], int *led_cnt, int led_array_size)
{
    if (led_array == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    led_array[0] = led_indicator_create(&bsp_leds_config);
    if (led_array[0] == NULL) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t bsp_spiffs_mount(void)
{
    esp_vfs_spiffs_conf_t conf = {
        .base_path = CONFIG_BSP_SPIFFS_MOUNT_POINT,
        .partition_label = CONFIG_BSP_SPIFFS_PARTITION_LABEL,
        .max_files = CONFIG_BSP_SPIFFS_MAX_FILES,
#ifdef CONFIG_BSP_SPIFFS_FORMAT_ON_MOUNT_FAIL
        .format_if_mount_failed = true,
#else
        .format_if_mount_failed = false,
#endif
    };

    esp_err_t ret_val = esp_vfs_spiffs_register(&conf);

    BSP_ERROR_CHECK_RETURN_ERR(ret_val);

    size_t total = 0, used = 0;
    ret_val = esp_spiffs_info(conf.partition_label, &total, &used);
    if (ret_val != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get SPIFFS partition information (%s)", esp_err_to_name(ret_val));
    } else {
        ESP_LOGI(TAG, "Partition size: total: %d, used: %d", total, used);
    }

    return ret_val;
}

esp_err_t bsp_spiffs_unmount(void)
{
    return esp_vfs_spiffs_unregister(CONFIG_BSP_SPIFFS_PARTITION_LABEL);
}

esp_err_t bsp_sdcard_mount(void)
{
    const esp_vfs_fat_sdmmc_mount_config_t mount_config = {
#ifdef CONFIG_BSP_SD_FORMAT_ON_MOUNT_FAIL
        .format_if_mount_failed = true,
#else
        .format_if_mount_failed = false,
#endif
        .max_files = CONFIG_BSP_SD_MAX_FILES,
        .allocation_unit_size = 16 * 1024
    };

    const sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    const sdmmc_slot_config_t slot_config = {
        .clk = BSP_SD_CLK,
        .cmd = BSP_SD_CMD,
        .d0 = BSP_SD_D0,
        .d1 = GPIO_NUM_NC,
        .d2 = GPIO_NUM_NC,
        .d3 = GPIO_NUM_NC,
        .d4 = GPIO_NUM_NC,
        .d5 = GPIO_NUM_NC,
        .d6 = GPIO_NUM_NC,
        .d7 = GPIO_NUM_NC,
        .cd = SDMMC_SLOT_NO_CD,
        .wp = SDMMC_SLOT_NO_WP,
        .width = 1,
        .flags = 0,
    };

    return esp_vfs_fat_sdmmc_mount(BSP_SD_MOUNT_POINT, &host, &slot_config, &mount_config, &bsp_sdcard);
}

esp_err_t bsp_sdcard_unmount(void)
{
    return esp_vfs_fat_sdcard_unmount(BSP_SD_MOUNT_POINT, bsp_sdcard);
}
