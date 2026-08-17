#include <stdio.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <stdbool.h>
#include <math.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/ringbuf.h"
#include "driver/gpio.h"
#include "driver/i2c.h"
#include "esp_timer.h"
#include "esp_rom_sys.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_app_desc.h"

#include "esp_bt.h"
#include "esp_gap_ble_api.h"
#include "esp_gatt_common_api.h"
#include "esp_gatts_api.h"
#include "esp_bt_main.h"

// ─── Sensor ultrassônico ───────────────────────────────────────────────────
#define TRIG_PIN        GPIO_NUM_21
#define ECHO_PIN        GPIO_NUM_20
#define SENSOR_PWR_ENABLED 0
#define SENSOR_PWR_PIN  GPIO_NUM_NC
#define SENSOR_PWR_ON_LEVEL  1
#define SENSOR_PWR_OFF_LEVEL 0
#define SENSOR_PWR_STABILIZE_MS 200

#define TIMEOUT_US      45000       // timeout do echo (45 ms)
#define INTERVALO_MS    100         // intervalo entre leituras (100 ms)
#define LOW_POWER_INTERVAL_MS 1000  // intervalo no modo baixo consumo (1 s)
#define WATCHDOG_US     5000000     // 5 s sem leitura válida = erro

// Accelerometer I2C wiring for the ESP32-C6 carrier board (SDA=GPIO13, SCL=GPIO12).
#define LSM303_I2C_SDA_IO           GPIO_NUM_13
#define LSM303_I2C_SCL_IO           GPIO_NUM_12
#define LSM303_I2C_MASTER_NUM       I2C_NUM_0
#define LSM303_I2C_FREQ_HZ          100000
#define LSM303_I2C_TX_BUF_DISABLE   0
#define LSM303_I2C_RX_BUF_DISABLE   0
#define LSM303_I2C_TIMEOUT_MS       100

#define LSM303_ACCEL_ADDR_PRIMARY   0x19
#define LSM303_ACCEL_ADDR_ALT       0x18
#define ACCEL_RETRY_INTERVAL_US     2000000
#define ACCEL_ENABLED                0

#define CTRL_REG1_A                 0x20
#define CTRL_REG4_A                 0x23
#define OUT_X_L_A                   0x28
#define WHO_AM_I_A                  0x0F
#define LSM303_WHO_AM_I             0x33

#define MPU_ACCEL_ADDR              0x68
#define MPU_REG_ACCEL_XOUT          0x3B
#define MPU_REG_PWR_MGMT_1          0x6B
#define MPU_REG_ACCEL_CFG           0x1C
#define MPU_REG_WHO_AM_I            0x75

// ─── LEDs de status ───────────────────────────────────────────────────────
#define LED_RED_PIN     GPIO_NUM_3
#define LED_GREEN_PIN   GPIO_NUM_4
#define LED_YELLOW_PIN  GPIO_NUM_2
#define LED_BLINK_INTERVAL_US 500000

static volatile bool sensor_error = false;

// ─── BLE ──────────────────────────────────────────────────────────────────
#define TAG                 "FilterTrack"
#define SERVICE_UUID        0x00FF
#define CHARACTERISTIC_UUID 0xFF01
#define OTA_CHARACTERISTIC_UUID 0xFF02
#define BLE_NOTIFY_PAYLOAD_MAX 20

// ─── OTA (atualização de firmware via BLE) ────────────────────────────────
// O app escreve "OTA:BEGIN:<bytes>" na characteristic de comando (0xFF01) e em
// seguida envia o .bin em chunks via write-no-response na characteristic 0xFF02.
// O firmware grava na partição OTA inativa e troca o boot no fim ("OTA=OK").
#define OTA_RINGBUF_SIZE        (16 * 1024)
#define OTA_RECV_TIMEOUT_US     30000000    // 30 s sem chunk = transferência morta

static uint16_t gatts_if_global  = 0;
static uint16_t conn_id_global   = 0;
static uint16_t char_handle      = 0;
static uint16_t service_handle_global = 0;
static uint16_t ota_char_handle  = 0;
static volatile bool ota_in_progress = false;
static volatile bool ota_abort_request = false;
static uint32_t ota_expected_size = 0;
static RingbufHandle_t ota_ringbuf = NULL;
static bool     device_connected = false;
static bool     adv_config_done  = false;
static volatile bool low_power_mode = false;
static volatile bool bluetooth_error = false;
static volatile bool ble_advertising = false;
static volatile bool sensor_power_on = false;
static volatile bool lsm303_error = false;
static bool lsm303_initialized = false;
static uint8_t lsm303_accel_addr = LSM303_ACCEL_ADDR_PRIMARY;

typedef enum {
    ACCEL_NONE,
    ACCEL_LSM303,
    ACCEL_MPU,
} accel_type_t;

static accel_type_t accel_type = ACCEL_NONE;

typedef struct {
    int16_t x;
    int16_t y;
    int16_t z;
} vec3i16_t;

static esp_ble_adv_params_t adv_params = {
    .adv_int_min       = 0x20,
    .adv_int_max       = 0x40,
    .adv_type          = ADV_TYPE_IND,
    .own_addr_type     = BLE_ADDR_TYPE_PUBLIC,
    .channel_map       = ADV_CHNL_ALL,
    .adv_filter_policy = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY,
};

static void set_status_leds(bool red_on, bool green_on, bool yellow_on)
{
    gpio_set_level(LED_RED_PIN, red_on ? 1 : 0);
    gpio_set_level(LED_GREEN_PIN, green_on ? 1 : 0);
    gpio_set_level(LED_YELLOW_PIN, yellow_on ? 1 : 0);
}

static void set_sensor_power(bool on)
{
#if !SENSOR_PWR_ENABLED
    sensor_power_on = on;
    return;
#else
    if (sensor_power_on == on) {
        return;
    }

    gpio_set_level(SENSOR_PWR_PIN,
                   on ? SENSOR_PWR_ON_LEVEL : SENSOR_PWR_OFF_LEVEL);
    sensor_power_on = on;

    if (on) {
        vTaskDelay(pdMS_TO_TICKS(SENSOR_PWR_STABILIZE_MS));
    }
#endif
}

static void set_low_power_mode(bool enabled)
{
    if (low_power_mode == enabled) {
        return;
    }

    low_power_mode = enabled;
    set_sensor_power(!enabled);

    if (enabled) {
        gpio_set_level(TRIG_PIN, 0);
        sensor_error = false;
    }
}

static void atualizar_leds_status(bool reading_sensor)
{
    static bool blink_state = false;
    static int64_t last_blink_us = 0;

    int64_t now = esp_timer_get_time();
    if ((now - last_blink_us) >= LED_BLINK_INTERVAL_US) {
        blink_state = !blink_state;
        last_blink_us = now;
    }

    if (low_power_mode) {
        set_status_leds(false, false, true);
        return;
    }

    if (bluetooth_error || sensor_error || lsm303_error) {
        set_status_leds(true, false, false);
        return;
    }

    if (device_connected) {
        set_status_leds(false, true, false);
        return;
    }

    bool active_blink = ble_advertising || reading_sensor;
    set_status_leds(active_blink && blink_state, false, false);
}

static void iniciar_advertising(void)
{
    esp_err_t err = esp_ble_gap_start_advertising(&adv_params);
    if (err != ESP_OK) {
        bluetooth_error = true;
        ble_advertising = false;
        ESP_LOGE(TAG, "Falha ao iniciar advertising: %s", esp_err_to_name(err));
    }
}

// ─── Sensor ───────────────────────────────────────────────────────────────
// --- LSM303DLHC raw readings -------------------------------------------------
static esp_err_t lsm303_i2c_master_init(void)
{
    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = LSM303_I2C_SDA_IO,
        .scl_io_num = LSM303_I2C_SCL_IO,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = LSM303_I2C_FREQ_HZ
    };

    esp_err_t ret = i2c_param_config(LSM303_I2C_MASTER_NUM, &conf);
    if (ret != ESP_OK) {
        return ret;
    }

    return i2c_driver_install(
        LSM303_I2C_MASTER_NUM,
        conf.mode,
        LSM303_I2C_RX_BUF_DISABLE,
        LSM303_I2C_TX_BUF_DISABLE,
        0
    );
}

static esp_err_t accel_write_reg(uint8_t dev_addr, uint8_t reg_addr, uint8_t data)
{
    uint8_t write_buf[2] = {reg_addr, data};

    return i2c_master_write_to_device(
        LSM303_I2C_MASTER_NUM,
        dev_addr,
        write_buf,
        sizeof(write_buf),
        pdMS_TO_TICKS(LSM303_I2C_TIMEOUT_MS)
    );
}

static esp_err_t accel_read_regs(uint8_t dev_addr, uint8_t reg_addr, uint8_t *data, size_t len)
{
    return i2c_master_write_read_device(
        LSM303_I2C_MASTER_NUM,
        dev_addr,
        &reg_addr,
        1,
        data,
        len,
        pdMS_TO_TICKS(LSM303_I2C_TIMEOUT_MS)
    );
}

static void accel_check_idle_pin_levels(void)
{
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << LSM303_I2C_SDA_IO) | (1ULL << LSM303_I2C_SCL_IO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    ESP_ERROR_CHECK(gpio_config(&io));
    vTaskDelay(pdMS_TO_TICKS(10));

    ESP_LOGI(TAG,
             "I2C idle levels: SDA(GPIO%d)=%d SCL(GPIO%d)=%d; both should be 1",
             LSM303_I2C_SDA_IO,
             gpio_get_level(LSM303_I2C_SDA_IO),
             LSM303_I2C_SCL_IO,
             gpio_get_level(LSM303_I2C_SCL_IO));
}

static void accel_i2c_scan(void)
{
    int found = 0;

    ESP_LOGI(TAG,
             "Scanning I2C bus on SDA=GPIO%d SCL=GPIO%d",
             LSM303_I2C_SDA_IO,
             LSM303_I2C_SCL_IO);

    for (uint8_t addr = 0x08; addr <= 0x77; addr++) {
        i2c_cmd_handle_t cmd = i2c_cmd_link_create();
        i2c_master_start(cmd);
        i2c_master_write_byte(cmd, (addr << 1) | I2C_MASTER_WRITE, true);
        i2c_master_stop(cmd);

        esp_err_t ret = i2c_master_cmd_begin(
            LSM303_I2C_MASTER_NUM,
            cmd,
            pdMS_TO_TICKS(50)
        );
        i2c_cmd_link_delete(cmd);

        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "I2C ACK at 0x%02X", addr);
            found++;
        }
    }

    ESP_LOGI(TAG, "I2C scan complete: %d device(s) found", found);
}

static esp_err_t lsm303_init_at(uint8_t addr)
{
    uint8_t who = 0;
    esp_err_t ret = accel_read_regs(addr, WHO_AM_I_A, &who, 1);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "LSM303 WHO_AM_I read failed at 0x%02X: %s",
                 addr, esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "LSM303 accel WHO_AM_I=0x%02X at 0x%02X (expected 0x%02X)",
             who, addr, LSM303_WHO_AM_I);
    if (who != LSM303_WHO_AM_I) {
        ESP_LOGW(TAG, "Unexpected LSM303 WHO_AM_I at 0x%02X; continuing anyway", addr);
    }

    ret = accel_write_reg(addr, CTRL_REG1_A, 0x57);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao configurar CTRL_REG1_A do acelerometro: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = accel_write_reg(addr, CTRL_REG4_A, 0x08);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao configurar CTRL_REG4_A do acelerometro: %s", esp_err_to_name(ret));
        return ret;
    }

    lsm303_accel_addr = addr;
    return ESP_OK;
}

