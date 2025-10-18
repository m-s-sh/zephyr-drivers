# Copyright (c) 2025 Blue Vending
# SPDX-License-Identifier: Apache-2.0

# Declares the current directory as a Zephyr library
# If no name is given, the name is derived from the directory name
zephyr_library()

# List the source code files for the library
zephyr_library_sources(sim800l.c)
