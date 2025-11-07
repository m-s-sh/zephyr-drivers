# Copyright (c) 2025 Blue Vending
# SPDX-License-Identifier: Apache-2.0

# Only build if CONFIG_MODEM_SIM800L is enabled
if(CONFIG_MODEM_SIM800L)
  # Declares the current directory as a Zephyr library
  zephyr_library()

  zephyr_library_include_directories(${ZEPHYR_BASE}/drivers/modem)
  zephyr_library_include_directories(${ZEPHYR_BASE}/subsys/net/ip)
  zephyr_library_include_directories(${ZEPHYR_BASE}/subsys/net/lib/sockets)

  # List the source code files for the library
  zephyr_library_sources(
    sim800l.c
    sim800l_pdp.c
    sim800l_offload.c

    # sim800l_at_cmd.c
  )
endif()
