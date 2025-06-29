/*
 * Copyright (c) 2025 Conexio Technologies, Inc
 * Author: Rajeev Piyare <rajeev@conexiotech.com>
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(app_sensors, LOG_LEVEL_DBG);

#include <golioth/client.h>
#include <golioth/stream.h>
#include <zcbor_encode.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <date_time.h>
#include <stdio.h>
#include <zephyr/drivers/sensor.h>
#include "app_sensors.h"
#include "fuel_gauge.h"
#include <helpers/nrfx_reset_reason.h>
#include <modem/modem_info.h>

#define NUM_SENSOR_KEY_VALUE_PAIRS   2
#define MODEM_MAP_ENTRIES            4
#define BATTERY_MAP_ENTRIES          5

#define JSON_FMT "{\"rst_reason\":%d}"

static struct golioth_client *client;

static uint32_t tx_success_counter = 0;
static uint32_t tx_failure_counter = 0;

/* Callback for LightDB Stream */
void async_error_handler(struct golioth_client *client, enum golioth_status status,
						 const struct golioth_coap_rsp_code *coap_rsp_code, const char *path,
						 void *arg)
{
	if (status != GOLIOTH_OK)
	{
		LOG_ERR("Async task failed: %d", status);
		return;
	}
}

uint32_t app_sensors_get_tx_success_count(void)
{
	return tx_success_counter;
}

uint32_t app_sensors_get_tx_failure_count(void)
{
	return tx_failure_counter;
}

static enum golioth_status read_modem_data(zcbor_state_t *zse)
{
	bool ok;
	int err;
	int modem_voltage;
	int modem_temp;

	err = modem_info_get_batt_voltage(&modem_voltage);
	if (err) {
		LOG_ERR("Modem voltage read failed, err: %d\n", err);
		return GOLIOTH_ERR_FAIL;
	}
	LOG_INF("Modem voltage: %d mV", modem_voltage);

	err = modem_info_get_temperature(&modem_temp);
	if (err) {
		LOG_ERR("Modem Temp read failed, err: %d\n", err);
		return GOLIOTH_ERR_FAIL;
	}
	LOG_INF("Modem Temp: %d degC\n", modem_temp);

	ok = zcbor_tstr_put_lit(zse, "modem") && zcbor_map_start_encode(zse, MODEM_MAP_ENTRIES);
	if (!ok) {
		LOG_ERR("ZCBOR unable to open modem map");
		return GOLIOTH_ERR_QUEUE_FULL;
	}

	ok = zcbor_tstr_put_lit(zse, "vbat") &&
		 zcbor_int32_put(zse, modem_voltage) &&
		 zcbor_tstr_put_lit(zse, "temp") &&
		 zcbor_int32_put(zse, modem_temp) &&
		 zcbor_tstr_put_lit(zse, "success") &&
		 zcbor_int32_put(zse, app_sensors_get_tx_success_count()) &&
		 zcbor_tstr_put_lit(zse, "fail") &&
		 zcbor_int32_put(zse, app_sensors_get_tx_failure_count());

	if (!ok) {
		LOG_ERR("ZCBOR failed to encode modem data");
		return GOLIOTH_ERR_QUEUE_FULL;
	}

	ok = zcbor_map_end_encode(zse, MODEM_MAP_ENTRIES);
	if (!ok) {
		LOG_ERR("ZCBOR failed to close modem map");
		return GOLIOTH_ERR_QUEUE_FULL;
	}

	return GOLIOTH_OK;
}

static enum golioth_status read_battery_data(zcbor_state_t *zse)
{
	bool ok;
	struct battery_data batt_data;
	get_battery_data(&batt_data);

	struct sensor_value voltage;
	struct sensor_value current;
	struct sensor_value soc;
	struct sensor_value tte;
	struct sensor_value ttf;

	sensor_value_from_double(&voltage, batt_data.voltage);
	sensor_value_from_double(&current, batt_data.current);
	sensor_value_from_double(&soc, batt_data.soc);
	sensor_value_from_double(&tte, batt_data.tte);
	sensor_value_from_double(&ttf, batt_data.ttf);

