cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})

project(proxy_core_identity_test)

# proxy_core.c is compiled straight into the suite: it has no Zephyr dependency
# (see src/proxy_core.h), so it needs no library packaging and no BT config --
# not even for the address, which crosses as plain bytes rather than bt_addr_le_t.
target_sources(app PRIVATE
  src/main.c
  ${CMAKE_CURRENT_SOURCE_DIR}/../../../src/proxy_core.c
)
target_include_directories(app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../../src)
