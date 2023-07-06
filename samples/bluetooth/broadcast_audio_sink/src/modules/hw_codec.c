/*
 * Copyright (c) 2018 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include "hw_codec.h"

#include <zephyr/kernel.h>
#include <stdlib.h>
#include <stdint.h>
#include <ctype.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/device.h>
#include <zephyr/shell/shell.h>

#include "../drivers/cirrus/cs47l63/cs47l63.h"
#include "../drivers/cirrus/cs47l63/cs47l63_spec.h"
#include "../drivers/cs47l63_reg_conf.h"
#include "../drivers/cs47l63_comm.h"

#define BIT_SET(REG, BIT) ((REG) |= (BIT))
#define BIT_CLEAR(REG, BIT) ((REG) &= ~(BIT))

#define VOLUME_ADJUST_STEP_DB 3
#define BASE_10 10

static cs47l63_t cs47l63_driver;

/**@brief Write to multiple registers in CS47L63
 */
static int cs47l63_comm_reg_conf_write(const uint32_t config[][2], uint32_t num_of_regs)
{
	int ret;
	uint32_t reg;
	uint32_t value;

	for (int i = 0; i < num_of_regs; i++) {
		reg = config[i][0];
		value = config[i][1];

		if (reg == SPI_BUSY_WAIT) {
			/* Wait for us defined in value */
			k_busy_wait(value);
		} else {
			ret = cs47l63_write_reg(&cs47l63_driver, reg, value);
			if (ret) {
				return ret;
			}
		}
	}

	return 0;
}

int hw_codec_volume_set(uint8_t set_val)
{
	int ret;
	uint32_t volume_reg_val;

	volume_reg_val = set_val;
	if (volume_reg_val == 0) {
		printk("Volume at MIN (-64dB)\n");
	} else if (volume_reg_val >= MAX_VOLUME_REG_VAL) {
		printk("Volume at MAX (0dB)\n");
		volume_reg_val = MAX_VOLUME_REG_VAL;
	}

	ret = cs47l63_write_reg(&cs47l63_driver, CS47L63_OUT1L_VOLUME_1,
				volume_reg_val | CS47L63_OUT_VU);
	if (ret) {
		return ret;
	}
	return 0;
}

int hw_codec_volume_adjust(int8_t adjustment_db)
{
	int ret;
	static uint32_t prev_volume_reg_val = OUT_VOLUME_DEFAULT;

	printk("Adj dB in: %d\n", adjustment_db);

	if (adjustment_db == 0) {
		ret = cs47l63_write_reg(&cs47l63_driver, CS47L63_OUT1L_VOLUME_1,
					(prev_volume_reg_val | CS47L63_OUT_VU) &
						~CS47L63_OUT1L_MUTE);
		return ret;
	}

	uint32_t volume_reg_val;

	ret = cs47l63_read_reg(&cs47l63_driver, CS47L63_OUT1L_VOLUME_1, &volume_reg_val);
	if (ret) {
		return ret;
	}

	volume_reg_val &= CS47L63_OUT1L_VOL_MASK;

	/* The adjustment is in dB, 1 bit equals 0.5 dB,
	 * so multiply by 2 to get increments of 1 dB
	 */
	int32_t new_volume_reg_val = volume_reg_val + (adjustment_db * 2);

	if (new_volume_reg_val < 0) {
		printk("Volume at MIN (-64dB)\n");
		new_volume_reg_val = 0;

	} else if (new_volume_reg_val > MAX_VOLUME_REG_VAL) {
		printk("Volume at MAX (0dB)\n");
		new_volume_reg_val = MAX_VOLUME_REG_VAL;
	}

	ret = cs47l63_write_reg(&cs47l63_driver, CS47L63_OUT1L_VOLUME_1,
				((uint32_t)new_volume_reg_val | CS47L63_OUT_VU) &
					~CS47L63_OUT1L_MUTE);

	if (ret) {
		return ret;
	}

	prev_volume_reg_val = new_volume_reg_val;

	/* This is rounded down to nearest integer */
	printk("Volume: %" PRId32 " dB\n", (new_volume_reg_val / 2) - MAX_VOLUME_DB);

	return 0;
}

