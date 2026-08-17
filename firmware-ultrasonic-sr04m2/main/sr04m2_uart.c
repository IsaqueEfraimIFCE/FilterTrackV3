#include <stdbool.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// ESP GPIO21 -> SR04M-2 Trig/RX, ESP GPIO20 <- SR04M-2 Echo/TX.
#define SR04M_TRIG_GPIO          21
#define SR04M_ECHO_GPIO          20
#define SR04M_SAMPLE_PERIOD_MS   100
#define SR04M_ECHO_TIMEOUT_US    45000
#define SR04M_TRIGGER_PULSE_US   20

static const char *TAG = "sr04m2";

typedef enum {
    SR04M_MEASUREMENT_OK,
    SR04M_ECHO_NEVER_RAISED,
    SR04M_ECHO_HIGH_BEFORE_TRIGGER,
    SR04M_ECHO_STUCK_HIGH,
} sr04m_measurement_result_t;

static void sr04m_gpio_init(void)
{
    const gpio_config_t trig_config = {
        .pin_bit_mask = 1ULL << SR04M_TRIG_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    const gpio_config_t echo_config = {
        .pin_bit_mask = 1ULL << SR04M_ECHO_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    ESP_ERROR_CHECK(gpio_config(&trig_config));
    ESP_ERROR_CHECK(gpio_config(&echo_config));
    ESP_ERROR_CHECK(gpio_set_level(SR04M_TRIG_GPIO, 0));
}

static sr04m_measurement_result_t sr04m_read_distance_mm(uint16_t *distance_mm)
{
    if (gpio_get_level(SR04M_ECHO_GPIO) != 0) {
        return SR04M_ECHO_HIGH_BEFORE_TRIGGER;
    }

    gpio_set_level(SR04M_TRIG_GPIO, 0);
    esp_rom_delay_us(5);
    gpio_set_level(SR04M_TRIG_GPIO, 1);
    esp_rom_delay_us(SR04M_TRIGGER_PULSE_US);
    gpio_set_level(SR04M_TRIG_GPIO, 0);

    int64_t deadline = esp_timer_get_time() + SR04M_ECHO_TIMEOUT_US;
    while (gpio_get_level(SR04M_ECHO_GPIO) == 0) {
        if (esp_timer_get_time() >= deadline) {
            return SR04M_ECHO_NEVER_RAISED;
        }
    }

    int64_t echo_started_us = esp_timer_get_time();
    while (gpio_get_level(SR04M_ECHO_GPIO) == 1) {
        if (esp_timer_get_time() >= deadline) {
            return SR04M_ECHO_STUCK_HIGH;
        }
    }

    int64_t echo_width_us = esp_timer_get_time() - echo_started_us;
    // Datasheet conversion: distance cm = echo width us / 57.5.
    *distance_mm = (uint16_t)((echo_width_us * 4 + 11) / 23);
    return SR04M_MEASUREMENT_OK;
}

void app_main(void)
{
    sr04m_gpio_init();
    ESP_LOGI(TAG, "SR04M-2 trigger/echo reader ready: TRIG=GPIO%d ECHO=GPIO%d",
             SR04M_TRIG_GPIO, SR04M_ECHO_GPIO);

    while (true) {
        uint16_t distance_mm;
        sr04m_measurement_result_t result = sr04m_read_distance_mm(&distance_mm);
        if (result == SR04M_MEASUREMENT_OK) {
            ESP_LOGI(TAG, "distance=%u mm", distance_mm);
        } else if (result == SR04M_ECHO_NEVER_RAISED) {
            ESP_LOGW(TAG, "fault: echo never rose after trigger");
        } else if (result == SR04M_ECHO_HIGH_BEFORE_TRIGGER) {
            ESP_LOGW(TAG, "fault: echo high before trigger");
        } else {
            ESP_LOGW(TAG, "fault: echo remained high longer than %d us",
                     SR04M_ECHO_TIMEOUT_US);
        }
        vTaskDelay(pdMS_TO_TICKS(SR04M_SAMPLE_PERIOD_MS));
    }
}
