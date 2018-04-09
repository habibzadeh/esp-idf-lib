/**
 * @file i2cdev.c
 *
 * ESP-32 I2C master thread-safe functions for communication with I2C slave
 *
 * The MIT License (MIT)
 *
 * Copyright (c) 2018 Ruslan V. Uss (https://github.com/UncleRus)
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */
#include "i2cdev.h"
#include <string.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <esp_log.h>

static const char *TAG = "I2C_DEV";

static SemaphoreHandle_t locks[I2C_NUM_MAX];
static i2c_config_t configs[I2C_NUM_MAX];

#define SEMAPHORE_TAKE(port) do { \
        if (!xSemaphoreTake(locks[port], CONFIG_I2CDEV_TIMEOUT / portTICK_RATE_MS)) \
        { \
            ESP_LOGE(TAG, "Could not take mutex %d", port); \
            return ESP_ERR_TIMEOUT; \
        } \
        } while (0)

#define SEMAPHORE_GIVE(port) do { \
        if (!xSemaphoreGive(locks[port])) \
        { \
            ESP_LOGE(TAG, "Could not give mutex %d", port); \
            return ESP_FAIL; \
        } \
        } while (0)


esp_err_t i2cdev_init()
{
    for (int i = 0; i < I2C_NUM_MAX; i++)
    {
        locks[i] = xSemaphoreCreateMutex();
        if (!locks[i])
        {
            ESP_LOGE(TAG, "Could not create mutex %d", i);
            return ESP_FAIL;
        }
    }

    return ESP_OK;
}

esp_err_t i2cdev_done()
{
    for (int i = 0; i < I2C_NUM_MAX; i++)
    {
        if (!locks[i]) continue;

        SEMAPHORE_TAKE(i);
        i2c_driver_delete(i);
        SEMAPHORE_GIVE(i);
        vSemaphoreDelete(locks[i]);
    }
    return ESP_OK;
}

inline static bool cfg_equal(const i2c_config_t *a, const i2c_config_t *b)
{
    return a->scl_io_num == b->scl_io_num
        && a->sda_io_num == b->sda_io_num
        && a->scl_pullup_en == b->scl_pullup_en
        && a->sda_pullup_en == b->sda_pullup_en
        && a->master.clk_speed == b->master.clk_speed;
}

static esp_err_t i2c_setup_port(i2c_port_t port, const i2c_config_t *cfg)
{
    if (!cfg) return ESP_ERR_INVALID_ARG;

    esp_err_t res;
    if (!cfg_equal(cfg, &configs[port]))
    {
        i2c_config_t temp;
        memcpy(&temp, cfg, sizeof(i2c_config_t));
        temp.mode = I2C_MODE_MASTER;

        i2c_driver_delete(port);
        if ((res = i2c_param_config(port, &temp)) != ESP_OK)
            return res;
        if ((res = i2c_driver_install(port, temp.mode, 0, 0, 0)) != ESP_OK)
            return res;
    }

    return ESP_OK;
}

esp_err_t i2c_dev_read(const i2c_dev_t *dev, const void *out_data, size_t out_size, void *in_data, size_t in_size)
{
    if (!dev || !in_data || !in_size) return ESP_ERR_INVALID_ARG;

    SEMAPHORE_TAKE(dev->port);

    esp_err_t res = i2c_setup_port(dev->port, &dev->cfg);
    if (res == ESP_OK)
    {
        i2c_cmd_handle_t cmd = i2c_cmd_link_create();
        if (out_data && out_size)
        {
            i2c_master_start(cmd);
            i2c_master_write_byte(cmd, dev->addr << 1, true);
            i2c_master_write(cmd, (void *)out_data, out_size, true);
        }
        i2c_master_start(cmd);
        i2c_master_write_byte(cmd, (dev->addr << 1) | 1, true);
        i2c_master_read(cmd, in_data, in_size, I2C_MASTER_LAST_NACK);
        i2c_master_stop(cmd);

        res = i2c_master_cmd_begin(dev->port, cmd, CONFIG_I2CDEV_TIMEOUT / portTICK_RATE_MS);
        if (res != ESP_OK)
            ESP_LOGE(TAG, "Could not read from device [0x%02x at %d]: %d", dev->addr, dev->port, res);

        i2c_cmd_link_delete(cmd);
    }

    SEMAPHORE_GIVE(dev->port);
    return res;
}

esp_err_t i2c_dev_write(const i2c_dev_t *dev, const void *out_reg, size_t out_reg_size, const void *out_data, size_t out_size)
{
    if (!dev || !out_data || !out_size) return ESP_ERR_INVALID_ARG;

    SEMAPHORE_TAKE(dev->port);

    esp_err_t res = i2c_setup_port(dev->port, &dev->cfg);
    if (res == ESP_OK)
    {
        i2c_cmd_handle_t cmd = i2c_cmd_link_create();
        i2c_master_start(cmd);
        i2c_master_write_byte(cmd, dev->addr << 1, true);
        if (out_reg && out_reg_size)
            i2c_master_write(cmd, (void *)out_reg, out_reg_size, true);
        i2c_master_write(cmd, (void *)out_data, out_size, true);
        i2c_master_stop(cmd);
        esp_err_t res = i2c_master_cmd_begin(dev->port, cmd, CONFIG_I2CDEV_TIMEOUT / portTICK_RATE_MS);
        if (res != ESP_OK)
            ESP_LOGE(TAG, "Could not write to device [0x%02x at %d]: %d", dev->addr, dev->port, res);
        i2c_cmd_link_delete(cmd);
    }

    SEMAPHORE_GIVE(dev->port);
    return res;
}
