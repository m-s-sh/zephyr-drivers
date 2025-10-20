# Copyright (c) 2025 Blue Vending
# SPDX-License-Identifier: Apache-2.0

# Only build if CONFIG_MODEM_SIM800L is enabled
if(CONFIG_MODEM_SIM800L)
  # Declares the current directory as a Zephyr library
  zephyr_library()

  # List the source code files for the library
  zephyr_library_sources(sim800l.c)
endif()