static esp_err_t lsm303_init(void)
{
    const uint8_t addrs[] = {LSM303_ACCEL_ADDR_PRIMARY, LSM303_ACCEL_ADDR_ALT};

    for (size_t i = 0; i < (sizeof(addrs) / sizeof(addrs[0])); i++) {
        if (lsm303_init_at(addrs[i]) == ESP_OK) {
            return ESP_OK;
        }
    }

    return ESP_FAIL;
}

static esp_err_t lsm303_read_accel_raw(vec3i16_t *accel)
{
    uint8_t data[6] = {0};

    esp_err_t ret = accel_read_regs(lsm303_accel_addr, OUT_X_L_A | 0x80, data, sizeof(data));
    if (ret != ESP_OK) {
        return ret;
    }

    int16_t raw_x = (int16_t)((data[1] << 8) | data[0]);
    int16_t raw_y = (int16_t)((data[3] << 8) | data[2]);
    int16_t raw_z = (int16_t)((data[5] << 8) | data[4]);

    accel->x = raw_x >> 4;
    accel->y = raw_y >> 4;
    accel->z = raw_z >> 4;

    return ESP_OK;
}

static esp_err_t mpu_init(void)
{
    uint8_t who = 0;
    esp_err_t ret = accel_read_regs(MPU_ACCEL_ADDR, MPU_REG_WHO_AM_I, &who, 1);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "MPU WHO_AM_I read failed at 0x%02X: %s",
                 MPU_ACCEL_ADDR, esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "MPU WHO_AM_I=0x%02X at address 0x%02X", who, MPU_ACCEL_ADDR);

    ret = accel_write_reg(MPU_ACCEL_ADDR, MPU_REG_PWR_MGMT_1, 0x00);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to wake MPU: %s", esp_err_to_name(ret));
        return ret;
    }

    vTaskDelay(pdMS_TO_TICKS(100));

    ret = accel_write_reg(MPU_ACCEL_ADDR, MPU_REG_ACCEL_CFG, 0x00);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set MPU +/-2g range: %s", esp_err_to_name(ret));
        return ret;
    }

    return ESP_OK;
}

static esp_err_t mpu_read_accel_raw(vec3i16_t *accel)
{
    uint8_t data[6] = {0};
    esp_err_t ret = accel_read_regs(MPU_ACCEL_ADDR, MPU_REG_ACCEL_XOUT, data, sizeof(data));
    if (ret != ESP_OK) {
        return ret;
    }

    accel->x = (int16_t)((data[0] << 8) | data[1]);
    accel->y = (int16_t)((data[2] << 8) | data[3]);
    accel->z = (int16_t)((data[4] << 8) | data[5]);

    return ESP_OK;
}

static esp_err_t accel_init_auto(void)
{
    if (lsm303_init() == ESP_OK) {
        accel_type = ACCEL_LSM303;
        ESP_LOGI(TAG, "Detected LSM303DLHC accelerometer at 0x%02X", lsm303_accel_addr);
        return ESP_OK;
    }

    if (mpu_init() == ESP_OK) {
        accel_type = ACCEL_MPU;
        ESP_LOGI(TAG, "Detected MPU-style accelerometer/IMU");
        return ESP_OK;
    }

    accel_type = ACCEL_NONE;
    return ESP_FAIL;
}

static esp_err_t accel_read_raw(vec3i16_t *accel)
{
    switch (accel_type) {
    case ACCEL_LSM303:
        return lsm303_read_accel_raw(accel);
    case ACCEL_MPU:
        return mpu_read_accel_raw(accel);
    default:
        return ESP_FAIL;
    }
}

static bool ler_accel_raw(vec3i16_t *accel_raw)
{
    esp_err_t ret = accel_read_raw(accel_raw);
    if (ret != ESP_OK) {
        lsm303_error = true;
        lsm303_initialized = false;
        accel_type = ACCEL_NONE;
        ESP_LOGE(TAG, "Falha ao ler acelerometro: %s", esp_err_to_name(ret));
        return false;
    }

    lsm303_error = false;
    return true;
}

static bool tentar_iniciar_acelerometro(bool scan_bus)
{
    if (scan_bus) {
        accel_i2c_scan();
    }

    esp_err_t ret = accel_init_auto();
    if (ret != ESP_OK) {
        lsm303_initialized = false;
        lsm303_error = true;
        ESP_LOGE(TAG, "Falha ao iniciar acelerometro; tentando novamente em %d ms",
                 ACCEL_RETRY_INTERVAL_US / 1000);
        return false;
    }

    lsm303_initialized = true;
    lsm303_error = false;
    ESP_LOGI(TAG, "Acelerometro pronto para leitura raw");
    return true;
}

static bool enviar_notificacao_ble_chunk(const char *msg, size_t len)
{
    if (!device_connected) {
        return false;
    }

    esp_err_t err = esp_ble_gatts_send_indicate(
        gatts_if_global,
        conn_id_global,
        char_handle,
        len,
        (uint8_t *)msg,
        false
    );

    if (err != ESP_OK) {
        bluetooth_error = true;
        ESP_LOGE(TAG, "Falha ao enviar BLE: %s", esp_err_to_name(err));
        return false;
    } else {
        bluetooth_error = false;
        return true;
    }
}

static bool enviar_notificacao_ble(const char *msg)
{
    if (!device_connected) {
        // Sem cliente BLE, espelha no log serial para depuração em bancada.
        ESP_LOGI(TAG, "NOTIFY(off): %s", msg);
        return false;
    }

    size_t msg_len = strlen(msg);
    size_t offset = 0;
    bool ok = true;

    while (offset < msg_len) {
        size_t chunk_len = msg_len - offset;
        if (chunk_len > BLE_NOTIFY_PAYLOAD_MAX) {
            chunk_len = BLE_NOTIFY_PAYLOAD_MAX;
        }

        if (!enviar_notificacao_ble_chunk(msg + offset, chunk_len)) {
            ok = false;
            break;
        }

        offset += chunk_len;
        if (offset < msg_len) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }

    return ok;
}

static void enviar_medidas_ble(bool distancia_ok,
                               float distancia_cm,
                               bool raw_ok,
                               vec3i16_t accel_raw)
{
    char ble_msg[48];
    int offset = 0;

    if (distancia_ok) {
        offset += snprintf(ble_msg + offset, sizeof(ble_msg) - offset, "D=%.2f", distancia_cm);
    } else {
        offset += snprintf(ble_msg + offset, sizeof(ble_msg) - offset, "D=ERRO");
    }

    if (raw_ok) {
        offset += snprintf(
            ble_msg + offset,
            sizeof(ble_msg) - offset,
            ";A=%d,%d,%d",
            accel_raw.x,
            accel_raw.y,
            accel_raw.z
        );
    } else {
        offset += snprintf(ble_msg + offset, sizeof(ble_msg) - offset, ";RAW=ERRO");
    }

    enviar_notificacao_ble(ble_msg);
}

static float medir_distancia_cm(void)
{
    gpio_set_level(TRIG_PIN, 0);
    esp_rom_delay_us(5);
    gpio_set_level(TRIG_PIN, 1);
    esp_rom_delay_us(20);
    gpio_set_level(TRIG_PIN, 0);

    int64_t t0 = esp_timer_get_time();
    while (gpio_get_level(ECHO_PIN) == 0) {
        if ((esp_timer_get_time() - t0) > TIMEOUT_US)
            return -1.0f;
    }

    int64_t echo_start = esp_timer_get_time();
    while (gpio_get_level(ECHO_PIN) == 1) {
        if ((esp_timer_get_time() - echo_start) > TIMEOUT_US)
            return -1.0f;
    }

    int64_t echo_end = esp_timer_get_time();
    float duration = (float)(echo_end - echo_start);

    if (duration < 100.0f)
        return -1.0f;

    return (duration * 0.0349f) / 2.0f;
}

// ─── Partição de armazenamento de dados de sensor ─────────────────────────
// A partição "storage" (dados brutos, ver partitions_ota.csv) fica reservada
// para gravação de leituras de sensor. O self-test abaixo apaga a partição
// inteira, escreve um padrão pseudo-aleatório determinístico em todos os
// bytes e relê tudo conferindo — garante que o espaço todo grava e lê certo.
// Roda uma vez no primeiro boot (marcador em NVS) e sob demanda via comando
// BLE "4". O teste destrói o conteúdo da partição.
#define STORAGE_PARTITION_LABEL "storage"
#define STORAGE_TEST_BLOCK      4096
#define STORAGE_NVS_NAMESPACE   "ftstore"
#define STORAGE_NVS_KEY_TESTED  "selftest"

static volatile bool storage_test_running = false;

// Log de lavagens (modo filtro fixo) — definidos mais abaixo. O self-test
// destrói a partição, então precisa consultar/refazer o log.
static void washlog_init(void);
static uint32_t washlog_pending_count(void);
static bool washlog_is_busy(void);

// xorshift32: mesmo seed → mesma sequência, então o padrão escrito pode ser
// regenerado na leitura sem guardar nada em RAM.
static void storage_fill_pattern(uint8_t *buf, size_t len, uint32_t seed)
{
    uint32_t x = seed ? seed : 0xDEADBEEF;
    for (size_t i = 0; i < len; i += 4) {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        buf[i]     = (uint8_t)(x);
        buf[i + 1] = (uint8_t)(x >> 8);
        buf[i + 2] = (uint8_t)(x >> 16);
        buf[i + 3] = (uint8_t)(x >> 24);
    }
}

static uint32_t storage_block_seed(uint32_t block_index)
{
    return 0xA5A50000u ^ (block_index * 2654435761u);
}

static void storage_notify_status(const char *msg)
{
    if (device_connected) {
        enviar_notificacao_ble(msg);
    }
}

static void storage_selftest_task(void *arg)
{
    const esp_partition_t *part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY,
        STORAGE_PARTITION_LABEL);
    uint8_t *buf = NULL;
    uint8_t *expected = NULL;
    const char *fail_stage = NULL;
    int64_t t0 = esp_timer_get_time();

    if (part == NULL) {
        ESP_LOGE(TAG, "Storage: particao '%s' nao encontrada", STORAGE_PARTITION_LABEL);
        fail_stage = "part";
        goto done;
    }

    buf = malloc(STORAGE_TEST_BLOCK);
    expected = malloc(STORAGE_TEST_BLOCK);
    if (buf == NULL || expected == NULL) {
        fail_stage = "mem";
        goto done;
    }

    ESP_LOGI(TAG, "Storage: self-test iniciando em '%s' (%lu bytes @ 0x%lx)",
             part->label, (unsigned long)part->size, (unsigned long)part->address);

    // Apaga em blocos de 64 KB cedendo CPU entre eles para não travar as
    // outras tasks por vários segundos seguidos.
    for (uint32_t off = 0; off < part->size; off += 0x10000) {
        uint32_t len = (part->size - off) < 0x10000 ? (part->size - off) : 0x10000;
        if (esp_partition_erase_range(part, off, len) != ESP_OK) {
            fail_stage = "erase";
            goto done;
        }
        vTaskDelay(1);
    }
    ESP_LOGI(TAG, "Storage: apagado (%lld ms)",
             (esp_timer_get_time() - t0) / 1000);

    // Escreve o padrão em toda a partição.
    uint32_t nblocks = part->size / STORAGE_TEST_BLOCK;
    int last_pct = 0;
    for (uint32_t b = 0; b < nblocks; b++) {
        storage_fill_pattern(buf, STORAGE_TEST_BLOCK, storage_block_seed(b));
        if (esp_partition_write(part, (size_t)b * STORAGE_TEST_BLOCK, buf,
                                STORAGE_TEST_BLOCK) != ESP_OK) {
            fail_stage = "write";
            goto done;
        }
        int pct = (int)((uint64_t)(b + 1) * 100 / nblocks);
        if (pct >= last_pct + 25) {
            last_pct = pct;
            ESP_LOGI(TAG, "Storage: escrita %d%%", pct);
        }
        if ((b % 16) == 15) {
            vTaskDelay(1);
        }
    }

    // Relê tudo e confere byte a byte contra o padrão regenerado.
    last_pct = 0;
    for (uint32_t b = 0; b < nblocks; b++) {
        if (esp_partition_read(part, (size_t)b * STORAGE_TEST_BLOCK, buf,
                               STORAGE_TEST_BLOCK) != ESP_OK) {
            fail_stage = "read";
            goto done;
        }
        storage_fill_pattern(expected, STORAGE_TEST_BLOCK, storage_block_seed(b));
        if (memcmp(buf, expected, STORAGE_TEST_BLOCK) != 0) {
            ESP_LOGE(TAG, "Storage: dados divergentes no bloco %lu (offset 0x%lx)",
                     (unsigned long)b, (unsigned long)(b * STORAGE_TEST_BLOCK));
            fail_stage = "verify";
            goto done;
        }
        int pct = (int)((uint64_t)(b + 1) * 100 / nblocks);
        if (pct >= last_pct + 25) {
            last_pct = pct;
            ESP_LOGI(TAG, "Storage: leitura %d%%", pct);
        }
        if ((b % 16) == 15) {
            vTaskDelay(1);
        }
    }

