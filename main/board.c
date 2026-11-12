/* SPDX-License-Identifier: GPL-3.0-only */
/*
 * Shared board code: the bring-up order, the calibration held in NVS, and the
 * calibration commands. Everything that differs between boards is in
 * main/boards/board_<name>.c, behind the hooks in boards/board_hooks.h.
 */
#include "board.h"
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_ipc.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "nvs_flash.h"
#include "boards/board_hooks.h"
#include "boards/board_internal.h"

#define TAG "board"

board_calibration_t pc_calibration;
adc_oneshot_unit_handle_t board_adc1;
adc_cali_handle_t board_adc1_cal;

static void board_gpio_isr_install(void *arg) {
    *(esp_err_t *)arg =
        gpio_install_isr_service(ESP_INTR_FLAG_IRAM | ESP_INTR_FLAG_LEVEL3);
}

void board_setup(void) {
    board_pins_init();

    esp_err_t gpio_isr_err = ESP_OK;
    /* Keep shared GPIO timing interrupts away from Bluetooth on core 0. */
    ESP_ERROR_CHECK(
        esp_ipc_call_blocking(1, board_gpio_isr_install, &gpio_isr_err));
    ESP_ERROR_CHECK(gpio_isr_err);

    /* ADC inputs signals */
    adc_oneshot_unit_init_cfg_t init_config1 = {
        .unit_id = ADC_UNIT_1,
    };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&init_config1, &board_adc1));
    board_adc_init(board_adc1);

    adc_cali_curve_fitting_config_t cali_config = {
        .unit_id = ADC_UNIT_1,
        .atten = ADC_ATTEN_DB_0,
        .bitwidth = ADC_BITWIDTH_12,
    };
    ESP_ERROR_CHECK(
        adc_cali_create_scheme_curve_fitting(&cali_config, &board_adc1_cal));

    board_adc_calibrated();

    /* Retrieve calibration data from persistent config nvs */
    nvs_handle_t pc_handle;
    esp_err_t err;

    err = nvs_open("pc", NVS_READONLY, &pc_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error (%s) opening NVS handle!", esp_err_to_name(err));
        goto out;
    }

    err = nvs_get_i32(pc_handle, "vbatt_gain", &pc_calibration.vbatt_gain);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error (%s) failed to get vbatt_gain!",
                 esp_err_to_name(err));
        goto out_nvs;
    }

    err = nvs_get_i32(pc_handle, "vbatt_offset", &pc_calibration.vbatt_offset);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error (%s) failed to get vbatt_offset!",
                 esp_err_to_name(err));
        goto out_nvs;
    }

    err =
        nvs_get_i32(pc_handle, "hs_sense_gain", &pc_calibration.hs_sense_gain);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error (%s) failed to get hs_sense_gain!",
                 esp_err_to_name(err));
        goto out_nvs;
    }

    err = nvs_get_i32(pc_handle, "hs_sense_offset",
                      &pc_calibration.hs_sense_offset);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error (%s) failed to get hs_sense_offset!",
                 esp_err_to_name(err));
        goto out_nvs;
    }

    err = nvs_get_i32(pc_handle, "hs_duty_gain", &pc_calibration.hs_duty_gain);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error (%s) failed to get hs_duty_gain!",
                 esp_err_to_name(err));
        goto out_nvs;
    }

    err = nvs_get_i32(pc_handle, "hs_duty_offset",
                      &pc_calibration.hs_duty_offset);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error (%s) failed to get hs_duty_offset!",
                 esp_err_to_name(err));
        goto out;
    }

    pc_calibration.calibrated = true;

out_nvs:
    nvs_close(pc_handle);
