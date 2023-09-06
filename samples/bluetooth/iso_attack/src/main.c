/*
 * Copyright (c) 2021 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/iso.h>
#include <zephyr/sys/byteorder.h>

#define TIMEOUT_SYNC_CREATE K_SECONDS(10)
#define NAME_LEN            30

#define BT_LE_SCAN_CUSTOM BT_LE_SCAN_PARAM(BT_LE_SCAN_TYPE_ACTIVE, \
					   BT_LE_SCAN_OPT_NONE, \
					   BT_GAP_SCAN_FAST_INTERVAL, \
					   BT_GAP_SCAN_FAST_WINDOW)

#define PA_RETRY_COUNT 6

#define BIS_ISO_CHAN_COUNT 1

static bool         per_adv_found;
static bool         per_adv_lost;
static bt_addr_le_t per_addr;
static uint8_t      per_sid;
static uint32_t     per_interval_us;

static K_SEM_DEFINE(sem_per_adv, 0, 1);
static K_SEM_DEFINE(sem_per_sync, 0, 1);
static K_SEM_DEFINE(sem_per_sync_lost, 0, 1);
static K_SEM_DEFINE(sem_per_big_info, 0, 1);
static K_SEM_DEFINE(sem_big_sync, 0, BIS_ISO_CHAN_COUNT);
static K_SEM_DEFINE(sem_big_sync_lost, 0, BIS_ISO_CHAN_COUNT);

static const struct gpio_dt_spec button = GPIO_DT_SPEC_GET(DT_ALIAS(sw0), gpios);
static struct gpio_dt_spec led1 = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);
static struct gpio_dt_spec led2 = GPIO_DT_SPEC_GET(DT_ALIAS(led1), gpios);
static struct gpio_dt_spec led3 = GPIO_DT_SPEC_GET(DT_ALIAS(led2), gpios);
static struct gpio_dt_spec led4 = GPIO_DT_SPEC_GET(DT_ALIAS(led3), gpios);
static struct gpio_callback button_cb_data;

static uint8_t attack_on = 0;

void button_pressed(const struct device *dev, struct gpio_callback *cb,
		    uint32_t pins)
{
	gpio_pin_toggle_dt(&led1);
	gpio_pin_toggle_dt(&led2);
	gpio_pin_toggle_dt(&led3);
	gpio_pin_toggle_dt(&led4);
	attack_on ^= 1;
	printk("BISON STARTED!\n");
	
	if(!attack_on) {
		NVIC_SystemReset();
	}
}

static void scan_recv(const struct bt_le_scan_recv_info *info,
		      struct net_buf_simple *buf)
{
	if (!per_adv_found && info->interval) {
		per_adv_found = true;

		per_sid = info->sid;
		per_interval_us = BT_CONN_INTERVAL_TO_US(info->interval);
		bt_addr_le_copy(&per_addr, info->addr);

		k_sem_give(&sem_per_adv);
	}
}

static struct bt_le_scan_cb scan_callbacks = {
	.recv = scan_recv,
};

static void sync_cb(struct bt_le_per_adv_sync *sync,
		    struct bt_le_per_adv_sync_synced_info *info)
{
	k_sem_give(&sem_per_sync);
}

static void term_cb(struct bt_le_per_adv_sync *sync,
		    const struct bt_le_per_adv_sync_term_info *info)
{
	k_sem_give(&sem_per_sync_lost);
}

static void biginfo_cb(struct bt_le_per_adv_sync *sync,
		       const struct bt_iso_biginfo *biginfo)
{
	k_sem_give(&sem_per_big_info);
}

static struct bt_le_per_adv_sync_cb sync_callbacks = {
	.synced = sync_cb,
	.term = term_cb,
	.biginfo = biginfo_cb,
};

static void iso_connected(struct bt_iso_chan *chan)
{
	k_sem_give(&sem_big_sync);
}

static void iso_disconnected(struct bt_iso_chan *chan, uint8_t reason)
{
	if (reason != BT_HCI_ERR_OP_CANCELLED_BY_HOST) {
		k_sem_give(&sem_big_sync_lost);
	}
}

static struct bt_iso_chan_ops iso_ops = {
	.connected	= iso_connected,
	.disconnected	= iso_disconnected,
};

static struct bt_iso_chan_io_qos iso_rx_qos[BIS_ISO_CHAN_COUNT];

static struct bt_iso_chan_qos bis_iso_qos[] = {
	{ .rx = &iso_rx_qos[0], },
	{ .rx = &iso_rx_qos[1], },
};

static struct bt_iso_chan bis_iso_chan[] = {
	{ .ops = &iso_ops,
	  .qos = &bis_iso_qos[0], },
	{ .ops = &iso_ops,
	  .qos = &bis_iso_qos[1], },
};

static struct bt_iso_chan *bis[] = {
	&bis_iso_chan[0],
	&bis_iso_chan[1],
};

static struct bt_iso_big_sync_param big_sync_param = {
	.bis_channels = bis,
	.num_bis = BIS_ISO_CHAN_COUNT,
	.bis_bitfield = (BIT_MASK(BIS_ISO_CHAN_COUNT) << 1),
	.mse = 1,
	.sync_timeout = 100, /* in 10 ms units */
};