done:
    free(buf);
    free(expected);
    if (fail_stage == NULL && part != NULL) {
        int64_t ms = (esp_timer_get_time() - t0) / 1000;
        ESP_LOGI(TAG, "Storage: SELF-TEST OK — %lu bytes gravados e verificados em %lld ms",
                 (unsigned long)part->size, ms);
        char msg[40];
        snprintf(msg, sizeof(msg), "ST=OK,%lu", (unsigned long)part->size);
        storage_notify_status(msg);

        nvs_handle_t nvsh;
        if (nvs_open(STORAGE_NVS_NAMESPACE, NVS_READWRITE, &nvsh) == ESP_OK) {
            nvs_set_u8(nvsh, STORAGE_NVS_KEY_TESTED, 1);
            nvs_commit(nvsh);
            nvs_close(nvsh);
        }
        // O teste sobrescreveu a partição inteira; reformata o log de
        // lavagens para deixá-la utilizável de novo.
        washlog_init();
    } else {
        ESP_LOGE(TAG, "Storage: SELF-TEST FALHOU (%s)",
                 fail_stage ? fail_stage : "?");
        char msg[40];
        snprintf(msg, sizeof(msg), "ST=ERRO,%s", fail_stage ? fail_stage : "?");
        storage_notify_status(msg);
    }
    storage_test_running = false;
    vTaskDelete(NULL);
}

static void storage_selftest_start(const char *origem)
{
    if (storage_test_running) {
        ESP_LOGW(TAG, "Storage: self-test ja em andamento");
        storage_notify_status("ST=ERRO,busy");
        return;
    }
    if (ota_in_progress) {
        storage_notify_status("ST=ERRO,ota");
        return;
    }
    if (washlog_is_busy()) {
        storage_notify_status("ST=ERRO,busy");
        return;
    }
    if (washlog_pending_count() > 0) {
        // Há lavagens ainda não entregues ao app; o teste apagaria tudo.
        // Consuma-as (LOG:READ + LOG:ACK) antes de rodar o teste.
        ESP_LOGW(TAG, "Storage: self-test recusado; ha lavagens pendentes");
        storage_notify_status("ST=ERRO,data");
        return;
    }
    storage_test_running = true;
    ESP_LOGI(TAG, "Storage: self-test agendado (%s)", origem);
    if (xTaskCreate(storage_selftest_task, "storage_st", 4096, NULL, 5, NULL)
            != pdPASS) {
        storage_test_running = false;
        storage_notify_status("ST=ERRO,task");
    }
}

// ─── Modo filtro de areia fixo ─────────────────────────────────────────────
// Em filtros de areia fixos o nível só muda quando o filtro é lavado (algumas
// vezes por dia). Neste modo o firmware monitora o nível sozinho: mantém um
// baseline estável (persistido em NVS) e, quando uma janela estável de
// leituras difere dele em mais de 3 cm, registra a lavagem na partição
// "storage". Os registros sobrevivem a reboot e ficam guardados até um
// smartphone conectar, ler tudo ("LOG:READ") e confirmar o recebimento
// ("LOG:ACK:<n>") — só então a partição é apagada.
#define SAND_SAMPLE_MS_OFFLINE   100    // mantém 10 Hz sem BLE conectado
#define SAND_WINDOW_MAX          64
#define SAND_WINDOW_MIN          8
#define SAND_MIN_VALID_CM        25.0f  // mesma regra do app: abaixo disso é inválido

// ─── Detecção de lavagem: configuração genérica ────────────────────────────
// A lavagem pode ser ascendente ou descendente e outros fatores influenciam o
// processo, então nada é fixo: qualquer desvio sustentado do baseline (nas
// duas direções) abre uma lavagem; dentro dela cada excursão máx↔mín do nível
// vira um "evento", eventos consecutivos com vazão parecida são agregados, e
// a lavagem encerra quando o nível fica estável por um tempo longo. Todos os
// fatores são ajustáveis em tempo real pelo BLE ("CFG:SET:<chave>:<valor>",
// persistidos em NVS) — as chaves ficam na tabela wash_cfg[] abaixo.
typedef struct {
    const char *key;        // chave curta (BLE e NVS)
    const char *desc;       // descrição enviada no CFG:GET
    int32_t     def;        // valor padrão
    int32_t     min, max;   // faixa aceita
    volatile int32_t val;
} wash_cfg_item_t;

enum {
    CFG_START_MM,   // desvio do baseline que abre a lavagem (mm, qualquer direção)
    CFG_START_S,    // por quantos segundos o desvio precisa se sustentar
    CFG_STABLE_MM,  // faixa máx-mín considerada estável (mm)
    CFG_STABLE_S,   // duração de cada janela de estabilidade da lavagem (s)
    CFG_END_S,      // estabilidade contínua que encerra a lavagem (s)
    CFG_MIN_S,      // duração mínima de uma lavagem (s)
    CFG_MAX_S,      // encerramento forçado (s)
    CFG_COOL_S,     // trava até poder detectar outra lavagem (s)
    CFG_EV_MM,      // movimento mínimo para abrir um novo evento (mm)
    CFG_EV_END_S,   // estabilidade que encerra um evento (s)
    CFG_EV_TOL,     // eventos com vazão dentro de ±tol% se agregam
    CFG_DRIFT_MM,   // deriva mínima para regravar o baseline (mm)
    CFG_COUNT
};

static wash_cfg_item_t wash_cfg[CFG_COUNT] = {
    [CFG_START_MM]  = { "start_mm",  "desvio inicial (mm)",           50,   5,  2000, 50 },
    [CFG_START_S]   = { "start_s",   "sustentacao (s)",               5,    1,   120, 5 },
    [CFG_STABLE_MM] = { "stable_mm", "faixa estavel (mm)",            20,   2,   500, 20 },
    [CFG_STABLE_S]  = { "stable_s",  "janela da lavagem (s)",         30,   5,   600, 30 },
    [CFG_END_S]     = { "end_s",     "estabilidade final (s)",        180,  10, 3600, 180 },
    [CFG_MIN_S]     = { "min_s",     "duracao minima (s)",            60,   0,  3600, 60 },
    [CFG_MAX_S]     = { "max_s",     "limite maximo (s)",             1800, 60, 14400, 1800 },
    [CFG_COOL_S]    = { "cool_s",    "trava entre lavagens (s)",      2700, 0,  86400, 2700 },
    [CFG_EV_MM]     = { "ev_mm",     "movimento de evento (mm)",      20,   2,  1000, 20 },
    [CFG_EV_END_S]  = { "ev_end_s",  "estabilidade do evento (s)",    5,    1,   300, 5 },
    [CFG_EV_TOL]    = { "ev_tol",    "tolerancia de vazao (%)",       30,   1,   500, 30 },
    [CFG_DRIFT_MM]  = { "drift_mm",  "deriva minima (mm)",            5,    1,   200, 5 },
};

#define WCFG(i)  (wash_cfg[i].val)

// Teste de bancada do log de lavagens executado no boot (1 = ativo). Deve
// ficar em 0 em builds de produção.
#define WASHLOG_BOOT_TEST        0

#define WASHLOG_MAGIC            0x4654574Cu    // "FTWL"
#define WASHLOG_VERSION          2u             // v2: type/count/aux (eventos)
#define WASHLOG_DATA_OFFSET      16

#define WASHREC_TYPE_WASH        0u    // resumo da lavagem (a=antes, b=depois)
#define WASHREC_TYPE_EVENT       1u    // evento agregado (a=nível mín, b=máx)
#define STORAGE_NVS_KEY_SAND     "sandmode"
#define STORAGE_NVS_KEY_BASE     "baseline"
#define STORAGE_NVS_KEY_BOOT     "bootcnt"
#define STORAGE_NVS_KEY_CAL      "calppm"

typedef struct __attribute__((packed)) {
    uint32_t seq;        // 0xFFFFFFFF = slot vazio (flash apagada)
    uint32_t uptime_s;   // segundos desde o boot no fim do registro
    int16_t  a_mm;       // lavagem: nível antes | evento: nível mínimo
    int16_t  b_mm;       // lavagem: nível depois | evento: nível máximo
    uint16_t boot;       // contador de boots, para o app separar as épocas
    uint8_t  type;       // WASHREC_TYPE_*
    uint8_t  count;      // lavagem: nº de eventos | evento: nº de excursões
    uint16_t aux;        // lavagem: duração (s) | evento: vazão média (mm/min)
    uint16_t crc;        // soma dos bytes anteriores
} wash_record_t;
_Static_assert(sizeof(wash_record_t) == 20, "wash_record_t deve ter 20 bytes");

static volatile bool sand_mode = false;
static volatile bool washlog_busy = false;  // dump ou apagamento em andamento
static const esp_partition_t *washlog_part = NULL;
static uint32_t washlog_count = 0;
static uint16_t boot_count = 0;
static int32_t sand_baseline_mm = -1;       // <0 = ainda sem baseline

static int16_t sand_win[SAND_WINDOW_MAX];
static int     sand_win_count = 0;
static int64_t sand_win_start_us = 0;

// Pré-filtro de entrada: as decisões (baseline, disparo, eventos, janelas)
// consomem UMA mediana por segundo, não amostras cruas — ecos absurdos do
// ultrassônico (ex.: 24 cm no meio de leituras de 111 cm) são descartados.
#define SAND_PRE_MAX        16
#define SAND_PRE_WINDOW_US  1000000LL
static int16_t sand_pre[SAND_PRE_MAX];
static int     sand_pre_n = 0;
static int64_t sand_pre_start_us = 0;

// Estado da lavagem em andamento
static volatile bool sand_washing = false;
static int32_t wash_before_mm = -1;
static int64_t wash_start_us = 0;
static int64_t wash_exceed_since_us = 0;   // início do desvio sustentado
static int64_t wash_cooldown_until_us = 0;
static int16_t wash_last_median_mm = -1;
static int64_t wash_stable_since_us = 0;   // início da estabilidade contínua
static uint32_t wash_event_count = 0;      // eventos agregados já gravados

// Evento dentro da lavagem: o primeiro abre junto com a lavagem e cada um
// continua até o nível se estabilizar por ev_end_s; quando o nível volta a se
// mover (>= ev_mm do ponto de repouso) abre o próximo.
static bool    ev_active = false;
static int64_t ev_start_us = 0;
static int32_t ev_min_mm = 0, ev_max_mm = 0;
static int32_t ev_still_mm = 0;            // referência de imobilidade
static int64_t ev_still_since_us = 0;

