#
# Copyright (c) 2022 Nordic Semiconductor
#
# SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
#

target_sources(app PRIVATE
	       ${CMAKE_CURRENT_SOURCE_DIR}/audio_i2s.c
		   ${CMAKE_CURRENT_SOURCE_DIR}/hw_codec.c
)