void main(void)
{
	struct bt_le_per_adv_sync_param sync_create_param;
	struct bt_le_per_adv_sync *sync;
	struct bt_iso_big *big;
	uint32_t sem_timeout_us;
	int err;

	printk("Hello, I'm Mallory ...\n");
	printk("PRESS START ...\n");

	/* Initialize the LEDs and the button */
	if (!gpio_is_ready_dt(&button) || !gpio_is_ready_dt(&led1) || !gpio_is_ready_dt(&led2) || 
		!gpio_is_ready_dt(&led3) || !gpio_is_ready_dt(&led4)) {
		printk("Error: button or leds not ready\n");
		return;
	}

	err = gpio_pin_configure_dt(&button, GPIO_INPUT);
	err |= gpio_pin_configure_dt(&led1, GPIO_OUTPUT_INACTIVE);
	err |= gpio_pin_configure_dt(&led2, GPIO_OUTPUT_INACTIVE);
	err |= gpio_pin_configure_dt(&led3, GPIO_OUTPUT_INACTIVE);
	err |= gpio_pin_configure_dt(&led4, GPIO_OUTPUT_INACTIVE);
	if (err != 0) {
		printk("Error %d: failed to configure button or leds\n", err);
		return;
	}

	err = gpio_pin_interrupt_configure_dt(&button, GPIO_INT_EDGE_TO_ACTIVE);
	if (err != 0) {
		printk("Error %d: failed to configure interrupt for button\n", err);
		return;
	}

	gpio_init_callback(&button_cb_data, button_pressed, BIT(button.pin));
	gpio_add_callback(button.port, &button_cb_data);

	/* Initialize the Bluetooth Subsystem */
	err = bt_enable(NULL);
	if (err) {
		printk("Bluetooth init failed (err %d)\n", err);
		return;
	}

	bt_le_scan_cb_register(&scan_callbacks);
	bt_le_per_adv_sync_cb_register(&sync_callbacks);

	while(!attack_on) {
		k_sleep(K_MSEC(1));
	}

	do {
		per_adv_lost = false;

		err = bt_le_scan_start(BT_LE_SCAN_CUSTOM, NULL);
		if (err) {
			printk("Scanning failed to start (err %d)\n", err);
			return;
		}

		per_adv_found = false;
		err = k_sem_take(&sem_per_adv, K_FOREVER);
		if (err) {
			printk("Waiting for per. adv. failed (err %d)\n", err);
			return;
		}

		err = bt_le_scan_stop();
		if (err) {
			printk("Stopping scanning failed (err %d)\n", err);
			return;
		}

		bt_addr_le_copy(&sync_create_param.addr, &per_addr);
		sync_create_param.options = 0;
		sync_create_param.sid = per_sid;
		sync_create_param.skip = 0;
		/* Multiple PA interval with retry count and convert to unit of 10 ms */
		sync_create_param.timeout = (per_interval_us * PA_RETRY_COUNT) /
						(10 * USEC_PER_MSEC);
		sem_timeout_us = per_interval_us * PA_RETRY_COUNT;
		err = bt_le_per_adv_sync_create(&sync_create_param, &sync);
		if (err) {
			printk("Creating per. adv. sync failed (err %d)\n", err);
			return;
		}

		err = k_sem_take(&sem_per_sync, K_USEC(sem_timeout_us));
		if (err) {
			printk("Waiting for per. adv. sync failed (err %d)\n", err);

			err = bt_le_per_adv_sync_delete(sync);
			if (err) {
				printk("Deleting per. adv. sync failed (err %d)\n", err);
				return;
			}
			continue;
		}

		err = k_sem_take(&sem_per_big_info, K_USEC(sem_timeout_us));
		if (err) {
			printk("Waiting for BIG info failed (err %d)\n", err);

			if (per_adv_lost) {
				continue;
			}

			err = bt_le_per_adv_sync_delete(sync);
			if (err) {
				printk("Deleting per. adv. sync failed (err %d)\n", err);
				return;
			}
			continue;
		}

big_sync_create:
		err = bt_iso_big_sync(sync, &big_sync_param, &big);
		if (err) {
			printk("Create BIG Sync failed (err %d)\n", err);
			return;
		}

		for (uint8_t chan = 0U; chan < BIS_ISO_CHAN_COUNT; chan++) {
			err = k_sem_take(&sem_big_sync, TIMEOUT_SYNC_CREATE);
			if (err) {
				break;
			}
		}
		if (err) {
			printk("Waiting for BIG sync failed (err %d)\n", err);

			err = bt_iso_big_terminate(big);
			if (err) {
				printk("BIG Sync Terminate failed (err %d)\n", err);
				return;
			}

			goto per_sync_lost_check;
		}

		for (uint8_t chan = 0U; chan < BIS_ISO_CHAN_COUNT; chan++) {
			err = k_sem_take(&sem_big_sync_lost, K_FOREVER);
			if (err) {
				printk("Waiting for BIG sync lost failed (err %d)\n", err);
				return;
			}
		}

per_sync_lost_check:
		err = k_sem_take(&sem_per_sync_lost, K_NO_WAIT);
		if (err) {
			/* Periodic Sync active, go back to creating BIG Sync */
			goto big_sync_create;
		}
	} while (true);
}