// Grupo de eventos em agregação (eventos com vazão parecida)
static bool    evg_open = false;
static int32_t evg_min_mm = 0, evg_max_mm = 0;
static int64_t evg_start_us = 0, evg_end_us = 0;
static float   evg_rate_mm_min = 0;        // média corrente das vazões
static uint32_t evg_count = 0;

// ─── Calibração de inclinação do sensor ────────────────────────────────────
// Se o sensor estiver inclinado em relação à vertical, a distância medida sai
// maior que a real (medida = real / cos θ). O app envia a distância real
// conhecida ("CAL:SET:<cm>") com o nível parado (filtro sem lavagem em curso);
// o fator real/medida (= cos θ) fica em NVS e corrige todas as leituras.
#define CAL_FACTOR_MIN      0.50f   // inclinação de até ~60° — além disso é erro
#define CAL_FACTOR_MAX      1.05f   // tolerância de ruído; acima disso é erro
#define CAL_RAW_MAX_AGE_US  (5LL * 1000 * 1000)

static volatile float   cal_factor = 1.0f;
static volatile float   cal_last_raw_cm = -1.0f;   // última medida bruta válida
static volatile int64_t cal_last_raw_us = 0;

static const esp_partition_t *washlog_partition(void)
{
    if (washlog_part == NULL) {
        washlog_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                                ESP_PARTITION_SUBTYPE_ANY,
                                                STORAGE_PARTITION_LABEL);
    }
    return washlog_part;
}

static uint32_t washlog_pending_count(void)
{
    return washlog_count;
}

static bool washlog_is_busy(void)
{
    return washlog_busy;
}

static uint16_t washlog_record_crc(const wash_record_t *r)
{
    const uint8_t *b = (const uint8_t *)r;
    uint16_t sum = 0;
    for (size_t i = 0; i < offsetof(wash_record_t, crc); i++) {
        sum += b[i];
    }
    return sum;
}

static bool washlog_erase_all(void)
{
    const esp_partition_t *part = washlog_partition();
    if (part == NULL) {
        return false;
    }
    for (uint32_t off = 0; off < part->size; off += 0x10000) {
        uint32_t len = (part->size - off) < 0x10000 ? (part->size - off) : 0x10000;
        if (esp_partition_erase_range(part, off, len) != ESP_OK) {
            return false;
        }
        vTaskDelay(1);
    }
    uint32_t hdr[2] = { WASHLOG_MAGIC, WASHLOG_VERSION };
    if (esp_partition_write(part, 0, hdr, sizeof(hdr)) != ESP_OK) {
        return false;
    }
    washlog_count = 0;
    return true;
}

// Descobre quantos registros válidos existem na flash. Se a partição não tem
// o cabeçalho do log (primeiro uso, ou logo após o self-test destrutivo),
// apaga e formata.
static void washlog_init(void)
{
    const esp_partition_t *part = washlog_partition();
    if (part == NULL) {
        ESP_LOGE(TAG, "Washlog: particao '%s' nao encontrada", STORAGE_PARTITION_LABEL);
        return;
    }

    uint32_t hdr[2] = {0};
    if (esp_partition_read(part, 0, hdr, sizeof(hdr)) != ESP_OK) {
        return;
    }
    if (hdr[0] != WASHLOG_MAGIC || hdr[1] != WASHLOG_VERSION) {
        ESP_LOGW(TAG, "Washlog: particao sem formato; formatando");
        if (!washlog_erase_all()) {
            ESP_LOGE(TAG, "Washlog: falha ao formatar");
        }
        return;
    }

    uint8_t *buf = malloc(4096);
    if (buf == NULL) {
        return;
    }
    washlog_count = 0;
    bool end = false;
    for (uint32_t off = WASHLOG_DATA_OFFSET; off < part->size && !end; ) {
        uint32_t chunk = 4096;
        if (off + chunk > part->size) {
            chunk = part->size - off;
        }
        if (esp_partition_read(part, off, buf, chunk) != ESP_OK) {
            break;
        }
        for (uint32_t i = 0; i + sizeof(wash_record_t) <= chunk;
             i += sizeof(wash_record_t)) {
            const wash_record_t *r = (const wash_record_t *)(buf + i);
            if (r->seq == 0xFFFFFFFFu || r->crc != washlog_record_crc(r)) {
                end = true;
                break;
            }
            washlog_count++;
        }
        off += chunk;
    }
    free(buf);
    ESP_LOGI(TAG, "Washlog: %lu lavagem(ns) pendente(s) na flash",
             (unsigned long)washlog_count);
}

static bool washlog_append(uint8_t type, int16_t a_mm, int16_t b_mm,
                           uint8_t count, uint16_t aux)
{
    const esp_partition_t *part = washlog_partition();
    if (part == NULL) {
        return false;
    }
    uint32_t capacity = (part->size - WASHLOG_DATA_OFFSET) / sizeof(wash_record_t);
    if (washlog_count >= capacity) {
        ESP_LOGE(TAG, "Washlog: particao cheia (%lu registros)",
                 (unsigned long)washlog_count);
        return false;
    }

    wash_record_t r = {
        .seq       = washlog_count + 1,
        .uptime_s  = (uint32_t)(esp_timer_get_time() / 1000000),
        .a_mm      = a_mm,
        .b_mm      = b_mm,
        .boot      = boot_count,
        .type      = type,
        .count     = count,
        .aux       = aux,
    };
    r.crc = washlog_record_crc(&r);

    uint32_t off = WASHLOG_DATA_OFFSET + washlog_count * sizeof(r);
    if (esp_partition_write(part, off, &r, sizeof(r)) != ESP_OK) {
        ESP_LOGE(TAG, "Washlog: falha ao gravar registro");
        return false;
    }
    washlog_count++;
    return true;
}

static void washlog_notify_count(void)
{
    char msg[48];
    snprintf(msg, sizeof(msg), "WL=CNT,%lu,%u,%lu",
             (unsigned long)washlog_count, (unsigned)boot_count,
             (unsigned long)(esp_timer_get_time() / 1000000));
    enviar_notificacao_ble(msg);
}

// Envia todos os registros pendentes para o app. Cada linha "WL=R,..." pode
// passar de 20 bytes e sair em mais de uma notificação — o BLEManager precisa
// reassemblar (as leituras de sensor ficam pausadas durante o dump).
static void washlog_dump_task(void *arg)
{
    const esp_partition_t *part = washlog_partition();
    bool was_connected = device_connected;
    washlog_notify_count();

    for (uint32_t i = 0; part != NULL && i < washlog_count; i++) {
        if (was_connected && !device_connected) {
            break;      // cliente caiu no meio do dump
        }
        wash_record_t r;
        if (esp_partition_read(part, WASHLOG_DATA_OFFSET + i * sizeof(r),
                               &r, sizeof(r)) != ESP_OK) {
            enviar_notificacao_ble("WL=ERR,read");
            break;
        }
        char msg[64];
        snprintf(msg, sizeof(msg), "WL=R,%u,%lu,%u,%d,%d,%u,%u",
                 (unsigned)r.boot, (unsigned long)r.uptime_s,
                 (unsigned)r.type, (int)r.a_mm, (int)r.b_mm,
                 (unsigned)r.count, (unsigned)r.aux);
        enviar_notificacao_ble(msg);
        vTaskDelay(pdMS_TO_TICKS(40));
    }

    enviar_notificacao_ble("WL=END");
    washlog_busy = false;
    vTaskDelete(NULL);
}

// Apagar 1344 KB leva >1 s: roda em task própria para não travar o callback
// do stack Bluetooth que recebeu o LOG:ACK.
static void washlog_erase_task(void *arg)
{
    bool ok = washlog_erase_all();
    ESP_LOGI(TAG, "Washlog: %s", ok ? "historico apagado apos ACK do app"
                                    : "falha ao apagar historico");
    enviar_notificacao_ble(ok ? "WL=CLR" : "WL=ERR,erase");
    washlog_busy = false;
    vTaskDelete(NULL);
}

static void washlog_handle_control(const char *data)
{
    if (strcmp(data, "LOG:COUNT") == 0) {
        washlog_notify_count();
    } else if (strcmp(data, "LOG:READ") == 0) {
        if (washlog_busy || storage_test_running) {
            enviar_notificacao_ble("WL=ERR,busy");
            return;
        }
        washlog_busy = true;
        if (xTaskCreate(washlog_dump_task, "wl_dump", 4096, NULL, 5, NULL)
                != pdPASS) {
            washlog_busy = false;
            enviar_notificacao_ble("WL=ERR,task");
        }
    } else if (strncmp(data, "LOG:ACK:", 8) == 0) {
        if (washlog_busy || storage_test_running) {
            enviar_notificacao_ble("WL=ERR,busy");
            return;
        }
        uint32_t n = (uint32_t)strtoul(data + 8, NULL, 10);
        if (n != washlog_count) {
            // Chegou lavagem nova depois do dump: o app precisa reler antes
            // de apagar, senão o registro novo seria perdido.
            enviar_notificacao_ble("WL=ERR,count");
            return;
        }
        if (washlog_count == 0) {
            enviar_notificacao_ble("WL=CLR");
            return;
        }
        washlog_busy = true;
        if (xTaskCreate(washlog_erase_task, "wl_erase", 4096, NULL, 5, NULL)
                != pdPASS) {
            washlog_busy = false;
            enviar_notificacao_ble("WL=ERR,task");
        }
    } else if (strcmp(data, "LOG:TEST") == 0) {
        // Registro sintético para testar o fluxo fim-a-fim sem mexer no sensor.
        int16_t before = (sand_baseline_mm >= 0) ? (int16_t)sand_baseline_mm : 400;
        int16_t after = before + 20;   // lavagem termina perto do nível original
        washlog_append(WASHREC_TYPE_EVENT, before, before + 120, 3, 90);
        if (washlog_append(WASHREC_TYPE_WASH, before, after, 1, 600)) {
            char msg[24];
            snprintf(msg, sizeof(msg), "WL=ADD,%lu", (unsigned long)washlog_count);
            enviar_notificacao_ble(msg);
        } else {
            enviar_notificacao_ble("WL=ERR,write");
        }
    } else {
        ESP_LOGW(TAG, "Comando LOG desconhecido: %s", data);
    }
}

static void sand_save_baseline(void)
{
    nvs_handle_t nvsh;
    if (nvs_open(STORAGE_NVS_NAMESPACE, NVS_READWRITE, &nvsh) == ESP_OK) {
        nvs_set_i32(nvsh, STORAGE_NVS_KEY_BASE, sand_baseline_mm);
        nvs_commit(nvsh);
        nvs_close(nvsh);
    }
}

static void sand_window_reset(void)
{
    sand_win_count = 0;
    sand_win_start_us = esp_timer_get_time();
}

// Fecha o grupo de eventos em agregação e grava um registro de evento.
static void sand_event_group_flush(void)
{
    if (!evg_open) {
        return;
    }
    evg_open = false;
    uint16_t rate = (uint16_t)(evg_rate_mm_min > 65535.0f ? 65535
                               : (evg_rate_mm_min < 0 ? 0 : evg_rate_mm_min + 0.5f));
    uint8_t n = evg_count > 255 ? 255 : (uint8_t)evg_count;
    if (washlog_append(WASHREC_TYPE_EVENT, (int16_t)evg_min_mm,
                       (int16_t)evg_max_mm, n, rate)) {
        wash_event_count++;
        ESP_LOGI(TAG, "Lavagem: evento agregado %ld..%ld mm, %lu excursoes, %u mm/min",
                 (long)evg_min_mm, (long)evg_max_mm,
                 (unsigned long)evg_count, (unsigned)rate);
        if (device_connected) {
            char msg[48];
            snprintf(msg, sizeof(msg), "WL=WEVT,%ld,%ld,%u,%lu",
                     (long)evg_min_mm, (long)evg_max_mm, (unsigned)rate,
                     (unsigned long)evg_count);
            enviar_notificacao_ble(msg);
        }
    }
}

