// One-shot MMA8452Q test: reads X/Y/Z and prints to UART (115200 baud).
// Board: ESP32-C3. I2C: SDA=GPIO8, SCL=GPIO9 (matches FilterTrackv3 wiring).

#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c.h"
#include "driver/gpio.h"
#include "esp_log.h"

#define I2C_PORT        I2C_NUM_0
#define I2C_SDA         GPIO_NUM_8
#define I2C_SCL         GPIO_NUM_9
#define I2C_FREQ_HZ     10000
#define SENSOR_PWR_PIN  GPIO_NUM_10  // Carrier board gates sensor 3V3 via this pin

// MMA8452Q 7-bit I2C address. Default is 0x1D (SA0 tied high on most breakouts).
// If WHO_AM_I read fails, try 0x1C (SA0 tied low).
#define MMA8452_ADDR    0x1C

// MMA8452Q registers
#define REG_STATUS          0x00
#define REG_OUT_X_MSB       0x01
#define REG_WHO_AM_I        0x0D
#define REG_XYZ_DATA_CFG    0x0E
#define REG_CTRL_REG1       0x2A

#define WHO_AM_I_EXPECTED   0x2A

static const char *TAG = "MMA8452";

static esp_err_t i2c_init(void)
{
    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = I2C_SDA,
        .scl_io_num = I2C_SCL,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = I2C_FREQ_HZ,
    };
    esp_err_t ret = i2c_param_config(I2C_PORT, &conf);
    if (ret != ESP_OK) return ret;
    return i2c_driver_install(I2C_PORT, conf.mode, 0, 0, 0);
}

static esp_err_t mma_write(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = { reg, val };
    return i2c_master_write_to_device(I2C_PORT, MMA8452_ADDR, buf, 2,
                                      pdMS_TO_TICKS(100));
}

static esp_err_t mma_read(uint8_t reg, uint8_t *data, size_t len)
{
    return i2c_master_write_read_device(I2C_PORT, MMA8452_ADDR, &reg, 1,
                                        data, len, pdMS_TO_TICKS(100));
}

static esp_err_t mma_init(void)
{
    uint8_t who = 0;
    esp_err_t ret = mma_read(REG_WHO_AM_I, &who, 1);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "WHO_AM_I read failed: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "WHO_AM_I = 0x%02X (expected 0x%02X)", who, WHO_AM_I_EXPECTED);
    if (who != WHO_AM_I_EXPECTED) {
        ESP_LOGW(TAG, "Unexpected WHO_AM_I — check address (try 0x1C) and wiring");
    }

    // Standby: clear CTRL_REG1 to configure.
    ret = mma_write(REG_CTRL_REG1, 0x00);
    if (ret != ESP_OK) return ret;

    // ±2g full scale, no high-pass filter.
    ret = mma_write(REG_XYZ_DATA_CFG, 0x00);
    if (ret != ESP_OK) return ret;

    // CTRL_REG1: data rate 100 Hz (DR=011 -> bits 5:3 = 011), ACTIVE bit set.
    ret = mma_write(REG_CTRL_REG1, (0x03 << 3) | 0x01);
    if (ret != ESP_OK) return ret;

    return ESP_OK;
}

static esp_err_t mma_read_xyz(float *x_g, float *y_g, float *z_g)
{
    uint8_t raw[6];
    esp_err_t ret = mma_read(REG_OUT_X_MSB, raw, 6);
    if (ret != ESP_OK) return ret;

    // 12-bit left-justified signed: combine MSB and upper 4 bits of LSB.
    int16_t x = (int16_t)((raw[0] << 8) | raw[1]) >> 4;
    int16_t y = (int16_t)((raw[2] << 8) | raw[3]) >> 4;
    int16_t z = (int16_t)((raw[4] << 8) | raw[5]) >> 4;

    // ±2g, 12-bit -> 1024 counts per g.
    const float counts_per_g = 1024.0f;
    *x_g = x / counts_per_g;
    *y_g = y / counts_per_g;
    *z_g = z / counts_per_g;
    return ESP_OK;
}

static void i2c_scan(void)
{
    ESP_LOGI(TAG, "Scanning I2C bus (0x08..0x77)...");
    int found = 0;
    for (uint8_t addr = 0x08; addr <= 0x77; addr++) {
        i2c_cmd_handle_t cmd = i2c_cmd_link_create();
        i2c_master_start(cmd);
        i2c_master_write_byte(cmd, (addr << 1) | I2C_MASTER_WRITE, true);
        i2c_master_stop(cmd);
        esp_err_t ret = i2c_master_cmd_begin(I2C_PORT, cmd, pdMS_TO_TICKS(50));
        i2c_cmd_link_delete(cmd);
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "  ACK from 0x%02X", addr);
            found++;
        }
    }
    ESP_LOGI(TAG, "Scan complete — %d device(s) found", found);
}

void app_main(void)
{
    ESP_LOGI(TAG, "MMA8452 test starting");

    // Enable carrier-board sensor power rail (GPIO10 HIGH).
    gpio_config_t pwr_cfg = {
        .pin_bit_mask = 1ULL << SENSOR_PWR_PIN,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&pwr_cfg);
    gpio_set_level(SENSOR_PWR_PIN, 1);
    ESP_LOGI(TAG, "Sensor power rail (GPIO%d) -> HIGH; waiting 200 ms for sensor startup", SENSOR_PWR_PIN);
    vTaskDelay(pdMS_TO_TICKS(200));

    // Pre-I2C GPIO-level idle check: weak pull-up + read.
    // Healthy idle = both HIGH. LOW = short to GND, missing pull-up, or device stuck.
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << I2C_SDA) | (1ULL << I2C_SCL),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);
    vTaskDelay(pdMS_TO_TICKS(10));
    int sda_level = gpio_get_level(I2C_SDA);
    int scl_level = gpio_get_level(I2C_SCL);
    ESP_LOGI(TAG, "Idle pin levels: SDA(GPIO%d)=%d  SCL(GPIO%d)=%d  (both should be 1)",
             I2C_SDA, sda_level, I2C_SCL, scl_level);

    ESP_ERROR_CHECK(i2c_init());

    i2c_scan();

    // Try WHO_AM_I at both candidate addresses for diagnostics.
    for (uint8_t a = 0x1C; a <= 0x1D; a++) {
        uint8_t reg = REG_WHO_AM_I, who = 0;
        esp_err_t r = i2c_master_write_read_device(I2C_PORT, a, &reg, 1, &who, 1, pdMS_TO_TICKS(100));
        ESP_LOGI(TAG, "Probe 0x%02X: %s WHO_AM_I=0x%02X",
                 a, esp_err_to_name(r), who);
    }

    if (mma_init() != ESP_OK) {
        ESP_LOGE(TAG, "MMA8452 init failed — halting");
        while (1) vTaskDelay(pdMS_TO_TICKS(1000));
    }

    ESP_LOGI(TAG, "MMA8452 ready — streaming X/Y/Z (g) at 5 Hz");

    while (1) {
        float xg, yg, zg;
        esp_err_t ret = mma_read_xyz(&xg, &yg, &zg);
        if (ret == ESP_OK) {
            printf("X=%+6.3f g  Y=%+6.3f g  Z=%+6.3f g\n", xg, yg, zg);
        } else {
            ESP_LOGW(TAG, "read failed: %s", esp_err_to_name(ret));
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}
