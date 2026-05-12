#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/i2c.h"
#include "esp_timer.h"
#include "esp_rom_sys.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"

#include "esp_bt.h"
#include "esp_gap_ble_api.h"
#include "esp_gatt_common_api.h"
#include "esp_gatts_api.h"
#include "esp_bt_main.h"

// ─── Sensor ultrassônico ───────────────────────────────────────────────────
#define TRIG_PIN        GPIO_NUM_20
#define ECHO_PIN        GPIO_NUM_21
#define SENSOR_PWR_PIN  GPIO_NUM_10
#define SENSOR_PWR_ON_LEVEL  0  // PNP high-side switch: base low powers the sensor.
#define SENSOR_PWR_OFF_LEVEL 1  // Base high shuts the sensor down.
#define SENSOR_PWR_STABILIZE_MS 10

#define TIMEOUT_US      30000       // timeout do echo (30 ms)
#define INTERVALO_MS    100         // intervalo entre leituras (100 ms)
#define LOW_POWER_INTERVAL_MS 1000  // intervalo no modo baixo consumo (1 s)
#define WATCHDOG_US     5000000     // 5 s sem leitura válida = erro

// LSM303DLHC - acelerometro + magnetometro
#define LSM303_I2C_SDA_IO           GPIO_NUM_8
#define LSM303_I2C_SCL_IO           GPIO_NUM_9
#define LSM303_I2C_MASTER_NUM       I2C_NUM_0
#define LSM303_I2C_FREQ_HZ          100000
#define LSM303_I2C_TX_BUF_DISABLE   0
#define LSM303_I2C_RX_BUF_DISABLE   0

#define LSM303_ACCEL_ADDR           0x19
#define LSM303_MAG_ADDR             0x1E

#define CTRL_REG1_A                 0x20
#define CTRL_REG4_A                 0x23
#define OUT_X_L_A                   0x28

#define CRA_REG_M                   0x00
#define CRB_REG_M                   0x01
#define MR_REG_M                    0x02
#define OUT_X_H_M                   0x03

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
#define BLE_NOTIFY_PAYLOAD_MAX 20

static uint16_t gatts_if_global  = 0;
static uint16_t conn_id_global   = 0;
static uint16_t char_handle      = 0;
static bool     device_connected = false;
static bool     adv_config_done  = false;
static volatile bool low_power_mode = false;
static volatile bool bluetooth_error = false;
static volatile bool ble_advertising = false;
static volatile bool sensor_power_on = false;
static volatile bool lsm303_error = false;
static bool lsm303_initialized = false;

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
    if (sensor_power_on == on) {
        return;
    }

    gpio_set_level(SENSOR_PWR_PIN,
                   on ? SENSOR_PWR_ON_LEVEL : SENSOR_PWR_OFF_LEVEL);
    sensor_power_on = on;

    if (on) {
        vTaskDelay(pdMS_TO_TICKS(SENSOR_PWR_STABILIZE_MS));
    }
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

static esp_err_t lsm303_write_reg(uint8_t dev_addr, uint8_t reg_addr, uint8_t data)
{
    uint8_t write_buf[2] = {reg_addr, data};

    return i2c_master_write_to_device(
        LSM303_I2C_MASTER_NUM,
        dev_addr,
        write_buf,
        sizeof(write_buf),
        pdMS_TO_TICKS(1000)
    );
}

static esp_err_t lsm303_read_regs(uint8_t dev_addr, uint8_t reg_addr, uint8_t *data, size_t len)
{
    return i2c_master_write_read_device(
        LSM303_I2C_MASTER_NUM,
        dev_addr,
        &reg_addr,
        1,
        data,
        len,
        pdMS_TO_TICKS(1000)
    );
}

static esp_err_t lsm303_init(void)
{
    esp_err_t ret;

    ret = lsm303_write_reg(LSM303_ACCEL_ADDR, CTRL_REG1_A, 0x57);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao configurar CTRL_REG1_A do acelerometro");
        return ret;
    }

    ret = lsm303_write_reg(LSM303_ACCEL_ADDR, CTRL_REG4_A, 0x08);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao configurar CTRL_REG4_A do acelerometro");
        return ret;
    }

    ret = lsm303_write_reg(LSM303_MAG_ADDR, CRA_REG_M, 0x14);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao configurar CRA_REG_M do magnetometro");
        return ret;
    }

    ret = lsm303_write_reg(LSM303_MAG_ADDR, CRB_REG_M, 0x20);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao configurar CRB_REG_M do magnetometro");
        return ret;
    }

    ret = lsm303_write_reg(LSM303_MAG_ADDR, MR_REG_M, 0x00);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao configurar MR_REG_M do magnetometro");
        return ret;
    }

    return ESP_OK;
}