// Um evento terminou: agrega no grupo atual se a vazão for parecida
// (±ev_tol%), senão fecha o grupo e abre outro.
static void sand_event_aggregate(int32_t lo, int32_t hi,
                                 int64_t de_us, int64_t ate_us, float rate)
{
    if (evg_open) {
        float tol = evg_rate_mm_min * (float)WCFG(CFG_EV_TOL) / 100.0f;
        if (fabsf(rate - evg_rate_mm_min) <= tol) {
            if (lo < evg_min_mm) evg_min_mm = lo;
            if (hi > evg_max_mm) evg_max_mm = hi;
            evg_end_us = ate_us;
            evg_rate_mm_min += (rate - evg_rate_mm_min) / (float)(evg_count + 1);
            evg_count++;
            return;
        }
        sand_event_group_flush();
    }
    evg_open = true;
    evg_min_mm = lo;
    evg_max_mm = hi;
    evg_start_us = de_us;
    evg_end_us = ate_us;
    evg_rate_mm_min = rate;
    evg_count = 1;
}

static void sand_event_open(int32_t mm, int64_t agora)
{
    ev_active = true;
    ev_start_us = agora;
    ev_min_mm = mm;
    ev_max_mm = mm;
    ev_still_mm = mm;
    ev_still_since_us = agora;
}

// Fecha o evento em andamento no instante em que o nível estabilizou.
static void sand_event_close(void)
{
    if (!ev_active) {
        return;
    }
    ev_active = false;
    float dur_s = (float)(ev_still_since_us - ev_start_us) / 1000000.0f;
    int32_t amp = ev_max_mm - ev_min_mm;
    if (amp < WCFG(CFG_EV_MM) || dur_s <= 0.1f) {
        return;     // não houve movimento real; descarta
    }
    float rate = (float)amp * 60.0f / dur_s;   // mm/min
    sand_event_aggregate(ev_min_mm, ev_max_mm, ev_start_us,
                         ev_still_since_us, rate);
}

// Alimentado por amostra durante a lavagem. O evento corrente acumula
// min/máx; a imobilidade (leituras dentro de stable_mm da referência) por
// ev_end_s encerra o evento. Com o nível parado, um movimento >= ev_mm em
// relação ao ponto de repouso abre o próximo evento.
static void sand_event_feed(int32_t mm, int64_t agora)
{
    if (!ev_active) {
        if (labs((long)(mm - ev_still_mm)) >= WCFG(CFG_EV_MM)) {
            sand_event_open(mm, agora);
        }
        return;
    }
    if (mm < ev_min_mm) ev_min_mm = mm;
    if (mm > ev_max_mm) ev_max_mm = mm;
    if (labs((long)(mm - ev_still_mm)) > WCFG(CFG_STABLE_MM)) {
        ev_still_mm = mm;
        ev_still_since_us = agora;
    } else if ((agora - ev_still_since_us) >=
               (int64_t)WCFG(CFG_EV_END_S) * 1000000LL) {
        sand_event_close();
    }
}

static void sand_wash_begin(int32_t mm, int64_t agora)
{
    sand_washing = true;
    wash_before_mm = sand_baseline_mm;
    wash_start_us = agora;
    wash_exceed_since_us = 0;
    wash_stable_since_us = 0;
    wash_last_median_mm = -1;
    wash_event_count = 0;
    evg_open = false;
    // O primeiro evento começa automaticamente junto com a lavagem.
    sand_event_open(mm, agora);
    ESP_LOGI(TAG, "Filtro fixo: LAVAGEM iniciada (baseline %ld mm)",
             (long)sand_baseline_mm);
    if (device_connected) {
        enviar_notificacao_ble("WL=WSTART");
    }
}

static void sand_wash_finish(int16_t after_mm, int64_t agora, const char *como)
{
    // Fecha o evento em andamento e o grupo aberto.
    sand_event_close();
    sand_event_group_flush();

    uint32_t dur_s = (uint32_t)((agora - wash_start_us) / 1000000LL);
    ESP_LOGI(TAG, "Filtro fixo: LAVAGEM encerrada (%s) %ld -> %d mm, %lu evento(s), %lus",
             como, (long)wash_before_mm, (int)after_mm,
             (unsigned long)wash_event_count, (unsigned long)dur_s);
    uint8_t n = wash_event_count > 255 ? 255 : (uint8_t)wash_event_count;
    if (washlog_append(WASHREC_TYPE_WASH, (int16_t)wash_before_mm, after_mm, n,
                       (uint16_t)(dur_s > 65535 ? 65535 : dur_s))) {
        if (device_connected) {
            char msg[40];
            snprintf(msg, sizeof(msg), "WL=EVT,%ld,%d,%lu",
                     (long)wash_before_mm, (int)after_mm,
                     (unsigned long)wash_event_count);
            enviar_notificacao_ble(msg);
        }
    } else {
        ESP_LOGE(TAG, "Filtro fixo: lavagem NAO gravada");
    }
    sand_baseline_mm = after_mm;
    sand_save_baseline();
    sand_washing = false;
    wash_cooldown_until_us = agora + (int64_t)WCFG(CFG_COOL_S) * 1000000LL;
}

// Recebe cada leitura válida do loop principal.
//
// Fora de lavagem: qualquer desvio do baseline maior que start_mm — nas DUAS
// direções (lavagem ascendente ou descendente) — sustentado por start_s abre
// uma lavagem (WL=WSTART). Janelas estáveis apenas acompanham a deriva lenta
// do baseline (persistido em NVS).
//
// Em lavagem: cada excursão máx↔mín do nível com amplitude >= ev_mm vira uma
// excursão; excursões consecutivas com vazão parecida (±ev_tol%) são
// agregadas num evento, gravado na flash quando o grupo fecha. A lavagem
// encerra quando o nível fica estável por end_s contínuos (após min_s; à
// força em max_s), gravando o registro-resumo e armando a trava cool_s.
// Todos os fatores vêm da tabela wash_cfg (ajustável por BLE "CFG:").
static void sand_feed_sample(float dist_cm)
{
    if (dist_cm < SAND_MIN_VALID_CM) {
        return;
    }
    int64_t agora = esp_timer_get_time();

    // Mediana de 1 s: acumula as leituras e só segue com o valor central.
    if (sand_pre_n == 0) {
        sand_pre_start_us = agora;
    }
    if (sand_pre_n < SAND_PRE_MAX) {
        sand_pre[sand_pre_n++] = (int16_t)(dist_cm * 10.0f);
    }
    if ((agora - sand_pre_start_us) < SAND_PRE_WINDOW_US) {
        return;
    }
    for (int i = 1; i < sand_pre_n; i++) {
        int16_t v = sand_pre[i];
        int j = i - 1;
        while (j >= 0 && sand_pre[j] > v) {
            sand_pre[j + 1] = sand_pre[j];
            j--;
        }
        sand_pre[j + 1] = v;
    }
    int32_t mm = sand_pre[sand_pre_n / 2];
    sand_pre_n = 0;

    if (!sand_washing && sand_baseline_mm >= 0 &&
        agora >= wash_cooldown_until_us) {
        if (labs((long)(mm - sand_baseline_mm)) > WCFG(CFG_START_MM)) {
            if (wash_exceed_since_us == 0) {
                wash_exceed_since_us = agora;
            } else if ((agora - wash_exceed_since_us) >=
                       (int64_t)WCFG(CFG_START_S) * 1000000LL) {
                sand_wash_begin(mm, agora);
                sand_window_reset();
            }
        } else {
            wash_exceed_since_us = 0;
        }
    }

    if (sand_washing) {
        sand_event_feed(mm, agora);
    }

    if (sand_win_count < SAND_WINDOW_MAX) {
        sand_win[sand_win_count++] = (int16_t)mm;
    }
    if ((agora - sand_win_start_us) < (int64_t)WCFG(CFG_STABLE_S) * 1000000LL) {
        return;
    }

    bool estavel = false;
    int16_t mediana = -1;
    if (sand_win_count >= SAND_WINDOW_MIN) {
        // Ordena a janela (insertion sort; no máximo 64 valores).
        for (int i = 1; i < sand_win_count; i++) {
            int16_t v = sand_win[i];
            int j = i - 1;
            while (j >= 0 && sand_win[j] > v) {
                sand_win[j + 1] = sand_win[j];
                j--;
            }
            sand_win[j + 1] = v;
        }
        mediana = sand_win[sand_win_count / 2];
        int16_t faixa = sand_win[sand_win_count - 1] - sand_win[0];
        estavel = faixa <= WCFG(CFG_STABLE_MM);
    }

    if (!sand_washing) {
        if (estavel) {
            if (sand_baseline_mm < 0) {
                sand_baseline_mm = mediana;
                sand_save_baseline();
                ESP_LOGI(TAG, "Filtro fixo: baseline inicial %d mm", (int)mediana);
            } else if (labs((long)(mediana - sand_baseline_mm)) > WCFG(CFG_START_MM) &&
                       agora >= wash_cooldown_until_us) {
                // Mudança lenta demais para o gatilho por amostra: um nível
                // estável já longe do baseline também abre uma lavagem.
                sand_wash_begin(mediana, agora);
            } else if (labs((long)(mediana - sand_baseline_mm))
                           > WCFG(CFG_DRIFT_MM)) {
                sand_baseline_mm = mediana;
                sand_save_baseline();
            }
        }
    } else {
        int64_t decorrido_s = (agora - wash_start_us) / 1000000LL;
        // "Estável por muito tempo": janelas estáveis consecutivas com
        // medianas próximas acumulam tempo em wash_stable_since_us.
        if (estavel &&
            (wash_stable_since_us == 0 ||
             abs((int)mediana - (int)wash_last_median_mm) <= WCFG(CFG_STABLE_MM))) {
            if (wash_stable_since_us == 0) {
                wash_stable_since_us = sand_win_start_us;
            }
            wash_last_median_mm = mediana;
        } else {
            wash_stable_since_us = 0;
            if (estavel) {
                wash_last_median_mm = mediana;
                wash_stable_since_us = sand_win_start_us;
            }
        }
        int64_t estavel_s = wash_stable_since_us > 0
            ? (agora - wash_stable_since_us) / 1000000LL : 0;
        if (decorrido_s >= WCFG(CFG_MIN_S) && estavel_s >= WCFG(CFG_END_S)) {
            sand_wash_finish(wash_last_median_mm, agora, "nivel estavel");
        } else if (decorrido_s >= WCFG(CFG_MAX_S)) {
            sand_wash_finish(mediana >= 0 ? mediana : (int16_t)mm, agora,
                             "timeout");
        }
    }
    sand_window_reset();
}

static void set_sand_mode(bool enable)
{
    if (sand_mode != enable) {
        sand_mode = enable;
        if (enable) {
            sand_window_reset();
        } else {
            // Aborta uma lavagem em andamento sem gravar registro.
            sand_washing = false;
            wash_exceed_since_us = 0;
            wash_stable_since_us = 0;
            evg_open = false;
            ev_active = false;
        }
        nvs_handle_t nvsh;
        if (nvs_open(STORAGE_NVS_NAMESPACE, NVS_READWRITE, &nvsh) == ESP_OK) {
            nvs_set_u8(nvsh, STORAGE_NVS_KEY_SAND, enable ? 1 : 0);
            nvs_commit(nvsh);
            nvs_close(nvsh);
        }
        ESP_LOGI(TAG, "Modo filtro fixo %s", enable ? "ativado" : "desativado");
    }
    enviar_notificacao_ble(sand_mode ? "SAND=1" : "SAND=0");
}

