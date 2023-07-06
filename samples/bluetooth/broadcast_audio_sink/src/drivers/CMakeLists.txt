#
# Copyright (c) 2022 Nordic Semiconductor
#
# SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
#

target_sources(app PRIVATE
	       ${CMAKE_CURRENT_SOURCE_DIR}/cs47l63_comm.c
)

zephyr_sources(
    ${CMAKE_CURRENT_SOURCE_DIR}/cirrus/common/regmap.c
    ${CMAKE_CURRENT_SOURCE_DIR}/cirrus/cs47l63/cs47l63.c
    ${CMAKE_CURRENT_SOURCE_DIR}/cirrus/cs47l63/bsp/bsp_cs47l63.c
    ${CMAKE_CURRENT_SOURCE_DIR}/cirrus/cs47l63/bsp/vregmap.c
    ${CMAKE_CURRENT_SOURCE_DIR}/cirrus/cs47l63/generated/cs47l63_fw_img.c
    ${CMAKE_CURRENT_SOURCE_DIR}/cirrus/cs47l63/generated/cs47l63_syscfg_regs.c
)

zephyr_include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/cirrus
    ${CMAKE_CURRENT_SOURCE_DIR}/cirrus/common
    ${CMAKE_CURRENT_SOURCE_DIR}/cirrus/common/bridge
    ${CMAKE_CURRENT_SOURCE_DIR}/cirrus/common/platform_bsp
    ${CMAKE_CURRENT_SOURCE_DIR}/cirrus/cs47l63
    ${CMAKE_CURRENT_SOURCE_DIR}/cirrus/cs47l63/bsp
    ${CMAKE_CURRENT_SOURCE_DIR}/cirrus/cs47l63/config
    ${CMAKE_CURRENT_SOURCE_DIR}/cirrus/cs47l63/fw_pt
    ${CMAKE_CURRENT_SOURCE_DIR}/cirrus/cs47l63/generated
)