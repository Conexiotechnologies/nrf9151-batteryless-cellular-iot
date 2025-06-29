# Copyright (c) 2025 Conexio Technologies, Inc

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})

project(solaris)

target_sources(app PRIVATE src/main.c)
target_sources(app PRIVATE src/app_rpc.c)
target_sources(app PRIVATE src/app_settings.c)
target_sources(app PRIVATE src/app_state.c)
target_sources(app PRIVATE src/app_sensors.c)
target_sources(app PRIVATE src/fuel_gauge.c)
target_sources(app PRIVATE src/location_tracking.c)
target_sources_ifdef(CONFIG_SOC_SERIES_NRF91X app PRIVATE src/cellular_nrf91.c)
