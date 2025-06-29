/*
 * Copyright (c) 2025 Conexio Technologies, Inc
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <stdio.h>
#include <string.h>

#include <golioth/client.h>
#include <golioth/location/cellular.h>
#include <string.h>
#include <samples/common/net_connect.h>
#include "location_tracking.h"
#include "main.h"

#if defined(CONFIG_LOCATION_TRACKING)
LOG_MODULE_REGISTER(location_tracking, LOG_LEVEL_DBG);

/* Timer used to time the location sampling rate. */
static K_TIMER_DEFINE(location_sample_timer, NULL, NULL);

#define APP_TIMEOUT_S 10

static struct golioth_location_req location_req;

static int cellular_get_and_encode_info(void)
{
    struct golioth_cellular_info cellular_infos[4];
    enum golioth_status status;
    size_t num_infos;
    int err;

    if (!IS_ENABLED(CONFIG_GOLIOTH_LOCATION_CELLULAR))
    {
        LOG_ERR("!IS_ENABLED(CONFIG_GOLIOTH_LOCATION_CELLULAR)");
        return 0;
    }

    err = cellular_info_get(cellular_infos, ARRAY_SIZE(cellular_infos), &num_infos);
    if (err)
    {
        LOG_ERR("Failed to get cellular network info: %d", err);
        return err;
    }

    for (size_t i = 0; i < num_infos; i++)
    {
        status = golioth_location_cellular_append(&location_req, &cellular_infos[i]);
        if (status != GOLIOTH_OK)
        {
            LOG_ERR("Failed to append cellular info: %d", status);
            return -ENOMEM;
        }
    }

    return 0;
}

void location_tracking_thread_fn(void)
{
    struct golioth_location_rsp location_rsp;
    enum golioth_status status;
    int err;

	LOG_INF("Location tracking module has started");

	/* Begin location tracker */
	while (true) {
		k_timer_start(&location_sample_timer,
			K_SECONDS(CONFIG_LOCATION_TRACKING_SAMPLE_INTERVAL_SECONDS), K_FOREVER);

        golioth_location_init(&location_req);

        err = cellular_get_and_encode_info();
        if (err)
        {
            LOG_ERR("cellular_get_and_encode_info() err");
            continue;
        }

        status = golioth_location_finish(&location_req);
        if (status != GOLIOTH_OK)
        {
            LOG_ERR("Failed golioth_location_finish");
            if (status == GOLIOTH_ERR_NULL)
            {
                LOG_WRN("No location data to be provided");
                return;
            }

            LOG_ERR("Failed to encode location data");
            return;
        }

        status = golioth_location_get_sync(client, &location_req, &location_rsp, APP_TIMEOUT_S);
        if (status == GOLIOTH_OK)
        {
            LOG_INF("%s%lld.%09lld %s%lld.%09lld (%lld)",
                    location_rsp.latitude < 0 ? "-" : "",
                    llabs(location_rsp.latitude) / 1000000000,
                    llabs(location_rsp.latitude) % 1000000000,
                    location_rsp.longitude < 0 ? "-" : "",
                    llabs(location_rsp.longitude) / 1000000000,
                    llabs(location_rsp.longitude) % 1000000000,
                    (long long int) location_rsp.accuracy);
        }

		/* Wait out any remaining time on the sample interval timer. */
		k_timer_status_sync(&location_sample_timer);
	}
}
#endif