out:

    if (!pc_calibration.calibrated) {
        ESP_LOGE(TAG, "Voltage sense calibration missing. Fallback to default");
        pc_calibration.vbatt_gain = 308982;
        pc_calibration.vbatt_offset = 2361515;
        pc_calibration.hs_sense_gain = 229142;
        pc_calibration.hs_sense_offset = 601008;
        pc_calibration.hs_duty_gain = 42840;
        pc_calibration.hs_duty_offset = 36930392;
    } else {
        ESP_LOGI(TAG, "cal: vbatt_gain     : %" PRId32,
                 pc_calibration.vbatt_gain);
        ESP_LOGI(TAG, "cal: vbatt_offset   : %" PRId32,
                 pc_calibration.vbatt_offset);
        ESP_LOGI(TAG, "cal: hs_sense_gain  : %" PRId32,
                 pc_calibration.hs_sense_gain);
        ESP_LOGI(TAG, "cal: hs_sense_offset: %" PRId32,
                 pc_calibration.hs_sense_offset);
        ESP_LOGI(TAG, "cal: hs_duty_gain   : %" PRId32,
                 pc_calibration.hs_duty_gain);
        ESP_LOGI(TAG, "cal: hs_duty_offset : %" PRId32,
                 pc_calibration.hs_duty_offset);
    }
}

void board_print_info(void) {
    printf("\tcalibrated     : %d\n", pc_calibration.calibrated);
    printf("\tvbatt_gain     : %" PRId32 "\n", pc_calibration.vbatt_gain);
    printf("\tvbatt_offset   : %" PRId32 "\n", pc_calibration.vbatt_offset);
    printf("\ths_sense_gain  : %" PRId32 "\n", pc_calibration.hs_sense_gain);
    printf("\ths_sense_offset: %" PRId32 "\n", pc_calibration.hs_sense_offset);
    printf("\ths_duty_gain   : %" PRId32 "\n", pc_calibration.hs_duty_gain);
    printf("\ths_duty_offset : %" PRId32 "\n", pc_calibration.hs_duty_offset);
}

/* Calibration constants addressable by name. The name doubles as the NVS key,
 * and the order matches board_print_info(), so a dump taken from a working
 * unit can be typed straight back into a board whose NVS was erased. */
static const struct {
    const char *name;
    int32_t *value;
} pc_cal_fields[] = {
    {"vbatt_gain", &pc_calibration.vbatt_gain},
    {"vbatt_offset", &pc_calibration.vbatt_offset},
    {"hs_sense_gain", &pc_calibration.hs_sense_gain},
    {"hs_sense_offset", &pc_calibration.hs_sense_offset},
    {"hs_duty_gain", &pc_calibration.hs_duty_gain},
    {"hs_duty_offset", &pc_calibration.hs_duty_offset},
};

#define PC_CAL_NUM_FIELDS (sizeof(pc_cal_fields) / sizeof(pc_cal_fields[0]))

const char *board_calibration_field(int index) {
    if (index < 0 || index >= (int)PC_CAL_NUM_FIELDS) {
        return NULL;
    }

    return pc_cal_fields[index].name;
}

/** @brief True once every calibration key is present in the open namespace. */
static bool pc_cal_all_stored(nvs_handle_t pc_handle) {
    for (size_t i = 0; i < PC_CAL_NUM_FIELDS; i++) {
        int32_t val;

        if (nvs_get_i32(pc_handle, pc_cal_fields[i].name, &val) != ESP_OK) {
            return false;
        }
    }

    return true;
}

esp_err_t board_calibration_set(const char *name, int32_t value) {
    nvs_handle_t pc_handle;
    esp_err_t err;
    size_t i;

    for (i = 0; i < PC_CAL_NUM_FIELDS; i++) {
        if (strcmp(name, pc_cal_fields[i].name) == 0) {
            break;
        }
    }
    if (i == PC_CAL_NUM_FIELDS) {
        return ESP_ERR_NOT_FOUND;
    }

    err = nvs_open("pc", NVS_READWRITE, &pc_handle);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_i32(pc_handle, name, value);
    if (err == ESP_OK) {
        err = nvs_commit(pc_handle);
    }
    if (err != ESP_OK) {
        nvs_close(pc_handle);
        return err;
    }

    /* Apply it live as well, so the value is in use before the next reboot. */
    *pc_cal_fields[i].value = value;
    pc_calibration.calibrated = pc_cal_all_stored(pc_handle);

    nvs_close(pc_handle);

    ESP_LOGI(TAG, "cal: %s set to %" PRId32, name, value);

    return ESP_OK;
}
