cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(zmk_ble_diag)

target_sources(app PRIVATE app/src/main.c)