int hw_codec_volume_decrease(void)
{
	int ret;

	ret = hw_codec_volume_adjust(-VOLUME_ADJUST_STEP_DB);
	if (ret) {
		return ret;
	}

	return 0;
}

int hw_codec_volume_increase(void)
{
	int ret;

	ret = hw_codec_volume_adjust(VOLUME_ADJUST_STEP_DB);
	if (ret) {
		return ret;
	}

	return 0;
}

int hw_codec_volume_mute(void)
{
	int ret;
	uint32_t volume_reg_val;

	ret = cs47l63_read_reg(&cs47l63_driver, CS47L63_OUT1L_VOLUME_1, &volume_reg_val);
	if (ret) {
		return ret;
	}

	BIT_SET(volume_reg_val, CS47L63_OUT1L_MUTE_MASK);

	ret = cs47l63_write_reg(&cs47l63_driver, CS47L63_OUT1L_VOLUME_1,
				volume_reg_val | CS47L63_OUT_VU);
	if (ret) {
		return ret;
	}

	return 0;
}

int hw_codec_volume_unmute(void)
{
	int ret;
	uint32_t volume_reg_val;

	ret = cs47l63_read_reg(&cs47l63_driver, CS47L63_OUT1L_VOLUME_1, &volume_reg_val);
	if (ret) {
		return ret;
	}

	BIT_CLEAR(volume_reg_val, CS47L63_OUT1L_MUTE_MASK);

	ret = cs47l63_write_reg(&cs47l63_driver, CS47L63_OUT1L_VOLUME_1,
				volume_reg_val | CS47L63_OUT_VU);
	if (ret) {
		return ret;
	}

	return 0;
}

int hw_codec_default_conf_enable(void)
{
	int ret;

	ret = cs47l63_comm_reg_conf_write(clock_configuration, ARRAY_SIZE(clock_configuration));
	if (ret) {
		return ret;
	}

	ret = cs47l63_comm_reg_conf_write(GPIO_configuration, ARRAY_SIZE(GPIO_configuration));
	if (ret) {
		return ret;
	}

	ret = cs47l63_comm_reg_conf_write(asp1_enable, ARRAY_SIZE(asp1_enable));
	if (ret) {
		return ret;
	}

	ret = cs47l63_comm_reg_conf_write(output_enable, ARRAY_SIZE(output_enable));
	if (ret) {
		return ret;
	}

	ret = hw_codec_volume_adjust(0);
	if (ret) {
		return ret;
	}

	/* Toggle FLL to start up CS47L63 */
	ret = cs47l63_comm_reg_conf_write(FLL_toggle, ARRAY_SIZE(FLL_toggle));
	if (ret) {
		return ret;
	}

	return 0;
}

int hw_codec_soft_reset(void)
{
	int ret;

	ret = cs47l63_comm_reg_conf_write(output_disable, ARRAY_SIZE(output_disable));
	if (ret) {
		return ret;
	}

	ret = cs47l63_comm_reg_conf_write(soft_reset, ARRAY_SIZE(soft_reset));
	if (ret) {
		return ret;
	}

	return 0;
}

int hw_codec_init(void)
{
	int ret;

	ret = cs47l63_comm_init(&cs47l63_driver);
	if (ret) {
		return ret;
	}

	/* Run a soft reset on start to make sure all registers are default values */
	ret = cs47l63_comm_reg_conf_write(soft_reset, ARRAY_SIZE(soft_reset));
	if (ret) {
		return ret;
	}
	cs47l63_driver.state = CS47L63_STATE_STANDBY;

	return 0;
}