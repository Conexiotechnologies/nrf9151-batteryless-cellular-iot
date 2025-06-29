/*
 * Copyright (c) 2025 Conexio Technologies, Inc.
 * Author: Rajeev Piyare <rajeev@conexiotech.com>
 */

#include <stdlib.h>
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/logging/log.h>
#include <golioth/client.h>
#include <golioth/stream.h>
#include <zcbor_encode.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/drivers/sensor/npm1300_charger.h>
#include <zephyr/drivers/mfd/npm1300.h>
#include <zephyr/drivers/regulator.h>
#include <zephyr/sys/util.h>
#include "nrf_fuel_gauge.h"
#include "fuel_gauge.h"
#include "app_sensors.h"

#if defined(CONFIG_NRF_FUEL_GAUGE)

LOG_MODULE_REGISTER(battery, LOG_LEVEL_DBG);

static const struct device *pmic = DEVICE_DT_GET(DT_INST(0, nordic_npm1300));
static const struct device *charger = DEVICE_DT_GET(DT_NODELABEL(pmic_charger));
/* For settig up 3V3 regulator */
static const struct device *buck2 = DEVICE_DT_GET(DT_NODELABEL(reg_3v3));

static volatile bool vbus_connected;

#define NPM1300_CHGR_BASE 0x3
#define NPM1300_CHGR_OFFSET_DIS_SET 0x06

/* nPM1300 CHARGER.BCHGCHARGESTATUS.CONSTANTCURRENT register bitmask */
#define NPM1300_CHG_STATUS_CC_MASK BIT_MASK(3)
#define NUM_KEY_VALUE_PAIRS		5

static bool initialized = false;
static float max_charge_current;
static float term_charge_current;
static int64_t ref_time;
static struct battery_data batt_data;

static const struct battery_model battery_model = {
#include "battery_model.inc"
};

static int npm1300_disable_ntc(void)
{
	int ret = 0;

	ret = mfd_npm1300_reg_write(pmic, NPM1300_CHGR_BASE, NPM1300_CHGR_OFFSET_DIS_SET, 2);
	if (ret < 0)
	{
		return ret;
	}

	return ret;
}

static void event_callback(const struct device *dev, struct gpio_callback *cb, uint32_t pins)
{
	vbus_connected = (pins & BIT(NPM1300_EVENT_VBUS_DETECTED)) ? true :
					 (pins & BIT(NPM1300_EVENT_VBUS_REMOVED)) ? false : vbus_connected;
	LOG_DBG("Vbus %s", vbus_connected ? "connected" : "removed");
}

static inline float get_sensor_value(const struct device *dev, enum sensor_channel chan)
{
	struct sensor_value value;
	if (sensor_channel_get(dev, chan, &value) == 0) {
		return sensor_value_to_float(&value);
	}
	return 0.0f;
}

static int fuel_gauge_init_params(const struct device *charger)
{
	struct nrf_fuel_gauge_init_parameters parameters = {
		.model = &battery_model,
		.opt_params = NULL,
	};

	LOG_DBG("nRF Fuel Gauge version: %s", nrf_fuel_gauge_version);

	if (sensor_sample_fetch(charger) < 0) {
		return -EIO;
	}

	parameters.v0 = get_sensor_value(charger, SENSOR_CHAN_GAUGE_VOLTAGE);
	parameters.i0 = get_sensor_value(charger, SENSOR_CHAN_GAUGE_AVG_CURRENT);
	parameters.t0 = get_sensor_value(charger, SENSOR_CHAN_GAUGE_TEMP);
	max_charge_current = get_sensor_value(charger, SENSOR_CHAN_GAUGE_DESIRED_CHARGING_CURRENT);
	term_charge_current = max_charge_current / 10.f;

	nrf_fuel_gauge_init(&parameters, NULL);
	ref_time = k_uptime_get();

	return 0;
}

int fuel_gauge_update(const struct device *charger, bool vbus_connected)
{
	if (sensor_sample_fetch(charger) < 0) {
		LOG_ERR("Error: Could not fetch sensor samples");
		return -EIO;
	}

	batt_data.voltage = get_sensor_value(charger, SENSOR_CHAN_GAUGE_VOLTAGE);
	batt_data.temp = get_sensor_value(charger, SENSOR_CHAN_GAUGE_TEMP);
	batt_data.current = get_sensor_value(charger, SENSOR_CHAN_GAUGE_AVG_CURRENT);

	int32_t chg_status = (int32_t)get_sensor_value(charger, SENSOR_CHAN_NPM1300_CHARGER_STATUS);
	bool cc_charging = (chg_status & NPM1300_CHG_STATUS_CC_MASK) != 0;

	float delta = (float)k_uptime_delta(&ref_time) / 1000.f;
	batt_data.soc = nrf_fuel_gauge_process(batt_data.voltage, batt_data.current, batt_data.temp, delta, vbus_connected, NULL);
	batt_data.tte = nrf_fuel_gauge_tte_get();
	batt_data.ttf = nrf_fuel_gauge_ttf_get(cc_charging, -term_charge_current);

	LOG_DBG("V: %.2f, I: %.2f, SoC: %.2f, TTE: %.0f, TTF: %.0f",
		(double)batt_data.voltage, (double)batt_data.current, (double)batt_data.soc, (double)batt_data.tte, (double)batt_data.ttf);

	return 0;
}

void get_battery_data(struct battery_data *data)
{
	fuel_gauge_update(charger, vbus_connected);
    *data = batt_data;
}

/**@brief Initialize nPM1300 fuel gauge. */
int npm1300_fuel_gauge_init(void)
{
	int err;

	LOG_DBG("Init and start nPM1300 PMIC");

	if (!device_is_ready(pmic) || !device_is_ready(charger)) {
		LOG_ERR("PMIC or Charger device not ready.");
		return -ENODEV;
	}

	npm1300_disable_ntc();

	if (fuel_gauge_init_params(charger) < 0) {
		LOG_ERR("Could not initialise fuel gauge.");
	}

	static struct gpio_callback event_cb;
	gpio_init_callback(&event_cb, event_callback,
				   BIT(NPM1300_EVENT_VBUS_DETECTED) |
				   BIT(NPM1300_EVENT_VBUS_REMOVED));

	err = mfd_npm1300_add_callback(pmic, &event_cb);
	if (err) {
		LOG_ERR("Failed to add pmic callback.");
	}

	/* Initialise vbus detection status. */
	struct sensor_value val;
	int ret = sensor_attr_get(charger, SENSOR_CHAN_CURRENT, SENSOR_ATTR_UPPER_THRESH, &val);

	if (ret < 0) {
		return false;
	}

	vbus_connected = (val.val1 != 0) || (val.val2 != 0);
	initialized = true;
	LOG_DBG("PMIC device init successful\n");

	return initialized;
}

#endif /* CONFIG_NRF_FUEL_GAUGE */
