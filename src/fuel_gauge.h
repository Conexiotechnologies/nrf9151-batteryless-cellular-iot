/*
 * Copyright (c) 2025 Conexio Technologies, Inc
 */

#ifndef __FUEL_GAUGE_H__
#define __FUEL_GAUGE_H__

#include <zcbor_encode.h>

struct battery_data {
    float voltage;
    float current;
    float temp;
    float soc;
    float tte;
    float ttf;
};

int npm1300_fuel_gauge_init(void);
void get_battery_data(struct battery_data *data);
int set_3v3_power_gate(bool state);
void turn_off_regulators(void);

#endif /* __FUEL_GAUGE_H__ */