static esp_err_t read_accel_raw(vec3i16_t *accel)
{
    uint8_t data[6];

    esp_err_t ret = lsm303_read_regs(LSM303_ACCEL_ADDR, OUT_X_L_A | 0x80, data, 6);
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

static esp_err_t read_mag_raw(vec3i16_t *mag)
{
    uint8_t data[6];

    esp_err_t ret = lsm303_read_regs(LSM303_MAG_ADDR, OUT_X_H_M, data, 6);
    if (ret != ESP_OK) {
        return ret;
    }

    mag->x = (int16_t)((data[0] << 8) | data[1]);
    mag->z = (int16_t)((data[2] << 8) | data[3]);
    mag->y = (int16_t)((data[4] << 8) | data[5]);

    return ESP_OK;
}

static bool ler_lsm303_raw(vec3i16_t *accel_raw, vec3i16_t *mag_raw)
{
    esp_err_t ret = read_accel_raw(accel_raw);
    if (ret != ESP_OK) {
        lsm303_error = true;
        ESP_LOGE(TAG, "Falha ao ler acelerometro: %s", esp_err_to_name(ret));
        return false;
    }

    ret = read_mag_raw(mag_raw);
    if (ret != ESP_OK) {
        lsm303_error = true;
        ESP_LOGE(TAG, "Falha ao ler magnetometro: %s", esp_err_to_name(ret));
        return false;
    }

    lsm303_error = false;
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
                               vec3i16_t accel_raw,
                               vec3i16_t mag_raw)
{
    char ble_msg[64];
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
        offset += snprintf(
            ble_msg + offset,
            sizeof(ble_msg) - offset,
            ";M=%d,%d,%d",
            mag_raw.x,
            mag_raw.y,
            mag_raw.z
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

    default:
        ESP_LOGW(TAG, "Comando bluetooth invalido: %d", comando);
        break;
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
            }, 4);
        break;
    }

    case ESP_GATTS_CREATE_EVT:
        ESP_LOGI(TAG, "Serviço criado");
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
        char_handle = param->add_char.attr_handle;
        ESP_LOGI(TAG, "Characteristic criada (handle=%d)", char_handle);
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

        char data[32] = {0};
        size_t copy_len = param->write.len;
        if (copy_len > (sizeof(data) - 1)) {
            copy_len = sizeof(data) - 1;
        }
        memcpy(data, param->write.value, copy_len);
        data[copy_len] = '\0';
        ESP_LOGI(TAG, "Recebido do cliente: %s", data);

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

    io_conf.pin_bit_mask  = (1ULL << SENSOR_PWR_PIN);
    io_conf.mode          = GPIO_MODE_OUTPUT;
    io_conf.pull_up_en    = GPIO_PULLUP_DISABLE;
    io_conf.pull_down_en  = GPIO_PULLDOWN_DISABLE;
    gpio_config(&io_conf);
    set_sensor_power(true);

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
    esp_ble_gatt_set_local_mtu(128);

    esp_err_t ret = lsm303_i2c_master_init();
    if (ret != ESP_OK) {
        lsm303_error = true;
        ESP_LOGE(TAG, "Falha ao iniciar I2C do LSM303: %s", esp_err_to_name(ret));
    } else {
        ret = lsm303_init();
        if (ret != ESP_OK) {
            lsm303_error = true;
            ESP_LOGE(TAG, "Falha ao iniciar LSM303: %s", esp_err_to_name(ret));
        } else {
            lsm303_initialized = true;
            lsm303_error = false;
            ESP_LOGI(TAG, "LSM303 pronto para leitura raw");
        }
    }

    ESP_LOGI(TAG, "FilterTrack BLE pronto!");

    // ── Loop principal ────────────────────────────────────────────────────
    int64_t ultimo_envio_us = esp_timer_get_time();
    while (1) {
        int64_t agora = esp_timer_get_time();

        // ── Atualiza LEDs de status ──────────────────────────────────────
        atualizar_leds_status(!low_power_mode);

        if (low_power_mode) {
            // Reduz leitura do sensor para economizar energia, mantendo BLE ativo.
            gpio_set_level(TRIG_PIN, 0);
            ultimo_envio_us = agora;
            vTaskDelay(pdMS_TO_TICKS(LOW_POWER_INTERVAL_MS));
            continue;
        }

        vec3i16_t accel_raw = {0};
        vec3i16_t mag_raw = {0};
        bool raw_ok = false;

        if (lsm303_initialized) {
            raw_ok = ler_lsm303_raw(&accel_raw, &mag_raw);
        }

        float distancia_cm = medir_distancia_cm();
        bool distancia_ok = distancia_cm > 0.0f;
        agora = esp_timer_get_time();

        char msg[128];
        if (distancia_ok && raw_ok) {
            snprintf(
                msg,
                sizeof(msg),
                "DIST=%.2f;ACC_RAW=%d,%d,%d;MAG_RAW=%d,%d,%d",
                distancia_cm,
                accel_raw.x,
                accel_raw.y,
                accel_raw.z,
                mag_raw.x,
                mag_raw.y,
                mag_raw.z
            );
        } else if (distancia_ok) {
            snprintf(msg, sizeof(msg), "DIST=%.2f;RAW=ERRO", distancia_cm);
        } else if (raw_ok) {
            snprintf(
                msg,
                sizeof(msg),
                "DIST=ERRO;ACC_RAW=%d,%d,%d;MAG_RAW=%d,%d,%d",
                accel_raw.x,
                accel_raw.y,
                accel_raw.z,
                mag_raw.x,
                mag_raw.y,
                mag_raw.z
            );
        } else {
            snprintf(msg, sizeof(msg), "DIST=ERRO;RAW=ERRO");
        }

        ESP_LOGI(TAG, "%s", msg);

        if (device_connected) {
            enviar_medidas_ble(distancia_ok, distancia_cm, raw_ok, accel_raw, mag_raw);
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
