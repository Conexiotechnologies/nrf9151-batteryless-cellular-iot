/*
 * Copyright (c) 2025 Conexio Technologies, Inc.
 * Copyright (c) 2022-2023 Golioth, Inc.
 * Author: Rajeev Piyare <rajeev@conexiotech.com>
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(solaris, LOG_LEVEL_DBG);

#include <app_version.h>
#include "app_rpc.h"
#include "app_settings.h"
#include "app_state.h"
#include "app_sensors.h"
#include <golioth/client.h>
#include <golioth/fw_update.h>
#include <samples/common/net_connect.h>
#include <samples/common/sample_credentials.h>
#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <modem/lte_lc.h>
#include <helpers/nrfx_reset_reason.h>
#include "location_tracking.h"

#ifdef CONFIG_NRF_FUEL_GAUGE
#include "fuel_gauge.h"
#endif

#ifdef CONFIG_MODEM_INFO
#include <modem/modem_info.h>
#endif

/* Current firmware version; update in VERSION */
static const char *_current_version =
	STRINGIFY(APP_VERSION_MAJOR) "." STRINGIFY(APP_VERSION_MINOR) "." STRINGIFY(APP_PATCHLEVEL);

struct golioth_client *client;

K_SEM_DEFINE(connected, 0, 1);

static k_tid_t _system_thread = 0;

#if defined(CONFIG_LOCATION_TRACKING)
/* Start the cellular location thread with a delay of 30s */
K_THREAD_DEFINE(cell_location_thread, CONFIG_LOCATION_TRACKING_THREAD_STACK_SIZE, location_tracking_thread_fn,
		NULL, NULL, NULL, 0, 0, 30000);
#endif

static const struct gpio_dt_spec stratus_led = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);

/* forward declarations */
#if CONFIG_LED_INDICATION_ENABLED
void device_connection_led_set(uint8_t state);
#endif

void wake_system_thread(void)
{
	k_wakeup(_system_thread);
}

static void on_client_event(struct golioth_client *client,
							enum golioth_client_event event,
							void *arg)
{
	bool is_connected = (event == GOLIOTH_CLIENT_EVENT_CONNECTED);

	if (is_connected)
	{
		k_sem_give(&connected);

		#if CONFIG_LED_INDICATION_ENABLED
			device_connection_led_set(1);
		#endif
	}
	LOG_INF("Golioth client %s", is_connected ? "connected" : "disconnected");
}

static void start_golioth_client(void)
{
	/* Get the client configuration from auto-loaded settings */
	const struct golioth_client_config *client_config = golioth_sample_credentials_get();

	/* Create and start a Golioth Client */
	client = golioth_client_create(client_config);

	/* Register Golioth on_connect callback */
	golioth_client_register_event_callback(client, on_client_event, NULL);

	/* Initialize DFU components */
	// golioth_fw_update_init(client, _current_version);

	/*** Call Golioth APIs for other services in dedicated app files ***/

	/* Observe State service data */
	app_state_observe(client);

	/* Set Golioth Client for streaming sensor data */
	app_sensors_set_client(client);

	/* Register Settings service */
	app_settings_register(client);

	/* Register RPC service */
	//app_rpc_register(client);
}

static void lte_handler(const struct lte_lc_evt *const evt)
{
	switch (evt->type)
	{
		case LTE_LC_EVT_NW_REG_STATUS:
			switch (evt->nw_reg_status)
			{
			case LTE_LC_NW_REG_REGISTERED_ROAMING:
			case LTE_LC_NW_REG_REGISTERED_HOME:
				if (!client)
				{
					/* Create and start a Golioth Client */
					start_golioth_client();
				}
				break;
			}
			break;
	#if CONFIG_LTE_PSM_REQ
		case LTE_LC_EVT_PSM_UPDATE:
			LOG_INF("PSM parameter update: TAU: %d s, Active time: %d s\n",
					evt->psm_cfg.tau, evt->psm_cfg.active_time);
			break;
		case LTE_LC_EVT_CELL_UPDATE:
			LOG_INF("LTE cell changed: Cell ID: %d, Tracking area: %d\n",
					evt->cell.id, evt->cell.tac);
			break;
	#endif
		default:
			break;
	}
}

#if CONFIG_LTE_PSM_REQ
static int configure_lte_low_power(void)
{
	int err;

	if (IS_ENABLED(CONFIG_LTE_PSM_REQ))
	{
		/** Power Saving Mode */
		err = lte_lc_psm_req(true);

		if (err)
		{
			LOG_ERR("lte_lc_psm_req, error: %d", err);
		}
	}

	return err;
}
#endif