	ok = zcbor_tstr_put_lit(zse, "battery") && zcbor_map_start_encode(zse, BATTERY_MAP_ENTRIES);
	if (!ok)
	{
		LOG_ERR("ZCBOR unable to open battery map");
		return GOLIOTH_ERR_QUEUE_FULL;
	}

	ok = zcbor_tstr_put_lit(zse, "V") &&
		 zcbor_float64_put(zse, sensor_value_to_double(&voltage)) &&
		 zcbor_tstr_put_lit(zse, "I") &&
		 zcbor_float64_put(zse, sensor_value_to_double(&current)) &&
		 zcbor_tstr_put_lit(zse, "SoC") &&
		 zcbor_float64_put(zse, sensor_value_to_double(&soc)) &&
		 zcbor_tstr_put_lit(zse, "tte") &&
		 zcbor_float64_put(zse, sensor_value_to_double(&tte)) &&
		 zcbor_tstr_put_lit(zse, "ttf") &&
		 zcbor_float64_put(zse, sensor_value_to_double(&ttf));

	if (!ok)
	{
		LOG_ERR("ZCBOR failed to encode battery data");
		return GOLIOTH_ERR_QUEUE_FULL;
	}

	ok = zcbor_map_end_encode(zse, BATTERY_MAP_ENTRIES);
	if (!ok)
	{
		LOG_ERR("ZCBOR failed to close battery map");
		return GOLIOTH_ERR_QUEUE_FULL;
	}

	return GOLIOTH_OK;
}

/* This will be called by the main() loop */
/* Do all of your work here! */
void app_sensors_read_and_stream(void)
{
	int err;
	bool ok;
	enum golioth_status status;
	char cbor_buf[256];

	ZCBOR_STATE_E(zse, NUM_SENSOR_KEY_VALUE_PAIRS, cbor_buf, sizeof(cbor_buf), 1);

	ok = zcbor_map_start_encode(zse, NUM_SENSOR_KEY_VALUE_PAIRS);

	if (!ok)
	{
		LOG_ERR("ZCBOR failed to open map");
		return;
	}

	status = read_modem_data(zse);
	if (status == GOLIOTH_ERR_QUEUE_FULL)
	{
		return;
	}

	status = read_battery_data(zse);
	if (status == GOLIOTH_ERR_QUEUE_FULL)
	{
		return;
	}

	ok = zcbor_map_end_encode(zse, NUM_SENSOR_KEY_VALUE_PAIRS);
	if (!ok)
	{
		LOG_ERR("ZCBOR failed to close map");
	}

	size_t cbor_size = zse->payload - (const uint8_t *)cbor_buf;

	/* Only stream sensor data if connected */
	if (golioth_client_is_connected(client))
	{
		/* Send to LightDB Stream on "sensor" endpoint */
		err = golioth_stream_set_async(client, "sensor", GOLIOTH_CONTENT_TYPE_CBOR, cbor_buf,
									   cbor_size, async_error_handler, NULL);
		if (err)
		{   
			tx_failure_counter++;
			LOG_ERR("Failed to send sensor data to Golioth: %d", err);
		}
		else 
		{
			tx_success_counter++;
		}
	}
	else
	{
		LOG_DBG("No connection available, skipping sending data to Golioth");
	}
}

void app_sensors_set_client(struct golioth_client *sensors_client)
{
	client = sensors_client;
}

#define DEVICE_DATA_ENDP "device/state"

int report_startup(void)
{
	int err;
	char json_buf[128];
	uint32_t reset_reason;
	
	reset_reason = nrfx_reset_reason_get();
	snprintk(json_buf, sizeof(json_buf), JSON_FMT, reset_reason);

	LOG_INF("App: Reset reason: 0x%x", reset_reason);
	nrfx_reset_reason_clear(reset_reason);

	if (!client) {
		LOG_ERR("Golioth client not initialized!");
		return -EINVAL;
	}

	err = golioth_stream_set_async(client, DEVICE_DATA_ENDP, GOLIOTH_CONTENT_TYPE_JSON, json_buf,
				       strlen(json_buf), async_error_handler, NULL);
	if (err) {
		LOG_ERR("Failed to send device info to Golioth: %d", err);
		return err;
	} else {
		LOG_INF("Sent device info to Golioth");
	}

	return 0;
}
