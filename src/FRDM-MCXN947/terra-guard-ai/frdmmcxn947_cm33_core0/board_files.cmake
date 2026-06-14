
# Copyright 2026 NXP
#
# SPDX-License-Identifier: BSD-3-Clause

mcux_add_configuration(
    CC "-DSDK_DEBUGCONSOLE=1"
    CX "-DSDK_DEBUGCONSOLE=1"
)


mcux_add_source(
    SOURCES frdmmcxn947/board.c
            frdmmcxn947/board.h
)

mcux_add_include(
    INCLUDES frdmmcxn947
)

mcux_add_source(
    SOURCES frdmmcxn947/clock_config.c
            frdmmcxn947/clock_config.h
)

mcux_add_include(
    INCLUDES frdmmcxn947
)

mcux_add_source(
    SOURCES led_blinky/pin_mux.c
            led_blinky/pin_mux.h
)

mcux_add_include(
    INCLUDES led_blinky
)

mcux_add_source(
    SOURCES cm33_core0/app.h
            cm33_core0/hardware_init.c
)

mcux_add_include(
    INCLUDES cm33_core0
)