// ─── Configuração da detecção de lavagem (CFG:) ───────────────────────────
static void wash_cfg_save(int idx)
{
    nvs_handle_t nvsh;
    if (nvs_open(STORAGE_NVS_NAMESPACE, NVS_READWRITE, &nvsh) == ESP_OK) {
        nvs_set_i32(nvsh, wash_cfg[idx].key, wash_cfg[idx].val);
        nvs_commit(nvsh);
        nvs_close(nvsh);
    }
}

static void wash_cfg_load(void)
{
    nvs_handle_t nvsh;
    if (nvs_open(STORAGE_NVS_NAMESPACE, NVS_READONLY, &nvsh) != ESP_OK) {
        return;
    }
    for (int i = 0; i < CFG_COUNT; i++) {
        int32_t v = wash_cfg[i].def;
        if (nvs_get_i32(nvsh, wash_cfg[i].key, &v) == ESP_OK &&
            v >= wash_cfg[i].min && v <= wash_cfg[i].max) {
            wash_cfg[i].val = v;
        }
    }
    nvs_close(nvsh);
}

// Envia a configuração inteira: uma notificação por item, terminando em
// CFG=END. Roda em task própria (mesma razão do dump do washlog: várias
// notificações com pausa não podem segurar o callback do stack BLE).
static void wash_cfg_dump_task(void *arg)
{
    for (int i = 0; i < CFG_COUNT; i++) {
        char msg[96];
        snprintf(msg, sizeof(msg), "CFG=%s,%ld,%ld,%ld,%ld,%s",
                 wash_cfg[i].key, (long)wash_cfg[i].val, (long)wash_cfg[i].def,
                 (long)wash_cfg[i].min, (long)wash_cfg[i].max, wash_cfg[i].desc);
        enviar_notificacao_ble(msg);
        vTaskDelay(pdMS_TO_TICKS(40));
    }
    enviar_notificacao_ble("CFG=END");
    vTaskDelete(NULL);
}

static void wash_cfg_handle_control(const char *data)
{
    if (strcmp(data, "CFG:GET") == 0) {
        if (xTaskCreate(wash_cfg_dump_task, "cfg_dump", 3072, NULL, 5, NULL)
                != pdPASS) {
            enviar_notificacao_ble("CFG=ERR,task");
        }
    } else if (strncmp(data, "CFG:SET:", 8) == 0) {
        const char *arg = data + 8;
        const char *sep = strchr(arg, ':');
        if (sep == NULL || sep == arg) {
            enviar_notificacao_ble("CFG=ERR,formato");
            return;
        }
        char *endptr = NULL;
        long v = strtol(sep + 1, &endptr, 10);
        if (endptr == sep + 1 || (endptr != NULL && *endptr != '\0')) {
            enviar_notificacao_ble("CFG=ERR,valor");
            return;
        }
        size_t klen = (size_t)(sep - arg);
        for (int i = 0; i < CFG_COUNT; i++) {
            if (strlen(wash_cfg[i].key) == klen &&
                strncmp(wash_cfg[i].key, arg, klen) == 0) {
                if (v < wash_cfg[i].min || v > wash_cfg[i].max) {
                    char msg[64];
                    snprintf(msg, sizeof(msg), "CFG=ERR,faixa,%s,%ld,%ld",
                             wash_cfg[i].key, (long)wash_cfg[i].min,
                             (long)wash_cfg[i].max);
                    enviar_notificacao_ble(msg);
                    return;
                }
                wash_cfg[i].val = (int32_t)v;
                wash_cfg_save(i);
                ESP_LOGI(TAG, "CFG: %s = %ld", wash_cfg[i].key, v);
                char msg[48];
                snprintf(msg, sizeof(msg), "CFG=OK,%s,%ld", wash_cfg[i].key, v);
                enviar_notificacao_ble(msg);
                return;
            }
        }
        enviar_notificacao_ble("CFG=ERR,chave");
    } else if (strcmp(data, "CFG:RESET") == 0) {
        for (int i = 0; i < CFG_COUNT; i++) {
            wash_cfg[i].val = wash_cfg[i].def;
            wash_cfg_save(i);
        }
        ESP_LOGI(TAG, "CFG: restaurado para os padroes");
        enviar_notificacao_ble("CFG=OK,reset,0");
    } else {
        ESP_LOGW(TAG, "Comando CFG desconhecido: %s", data);
    }
}

// ─── Calibração de inclinação ─────────────────────────────────────────────
static void cal_save(void)
{
    nvs_handle_t nvsh;
    if (nvs_open(STORAGE_NVS_NAMESPACE, NVS_READWRITE, &nvsh) == ESP_OK) {
        nvs_set_u32(nvsh, STORAGE_NVS_KEY_CAL,
                    (uint32_t)(cal_factor * 1000000.0f + 0.5f));
        nvs_commit(nvsh);
        nvs_close(nvsh);
    }
}

static float cal_angle_deg(void)
{
    float f = cal_factor;
    if (f >= 1.0f) {
        return 0.0f;
    }
    return acosf(f) * (180.0f / (float)M_PI);
}

static void cal_notify(bool ok_prefix)
{
    char msg[40];
    snprintf(msg, sizeof(msg), "CAL=%s%.4f,%.1f",
             ok_prefix ? "OK," : "", (double)cal_factor, (double)cal_angle_deg());
    enviar_notificacao_ble(msg);
}

// Aplica um novo fator e reescala o baseline do modo areia, que foi medido com
// o fator antigo — sem isso a troca de calibração pareceria uma lavagem.
static void cal_apply_factor(float novo)
{
    float antigo = cal_factor;
    cal_factor = novo;
    cal_save();
    if (sand_baseline_mm >= 0 && antigo > 0.0f) {
        sand_baseline_mm = (int32_t)((float)sand_baseline_mm * (novo / antigo) + 0.5f);
        sand_save_baseline();
    }
    sand_window_reset();
    ESP_LOGI(TAG, "Calibracao: fator %.4f (inclinacao %.1f graus)",
             (double)cal_factor, (double)cal_angle_deg());
}

static void cal_handle_control(const char *data)
{
    if (strncmp(data, "CAL:SET:", 8) == 0) {
        const char *arg = data + 8;
        char *endptr = NULL;
        float real_cm = strtof(arg, &endptr);
        if (endptr == arg || real_cm <= 0.0f) {
            enviar_notificacao_ble("CAL=ERR,valor");
            return;
        }
        float raw = cal_last_raw_cm;
        if (raw <= 0.0f ||
            (esp_timer_get_time() - cal_last_raw_us) > CAL_RAW_MAX_AGE_US) {
            enviar_notificacao_ble("CAL=ERR,semleitura");
            return;
        }
        float fator = real_cm / raw;
        if (fator < CAL_FACTOR_MIN || fator > CAL_FACTOR_MAX) {
            enviar_notificacao_ble("CAL=ERR,faixa");
            return;
        }
        if (fator > 1.0f) {
            fator = 1.0f;   // inclinação nunca encurta a medida; sobra é ruído
        }
        cal_apply_factor(fator);
        cal_notify(true);
    } else if (strcmp(data, "CAL:GET") == 0) {
        cal_notify(false);
    } else if (strcmp(data, "CAL:CLEAR") == 0) {
        cal_apply_factor(1.0f);
        cal_notify(true);
    } else {
        ESP_LOGW(TAG, "Comando CAL desconhecido: %s", data);
    }
}

// Carrega estado persistido (modo, baseline) e incrementa o contador de boots
// usado para datar os registros de lavagem.
static void sand_mode_boot_load(void)
{
    nvs_handle_t nvsh;
    if (nvs_open(STORAGE_NVS_NAMESPACE, NVS_READWRITE, &nvsh) != ESP_OK) {
        boot_count = 1;
        return;
    }
    uint16_t bc = 0;
    nvs_get_u16(nvsh, STORAGE_NVS_KEY_BOOT, &bc);
    boot_count = bc + 1;
    nvs_set_u16(nvsh, STORAGE_NVS_KEY_BOOT, boot_count);

    uint8_t sm = 0;
    nvs_get_u8(nvsh, STORAGE_NVS_KEY_SAND, &sm);
    int32_t base = -1;
    nvs_get_i32(nvsh, STORAGE_NVS_KEY_BASE, &base);
    uint32_t calppm = 1000000;
    nvs_get_u32(nvsh, STORAGE_NVS_KEY_CAL, &calppm);
    nvs_commit(nvsh);
    nvs_close(nvsh);

    if (calppm >= (uint32_t)(CAL_FACTOR_MIN * 1000000.0f) && calppm <= 1000000) {
        cal_factor = (float)calppm / 1000000.0f;
        if (calppm != 1000000) {
            ESP_LOGI(TAG, "Calibracao carregada: fator %.4f (inclinacao %.1f graus)",
                     (double)cal_factor, (double)cal_angle_deg());
        }
    }

    wash_cfg_load();

    sand_baseline_mm = base;
    if (sm) {
        sand_mode = true;
        sand_window_reset();
        ESP_LOGI(TAG, "Modo filtro fixo ativo (boot %u, baseline %ld mm)",
                 (unsigned)boot_count, (long)sand_baseline_mm);
    }
}

static void tratar_comando_bluetooth(int comando)
{
    switch (comando) {
    case 0:
        ESP_LOGW(TAG, "Comando 0 recebido: reiniciando ESP32-C3");
        esp_restart();
        break;

    case 1:
        if (!low_power_mode) {
            set_low_power_mode(true);
            ESP_LOGI(TAG, "Comando 1 recebido: modo baixo consumo ativado");
        } else {
            ESP_LOGI(TAG, "Modo baixo consumo ja estava ativo");
        }
        break;

    case 2:
        if (low_power_mode) {
            set_low_power_mode(false);
            ESP_LOGI(TAG, "Comando 2 recebido: modo baixo consumo desativado");
        } else {
            ESP_LOGI(TAG, "Modo baixo consumo ja estava desativado");
        }
        break;

    case 3: {
        char ver_msg[48];
        snprintf(ver_msg, sizeof(ver_msg), "VER=%s",
                 esp_app_get_description()->version);
        enviar_notificacao_ble(ver_msg);
        break;
    }

    case 4:
        ESP_LOGI(TAG, "Comando 4 recebido: self-test da particao de armazenamento");
        storage_selftest_start("comando BLE");
        break;

    case 5:
        ESP_LOGI(TAG, "Comando 5 recebido: ativar modo filtro fixo");
        set_sand_mode(true);
        break;

    case 6:
        ESP_LOGI(TAG, "Comando 6 recebido: desativar modo filtro fixo");
        set_sand_mode(false);
        break;

    default:
        ESP_LOGW(TAG, "Comando bluetooth invalido: %d", comando);
        break;
    }
}

