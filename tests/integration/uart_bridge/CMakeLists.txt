cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})

project(uart_bridge_test)

# The real uart_bridge.c is the device under test -- not a copy of it. It has no
# BT dependency by design, which is what makes this possible.
#
# drop_stats.c and uart_rx_retry.c come along because uart_bridge.c calls into
# them for the drop counters and the RX-restart retry; they are pure helpers with
# no Zephyr dependency, so they add nothing to the suite's config.
target_sources(app PRIVATE
  src/main.c
  ${CMAKE_CURRENT_SOURCE_DIR}/../../../src/uart_bridge.c
  ${CMAKE_CURRENT_SOURCE_DIR}/../../../src/drop_stats.c
  ${CMAKE_CURRENT_SOURCE_DIR}/../../../src/uart_rx_retry.c
)
target_include_directories(app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../../src)
