#pico-zephyr-project
# Copyright (c) 2025 Blue Vending
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})

add_subdirectory(drivers)

zephyr_syscall_include_directories(include)
zephyr_include_directories(include)
