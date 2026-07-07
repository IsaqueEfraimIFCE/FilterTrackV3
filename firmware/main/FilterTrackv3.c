#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <stdbool.h>

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
#include "esp_ota_ops.h"
#include "esp_app_desc.h"

#include "esp_bt.h"
#include "esp_gap_ble_api.h"
#include "esp_gatt_common_api.h"
#include "esp_gatts_api.h"
#include "esp_bt_main.h"

// ─── Sensor ultrassônico ───────────────────────────────────────────────────
#define TRIG_PIN        GPIO_NUM_1
#define ECHO_PIN        GPIO_NUM_0
#define SENSOR_PWR_ENABLED 0
#define SENSOR_PWR_PIN  GPIO_NUM_NC
#define SENSOR_PWR_ON_LEVEL  1
#define SENSOR_PWR_OFF_LEVEL 0
#define SENSOR_PWR_STABILIZE_MS 200

#define TIMEOUT_US      30000       // timeout do echo (30 ms)
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
        if (ota_in_progress) {
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

        if (strncmp(data, "OTA:", 4) == 0) {
            ota_handle_control(data);
            break;
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
    ESP_ERROR_CHECK(nvs_flash_init());

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

    accel_check_idle_pin_levels();

    esp_err_t ret = lsm303_i2c_master_init();
    if (ret != ESP_OK) {
        lsm303_error = true;
        ESP_LOGE(TAG, "Falha ao iniciar I2C do acelerometro: %s", esp_err_to_name(ret));
    } else {
        tentar_iniciar_acelerometro(true);
    }

    ESP_LOGI(TAG, "FilterTrack BLE pronto!");

    // ── Loop principal ────────────────────────────────────────────────────
    int64_t ultimo_envio_us = esp_timer_get_time();
    int64_t ultimo_retry_accel_us = 0;
    while (1) {
        int64_t agora = esp_timer_get_time();

        // ── Atualiza LEDs de status ──────────────────────────────────────
        atualizar_leds_status(!low_power_mode);

        if (ota_in_progress) {
            // Pausa leituras e notificações de sensor para não competir com a
            // transferência de firmware pelo mesmo link BLE.
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

        if (lsm303_initialized) {
            raw_ok = ler_accel_raw(&accel_raw);
        } else if ((agora - ultimo_retry_accel_us) >= ACCEL_RETRY_INTERVAL_US) {
            ultimo_retry_accel_us = agora;
            tentar_iniciar_acelerometro(true);
        }

        float distancia_cm = medir_distancia_cm();
        bool distancia_ok = distancia_cm > 0.0f;
        agora = esp_timer_get_time();

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

        vTaskDelay(pdMS_TO_TICKS(INTERVALO_MS));
    }
}