#ifdef CONFIG_MODEM_INFO
static void log_modem_firmware_version(void)
{
	int err;
	char sbuf[128];

	/* Initialize modem info */
	err = modem_info_init();
	if (err)
	{
		LOG_ERR("Failed to initialize modem info: %d", err);
	}

	/* Log modem firmware version */
	modem_info_string_get(MODEM_INFO_FW_VERSION, sbuf, sizeof(sbuf));
	LOG_INF("Modem firmware version: %s", sbuf);
}
#endif

#if CONFIG_LED_INDICATION_ENABLED
/* Set (unset) LED indicators for active Golioth connection */
void device_connection_led_set(uint8_t state)
{
	/* Blink LED once connected */
	gpio_pin_set_dt(&stratus_led, 1);
	k_msleep(2000);
	gpio_pin_set_dt(&stratus_led, 0);
}
#endif

static void reset_reason_str_get(char *str, uint32_t reason)
{
	size_t len;

	*str = '\0';

	if (reason & NRFX_RESET_REASON_RESETPIN_MASK) {
		(void)strcat(str, "PIN reset | ");
	}
	if (reason & NRFX_RESET_REASON_DOG_MASK) {
		(void)strcat(str, "watchdog | ");
	}
	if (reason & NRFX_RESET_REASON_OFF_MASK) {
		(void)strcat(str, "wakeup from power-off | ");
	}
	if (reason & NRFX_RESET_REASON_DIF_MASK) {
		(void)strcat(str, "debug interface wakeup | ");
	}
	if (reason & NRFX_RESET_REASON_SREQ_MASK) {
		(void)strcat(str, "software | ");
	}
	if (reason & NRFX_RESET_REASON_LOCKUP_MASK) {
		(void)strcat(str, "CPU lockup | ");
	}
	if (reason & NRFX_RESET_REASON_CTRLAP_MASK) {
		(void)strcat(str, "control access port | ");
	}

	len = strlen(str);
	if (len == 0) {
		(void)strcpy(str, "power-on reset");
	} else {
		str[len - 3] = '\0';
	}
}

static void print_reset_reason(void)
{
	uint32_t reset_reason;
	char reset_reason_str[128];
	reset_reason = nrfx_reset_reason_get();

	reset_reason_str_get(reset_reason_str, reset_reason);
	LOG_INF("Reset reason: %s (0x%x)", reset_reason_str, reset_reason);
}

int main(void)
{
	int err;

	LOG_DBG("Starting sample on %s\n", CONFIG_BOARD);
	LOG_INF("Firmware version: %s", _current_version);
	IF_ENABLED(CONFIG_MODEM_INFO, (log_modem_firmware_version();));
	print_reset_reason();

	/* Get system thread id so loop delay change event can wake main */
	_system_thread = k_current_get();

	/* Initialize LED */
	err = gpio_pin_configure_dt(&stratus_led, GPIO_OUTPUT_INACTIVE);
	if (err)
	{
		LOG_ERR("Unable to configure LED");
	}

#if defined(CONFIG_NRF_FUEL_GAUGE)
	bool fuel_gauge_initialized = npm1300_fuel_gauge_init();

	if (!fuel_gauge_initialized)
	{
		LOG_ERR("Fuel gauge init, error: %d", fuel_gauge_initialized);
		return 0;
	}
#endif

	/* Start LTE asynchronously if the nRF91xx is used.
	 * Golioth Client will start automatically when LTE connects
	 */

	LOG_INF("Connecting to LTE, this may take some time...");
	err = lte_lc_connect_async(lte_handler);
	if (err)
	{
		LOG_ERR("Failed to connect to LTE network, error: %d", err);
		return -1;
	}

#if CONFIG_LTE_PSM_REQ
	err = configure_lte_low_power();
	if (err)
	{
		printk("Unable to set low power configuration, error: %d", err);
	}
#endif

	/* Block until connected to Golioth */
	k_sem_take(&connected, K_FOREVER);

	report_startup();

	while (true)
	{
		/* Check LTE connection and if Golioth client is connected */
		if (!golioth_client_is_connected(client))
		{
			LOG_DBG("LTE connection lost, reconnecting...");
			lte_lc_connect_async(lte_handler);
			k_sem_take(&connected, K_FOREVER);
		}

		/* Read sensor data and send it */
		app_sensors_read_and_stream();

		/* Sleep before the next cycle */
		k_sleep(K_SECONDS(get_loop_delay_s()));
	}
}