// ─── OTA ──────────────────────────────────────────────────────────────────
// Grava o firmware recebido pelo BLE na partição OTA inativa. Roda em task
// própria alimentada por um ring buffer: as escritas em flash bloqueiam por
// vários ms e não podem acontecer dentro do callback do stack Bluetooth.
static void ota_task(void *arg)
{
    const esp_partition_t *update_part = esp_ota_get_next_update_partition(NULL);
    esp_ota_handle_t handle = 0;
    uint32_t received = 0;
    int last_pct = 0;

    if (update_part == NULL || ota_expected_size == 0 ||
        ota_expected_size > update_part->size) {
        ESP_LOGE(TAG, "OTA: particao invalida ou tamanho excede o slot");
        enviar_notificacao_ble("OTA=ERRO,part");
        goto cleanup;
    }

    // Erase incremental durante as escritas: apagar o slot inteiro de uma vez
    // travaria a task por segundos logo no começo.
    esp_err_t err = esp_ota_begin(update_part, OTA_WITH_SEQUENTIAL_WRITES, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA: esp_ota_begin falhou: %s", esp_err_to_name(err));
        enviar_notificacao_ble("OTA=ERRO,begin");
        goto cleanup;
    }

    ESP_LOGI(TAG, "OTA iniciado: %lu bytes para %s",
             (unsigned long)ota_expected_size, update_part->label);
    enviar_notificacao_ble("OTA=READY");

    int64_t last_data_us = esp_timer_get_time();
    while (received < ota_expected_size) {
        if (ota_abort_request || !device_connected) {
            esp_ota_abort(handle);
            ESP_LOGW(TAG, "OTA abortado (%s)",
                     ota_abort_request ? "pedido do app" : "desconexao");
            enviar_notificacao_ble("OTA=ABORTED");
            goto cleanup;
        }

        size_t len = 0;
        uint8_t *chunk = (uint8_t *)xRingbufferReceiveUpTo(
            ota_ringbuf, &len, pdMS_TO_TICKS(500), 4096);
        if (chunk == NULL) {
            if ((esp_timer_get_time() - last_data_us) > OTA_RECV_TIMEOUT_US) {
                esp_ota_abort(handle);
                ESP_LOGE(TAG, "OTA: timeout aguardando dados (%lu/%lu bytes)",
                         (unsigned long)received, (unsigned long)ota_expected_size);
                enviar_notificacao_ble("OTA=ERRO,timeout");
                goto cleanup;
            }
            continue;
        }
        last_data_us = esp_timer_get_time();

        err = esp_ota_write(handle, chunk, len);
        vRingbufferReturnItem(ota_ringbuf, chunk);
        if (err != ESP_OK) {
            esp_ota_abort(handle);
            ESP_LOGE(TAG, "OTA: esp_ota_write falhou: %s", esp_err_to_name(err));
            enviar_notificacao_ble("OTA=ERRO,write");
            goto cleanup;
        }

        received += len;
        int pct = (int)((uint64_t)received * 100 / ota_expected_size);
        if (pct >= last_pct + 10) {
            last_pct = pct;
            char prog[24];
            snprintf(prog, sizeof(prog), "OTA=PROG,%d", pct);
            enviar_notificacao_ble(prog);
        }
    }

    err = esp_ota_end(handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA: imagem invalida: %s", esp_err_to_name(err));
        enviar_notificacao_ble("OTA=ERRO,verify");
        goto cleanup;
    }

    err = esp_ota_set_boot_partition(update_part);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA: falha ao definir boot: %s", esp_err_to_name(err));
        enviar_notificacao_ble("OTA=ERRO,boot");
        goto cleanup;
    }

    ESP_LOGI(TAG, "OTA concluido (%lu bytes); reiniciando", (unsigned long)received);
    enviar_notificacao_ble("OTA=OK");
    vTaskDelay(pdMS_TO_TICKS(1500));    // deixa a notificação sair antes do reset
    esp_restart();

cleanup:
    ota_in_progress = false;
    ota_abort_request = false;
    vTaskDelete(NULL);
}

static void ota_handle_control(const char *data)
{
    if (strncmp(data, "OTA:BEGIN:", 10) == 0) {
        if (ota_in_progress || storage_test_running) {
            enviar_notificacao_ble("OTA=ERRO,busy");
            return;
        }

        uint32_t size = (uint32_t)strtoul(data + 10, NULL, 10);
        if (size == 0) {
            enviar_notificacao_ble("OTA=ERRO,size");
            return;
        }

        // Criado uma vez e mantido: deletar o buffer poderia correr contra um
        // write BLE em andamento no callback do GATT.
        if (ota_ringbuf == NULL) {
            ota_ringbuf = xRingbufferCreate(OTA_RINGBUF_SIZE, RINGBUF_TYPE_BYTEBUF);
            if (ota_ringbuf == NULL) {
                enviar_notificacao_ble("OTA=ERRO,mem");
                return;
            }
        }

        // Descarta restos de uma tentativa anterior que falhou no meio.
        size_t stale_len = 0;
        void *stale;
        while ((stale = xRingbufferReceiveUpTo(ota_ringbuf, &stale_len, 0,
                                               OTA_RINGBUF_SIZE)) != NULL) {
            vRingbufferReturnItem(ota_ringbuf, stale);
        }

        ota_expected_size = size;
        ota_abort_request = false;
        ota_in_progress = true;
        if (xTaskCreate(ota_task, "ota", 4096, NULL, 5, NULL) != pdPASS) {
            ota_in_progress = false;
            enviar_notificacao_ble("OTA=ERRO,task");
        }
    } else if (strcmp(data, "OTA:ABORT") == 0) {
        if (ota_in_progress) {
            ota_abort_request = true;
        }
    } else {
        ESP_LOGW(TAG, "Comando OTA desconhecido: %s", data);
    }
}

// Dispatcha um comando em texto vindo do BLE (0xFF01) ou do console serial.
static void processar_comando_texto(const char *data)
{
    if (strncmp(data, "OTA:", 4) == 0) {
        ota_handle_control(data);
        return;
    }
    if (strncmp(data, "LOG:", 4) == 0) {
        washlog_handle_control(data);
        return;
    }
    if (strncmp(data, "CAL:", 4) == 0) {
        cal_handle_control(data);
        return;
    }
    if (strncmp(data, "CFG:", 4) == 0) {
        wash_cfg_handle_control(data);
        return;
    }

    char *endptr = NULL;
    long comando = strtol(data, &endptr, 10);
    while ((endptr != NULL) && (*endptr != '\0') && isspace((unsigned char)*endptr)) {
        endptr++;
    }

    if ((endptr == data) || ((endptr != NULL) && (*endptr != '\0'))) {
        ESP_LOGW(TAG, "Comando nao numerico recebido: %s", data);
    } else {
        tratar_comando_bluetooth((int)comando);
    }
}

// ─── GAP ──────────────────────────────────────────────────────────────────
static void gap_event_handler(esp_gap_ble_cb_event_t event,
                              esp_ble_gap_cb_param_t *param)
{
    switch (event) {
    case ESP_GAP_BLE_ADV_DATA_SET_COMPLETE_EVT:
    case ESP_GAP_BLE_SCAN_RSP_DATA_SET_COMPLETE_EVT:
        if (!adv_config_done) {
            adv_config_done = true;
            ESP_LOGI(TAG, "ADV configurado, iniciando advertising...");
            iniciar_advertising();
        }
        break;

    case ESP_GAP_BLE_ADV_START_COMPLETE_EVT:
        if (param->adv_start_cmpl.status == ESP_BT_STATUS_SUCCESS) {
            bluetooth_error = false;
            ble_advertising = true;
            ESP_LOGI(TAG, "Advertising iniciado");
        } else {
            bluetooth_error = true;
            ble_advertising = false;
            ESP_LOGE(TAG, "Falha ao iniciar advertising (status=%d)",
                     (int)param->adv_start_cmpl.status);
        }
        break;

    default:
        break;
    }
}

// ─── GATT ─────────────────────────────────────────────────────────────────
static void gatts_event_handler(esp_gatts_cb_event_t event,
                                esp_gatt_if_t gatts_if,
                                esp_ble_gatts_cb_param_t *param)
{
    switch (event) {

    case ESP_GATTS_REG_EVT: {
        ESP_LOGI(TAG, "Registrado no GATT");
        esp_ble_gap_set_device_name("FilterTrackV3");

        static uint16_t service_uuid = SERVICE_UUID;

        esp_ble_adv_data_t adv_data = {
            .set_scan_rsp    = false,
            .include_name    = true,
            .include_txpower = true,
            .service_uuid_len = sizeof(service_uuid),
            .p_service_uuid  = (uint8_t *)&service_uuid,
            .flag = ESP_BLE_ADV_FLAG_GEN_DISC | ESP_BLE_ADV_FLAG_BREDR_NOT_SPT,
        };
        esp_ble_adv_data_t scan_rsp_data = {
            .set_scan_rsp = true,
            .include_name = true,
        };
        esp_ble_gap_config_adv_data(&adv_data);
        esp_ble_gap_config_adv_data(&scan_rsp_data);

        esp_ble_gatts_create_service(gatts_if,
            &(esp_gatt_srvc_id_t){
                .is_primary      = true,
                .id.inst_id      = 0x00,
                .id.uuid.len     = ESP_UUID_LEN_16,
                .id.uuid.uuid.uuid16 = SERVICE_UUID,
            }, 8);
        break;
    }

    case ESP_GATTS_CREATE_EVT:
        ESP_LOGI(TAG, "Serviço criado");
        service_handle_global = param->create.service_handle;
        esp_ble_gatts_start_service(param->create.service_handle);
        esp_ble_gatts_add_char(param->create.service_handle,
            &(esp_bt_uuid_t){
                .len           = ESP_UUID_LEN_16,
                .uuid.uuid16   = CHARACTERISTIC_UUID,
            },
            ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE,
            ESP_GATT_CHAR_PROP_BIT_NOTIFY | ESP_GATT_CHAR_PROP_BIT_WRITE,
            NULL, NULL);
        break;

    case ESP_GATTS_ADD_CHAR_EVT:
        if (param->add_char.char_uuid.uuid.uuid16 == CHARACTERISTIC_UUID) {
            char_handle = param->add_char.attr_handle;
            ESP_LOGI(TAG, "Characteristic criada (handle=%d)", char_handle);
            // Characteristics são adicionadas em sequência: a de OTA só depois
            // da de comando/notificação existir.
            esp_ble_gatts_add_char(service_handle_global,
                &(esp_bt_uuid_t){
                    .len         = ESP_UUID_LEN_16,
                    .uuid.uuid16 = OTA_CHARACTERISTIC_UUID,
                },
                ESP_GATT_PERM_WRITE,
                ESP_GATT_CHAR_PROP_BIT_WRITE | ESP_GATT_CHAR_PROP_BIT_WRITE_NR,
                NULL, NULL);
        } else if (param->add_char.char_uuid.uuid.uuid16 == OTA_CHARACTERISTIC_UUID) {
            ota_char_handle = param->add_char.attr_handle;
            ESP_LOGI(TAG, "Characteristic OTA criada (handle=%d)", ota_char_handle);
        }
        break;

    case ESP_GATTS_WRITE_EVT: {
        if (param->write.need_rsp) {
            esp_ble_gatts_send_response(gatts_if,
                                        param->write.conn_id,
                                        param->write.trans_id,
                                        ESP_GATT_OK,
                                        NULL);
        }

        if (param->write.is_prep) {
            break;
        }

        // Chunks binários de firmware chegam na characteristic OTA e vão
        // direto para o ring buffer; a ota_task grava na flash.
        if (param->write.handle == ota_char_handle) {
            if (ota_in_progress && ota_ringbuf != NULL) {
                if (xRingbufferSend(ota_ringbuf, param->write.value,
                                    param->write.len,
                                    pdMS_TO_TICKS(2000)) != pdTRUE) {
                    ESP_LOGE(TAG, "OTA: ring buffer cheio; abortando");
                    ota_abort_request = true;
                }
            }
            break;
        }

        char data[32] = {0};
        size_t copy_len = param->write.len;
        if (copy_len > (sizeof(data) - 1)) {
            copy_len = sizeof(data) - 1;
        }
        memcpy(data, param->write.value, copy_len);
        data[copy_len] = '\0';
        ESP_LOGI(TAG, "Recebido do cliente: %s", data);

        processar_comando_texto(data);
        break;
    }

    case ESP_GATTS_CONNECT_EVT:
        ESP_LOGI(TAG, "Dispositivo conectado");
        device_connected  = true;
        bluetooth_error   = false;
        ble_advertising   = false;
        gatts_if_global   = gatts_if;
        conn_id_global    = param->connect.conn_id;
        break;

    case ESP_GATTS_DISCONNECT_EVT:
        ESP_LOGI(TAG, "Desconectado, reiniciando advertising...");
        device_connected = false;
        ble_advertising = false;
        iniciar_advertising();
        break;

    default:
        break;
    }
}

