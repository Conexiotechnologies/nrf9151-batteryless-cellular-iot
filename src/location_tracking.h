/*
 * Copyright (c) 2025 Conexio Technologies, Inc
 */

#ifndef _LOCATION_TRACKING_H_
#define _LOCATION_TRACKING_H_

#include <golioth/location/cellular.h>

void location_tracking_thread_fn(void);

int cellular_info_get(struct golioth_cellular_info *infos,
                      size_t num_max_infos,
                      size_t *num_returned_infos);

#endif /* _LOCATION_TRACKING_H_ */
