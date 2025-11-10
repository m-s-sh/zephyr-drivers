# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2025 Blue Vending

include($ENV{ZEPHYR_BASE}/cmake/modules/extensions.cmake)

add_subdirectory_ifdef(CONFIG_STATUS_LED led)
add_subdirectory_ifdef(CONFIG_MODEM_SIM800L modem)