// ─── MAIN ─────────────────────────────────────────────────────────────────
void app_main(void)
{
    // ── GPIOs do sensor ──────────────────────────────────────────────────
    gpio_config_t io_conf = {
        .pin_bit_mask  = (1ULL << TRIG_PIN),
        .mode          = GPIO_MODE_OUTPUT,
        .pull_up_en    = GPIO_PULLUP_DISABLE,
        .pull_down_en  = GPIO_PULLDOWN_DISABLE,
        .intr_type     = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);

#if SENSOR_PWR_ENABLED
    io_conf.pin_bit_mask  = (1ULL << SENSOR_PWR_PIN);
    io_conf.mode          = GPIO_MODE_OUTPUT;
    io_conf.pull_up_en    = GPIO_PULLUP_DISABLE;
    io_conf.pull_down_en  = GPIO_PULLDOWN_DISABLE;
    gpio_config(&io_conf);
    set_sensor_power(true);
#endif

    io_conf.pin_bit_mask  = (1ULL << ECHO_PIN);
    io_conf.mode          = GPIO_MODE_INPUT;
    io_conf.pull_down_en  = GPIO_PULLDOWN_ENABLE;
    gpio_config(&io_conf);

    // ── GPIOs dos LEDs de status ─────────────────────────────────────────
    gpio_config_t led_conf = {
        .pin_bit_mask  = (1ULL << LED_RED_PIN) |
                         (1ULL << LED_GREEN_PIN) |
                         (1ULL << LED_YELLOW_PIN),
        .mode          = GPIO_MODE_OUTPUT,
        .pull_up_en    = GPIO_PULLUP_DISABLE,
        .pull_down_en  = GPIO_PULLDOWN_DISABLE,
        .intr_type     = GPIO_INTR_DISABLE,
    };
    gpio_config(&led_conf);
    gpio_set_level(LED_RED_PIN, 0);
    gpio_set_level(LED_GREEN_PIN, 0);
    gpio_set_level(LED_YELLOW_PIN, 0);

    // ── BLE ──────────────────────────────────────────────────────────────
    esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES ||
            nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS incompatível após migração; apagando e reinicializando");
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs_err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(nvs_err);

    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_bt_controller_init(&bt_cfg));
    ESP_ERROR_CHECK(esp_bt_controller_enable(ESP_BT_MODE_BLE));
    ESP_ERROR_CHECK(esp_bluedroid_init());
    ESP_ERROR_CHECK(esp_bluedroid_enable());

    esp_ble_gap_register_callback(gap_event_handler);
    esp_ble_gatts_register_callback(gatts_event_handler);
    esp_ble_gatts_app_register(0);
    // MTU alto para o OTA: chunks de até 514 bytes por write-no-response.
    esp_ble_gatt_set_local_mtu(517);

    // Com rollback habilitado, uma imagem recém-gravada por OTA boota como
    // "pending verify"; se não for marcada válida, o bootloader volta para a
    // imagem anterior no próximo reset. Chegar até aqui (BLE de pé) é o nosso
    // critério de boot saudável.
    const esp_partition_t *running_part = esp_ota_get_running_partition();
    esp_ota_img_states_t img_state;
    if (esp_ota_get_state_partition(running_part, &img_state) == ESP_OK &&
        img_state == ESP_OTA_IMG_PENDING_VERIFY) {
        esp_ota_mark_app_valid_cancel_rollback();
        ESP_LOGI(TAG, "Imagem OTA validada (rollback cancelado)");
    }

#if ACCEL_ENABLED
    accel_check_idle_pin_levels();

    esp_err_t ret = lsm303_i2c_master_init();
    if (ret != ESP_OK) {
        lsm303_error = true;
        ESP_LOGE(TAG, "Falha ao iniciar I2C do acelerometro: %s", esp_err_to_name(ret));
    } else {
        tentar_iniciar_acelerometro(true);
    }
#else
    // This SR04M-2 board has no accelerometer.
    lsm303_error = false;
#endif

    ESP_LOGI(TAG, "FilterTrack BLE pronto!");

    // Self-test da partição de armazenamento: roda uma única vez no primeiro
    // boot após o flash (marcador em NVS); depois só via comando BLE "4".
    // Fora do primeiro boot, inicializa o log de lavagens direto (o self-test
    // reformata o log ao terminar).
    {
        uint8_t tested = 0;
        nvs_handle_t nvsh;
        if (nvs_open(STORAGE_NVS_NAMESPACE, NVS_READONLY, &nvsh) == ESP_OK) {
            nvs_get_u8(nvsh, STORAGE_NVS_KEY_TESTED, &tested);
            nvs_close(nvsh);
        }
        if (!tested) {
            storage_selftest_start("primeiro boot");
        } else {
            washlog_init();
        }
    }
    sand_mode_boot_load();

#if WASHLOG_BOOT_TEST
    // Teste temporário do log de lavagens (fica atrás de flag de compilação;
    // desligar após validar). Roda em fases, uma por boot, para provar a
    // persistência em power-cycle real:
    //   fase 0: liga o modo filtro fixo e grava 2 lavagens sintéticas;
    //   fase 1: confere o que sobreviveu ao reset, faz dump, simula o
    //           LOG:ACK do app (apaga tudo) e desliga o modo;
    //   fase 2: só reporta o estado final (deve estar limpo).
    {
        uint8_t phase = 0;
        nvs_handle_t th;
        if (nvs_open(STORAGE_NVS_NAMESPACE, NVS_READWRITE, &th) == ESP_OK) {
            nvs_get_u8(th, "wltest", &phase);
            if (phase == 0) {
                ESP_LOGW(TAG, "WLTEST fase 0: count=%lu; ligando modo e gravando 2 lavagens",
                         (unsigned long)washlog_count);
                set_sand_mode(true);
                washlog_append(WASHREC_TYPE_WASH, 400, 455, 0, 60);
                washlog_append(WASHREC_TYPE_WASH, 455, 505, 0, 60);
                ESP_LOGW(TAG, "WLTEST fase 0: count agora %lu (esperado 2)",
                         (unsigned long)washlog_count);
                nvs_set_u8(th, "wltest", 1);
            } else if (phase == 1) {
                ESP_LOGW(TAG, "WLTEST fase 1: apos reset count=%lu (esperado 2), sand_mode=%d (esperado 1)",
                         (unsigned long)washlog_count, (int)sand_mode);
                const esp_partition_t *p = washlog_partition();
                for (uint32_t i = 0; p != NULL && i < washlog_count; i++) {
                    wash_record_t r;
                    if (esp_partition_read(p, WASHLOG_DATA_OFFSET + i * sizeof(r),
                                           &r, sizeof(r)) == ESP_OK) {
                        ESP_LOGW(TAG, "WLTEST registro %lu: boot=%u t=%lus tipo=%u %d->%d mm (crc %s)",
                                 (unsigned long)i, (unsigned)r.boot,
                                 (unsigned long)r.uptime_s, (unsigned)r.type,
                                 (int)r.a_mm, (int)r.b_mm,
                                 r.crc == washlog_record_crc(&r) ? "ok" : "RUIM");
                    }
                }
                bool ok = washlog_erase_all();
                set_sand_mode(false);
                ESP_LOGW(TAG, "WLTEST fase 1: ACK simulado, erase=%s, count=%lu (esperado 0)",
                         ok ? "ok" : "FALHOU", (unsigned long)washlog_count);
                nvs_set_u8(th, "wltest", 2);
            } else {
                ESP_LOGW(TAG, "WLTEST fase 2: estado final count=%lu sand_mode=%d (esperado 0/0)",
                         (unsigned long)washlog_count, (int)sand_mode);
            }
            nvs_commit(th);
            nvs_close(th);
        }
    }
#endif

    // ── Loop principal ────────────────────────────────────────────────────
    int64_t ultimo_envio_us = esp_timer_get_time();
#if ACCEL_ENABLED
    int64_t ultimo_retry_accel_us = 0;
#endif
    while (1) {
        int64_t agora = esp_timer_get_time();

        // ── Atualiza LEDs de status ──────────────────────────────────────
        atualizar_leds_status(!low_power_mode);

        if (ota_in_progress || storage_test_running || washlog_busy) {
            // Pausa leituras e notificações de sensor para não competir com a
            // transferência de firmware pelo link BLE, com o self-test de
            // armazenamento nem com o dump/apagamento do log de lavagens.
            ultimo_envio_us = agora;
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }

        if (low_power_mode) {
            // Reduz leitura do sensor para economizar energia, mantendo BLE ativo.
            gpio_set_level(TRIG_PIN, 0);
            ultimo_envio_us = agora;
            vTaskDelay(pdMS_TO_TICKS(LOW_POWER_INTERVAL_MS));
            continue;
        }

        vec3i16_t accel_raw = {0};
        bool raw_ok = false;

#if ACCEL_ENABLED
        if (lsm303_initialized) {
            raw_ok = ler_accel_raw(&accel_raw);
        } else if ((agora - ultimo_retry_accel_us) >= ACCEL_RETRY_INTERVAL_US) {
            ultimo_retry_accel_us = agora;
            tentar_iniciar_acelerometro(true);
        }
#endif

        float dist_bruta_cm = medir_distancia_cm();
        bool distancia_ok = dist_bruta_cm > 0.0f;
        // Fator de calibração (= cos da inclinação do sensor) corrige a medida
        // bruta; a bruta fica guardada para o comando CAL:SET recalibrar.
        float distancia_cm = dist_bruta_cm * cal_factor;
        agora = esp_timer_get_time();
        if (distancia_ok) {
            cal_last_raw_cm = dist_bruta_cm;
            cal_last_raw_us = agora;
        }

        if (sand_mode && distancia_ok) {
            sand_feed_sample(distancia_cm);
        }

        char msg[64];
        if (distancia_ok && raw_ok) {
            snprintf(
                msg,
                sizeof(msg),
                "DIST=%.2f;ACC_RAW=%d,%d,%d",
                distancia_cm,
                accel_raw.x,
                accel_raw.y,
                accel_raw.z
            );
        } else if (distancia_ok) {
            snprintf(msg, sizeof(msg), "DIST=%.2f;RAW=ERRO", distancia_cm);
        } else if (raw_ok) {
            snprintf(
                msg,
                sizeof(msg),
                "DIST=ERRO;ACC_RAW=%d,%d,%d",
                accel_raw.x,
                accel_raw.y,
                accel_raw.z
            );
        } else {
            snprintf(msg, sizeof(msg), "DIST=ERRO;RAW=ERRO");
        }

        ESP_LOGI(TAG, "%s", msg);

        if (device_connected) {
            enviar_medidas_ble(distancia_ok, distancia_cm, raw_ok, accel_raw);
        }

        if (distancia_ok) {
            sensor_error = false;
            ultimo_envio_us = agora;

        } else {
            // Watchdog: mais de 5 s sem leitura válida
            if ((agora - ultimo_envio_us) >= WATCHDOG_US) {
                sensor_error = true;
                const char *err = "ERRO_TIMEOUT";

                if (device_connected) {
                    enviar_notificacao_ble(err);
                }

                ESP_LOGW(TAG, "%s", err);
                ultimo_envio_us = agora; // evita spam
            }
        }

        // No modo filtro fixo sem app conectado não há para quem notificar a
        // 10 Hz; leituras a cada 2 s bastam para detectar lavagens.
        vTaskDelay(pdMS_TO_TICKS((sand_mode && !device_connected)
                                     ? SAND_SAMPLE_MS_OFFLINE
                                     : INTERVALO_MS));
    }
}
