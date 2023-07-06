/*
 * Copyright (c) 2022 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: Apache-2.0
 * 
 * This file contains parts of the Zephyr RTOS repository
 * as well as the nrf-sdk repository on GitHub.
 * 
 */

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/audio/audio.h>
#include <zephyr/bluetooth/audio/pacs.h>
#include <zephyr/ipc/ipc_service.h>
#include <zephyr/drivers/gpio.h>
#include "audio_i2s.h"
#include "hw_codec.h"
#include "lc3.h"

#define SEM_TIMEOUT K_SECONDS(10)
#define CHANNEL_MAP_SIZE 5

#define NEXT_IDX(i) (((i) < (80 - 1)) ? ((i) + 1) : 0)
#define PREV_IDX(i) (((i) > 0) ? ((i)-1) : (80 - 1))

#define BLK_PERIOD_US 1000
#define NUM_BLKS(d) ((d) / BLK_PERIOD_US)
#define BLK_SIZE_SAMPLES(r) (((r)*BLK_PERIOD_US) / 1000000)
#define NUM_BLKS_IN_FRAME NUM_BLKS(CONFIG_AUDIO_FRAME_DURATION_US)
#define BLK_MONO_NUM_SAMPS BLK_SIZE_SAMPLES(CONFIG_AUDIO_SAMPLE_RATE_HZ)
#define BLK_STEREO_NUM_SAMPS (BLK_MONO_NUM_SAMPS * 2)
#define BLK_MONO_SIZE_OCTETS (BLK_MONO_NUM_SAMPS * CONFIG_AUDIO_BIT_DEPTH_OCTETS)
#define BLK_STEREO_SIZE_OCTETS (BLK_MONO_SIZE_OCTETS * 2)

#define FRAME_SIZE_BYTES_MONO 160 * 2 // TODO: change to codec setting
#define FRAME_SIZE_BYTES_STEREO FRAME_SIZE_BYTES_MONO * 2

static K_SEM_DEFINE(sem_broadcaster_found, 0U, 1U);
static K_SEM_DEFINE(sem_pa_synced, 0U, 1U);
static K_SEM_DEFINE(sem_base_received, 0U, 1U);
static K_SEM_DEFINE(sem_syncable, 0U, 1U);
static K_SEM_DEFINE(sem_pa_sync_lost, 0U, 1U);

static struct bt_audio_broadcast_sink *broadcast_sink;
static struct bt_audio_stream streams[CONFIG_BT_AUDIO_BROADCAST_SNK_STREAM_COUNT];
static struct bt_codec codec = BT_CODEC_LC3_CONFIG_16_2(BT_AUDIO_LOCATION_FRONT_LEFT,
							BT_AUDIO_CONTEXT_TYPE_UNSPECIFIED);
static const uint32_t bis_index_mask = BIT_MASK(ARRAY_SIZE(streams) + 1U);
static uint32_t bis_index_bitfield;
static lc3_decoder_t lc3_decoder;
static lc3_decoder_mem_16k_t lc3_decoder_mem;
static int frames_per_sdu;
static int16_t audio_buf[160];
static int32_t start_i2s_out = 10;
static int16_t __aligned(sizeof(uint32_t)) fifo[2560];
static uint16_t prod_blk_idx = 0;
static uint16_t cons_blk_idx = 0;
static bool streaming = false;

static const struct gpio_dt_spec led_blue = GPIO_DT_SPEC_GET(DT_ALIAS(led2), gpios);
static const struct gpio_dt_spec led_green = GPIO_DT_SPEC_GET(DT_ALIAS(led1), gpios);

static void bound_cb(void *priv) {}
static void recv_cb(const void *data, size_t len, void *priv)
{
	if (len == CHANNEL_MAP_SIZE) {
		uint8_t *channel_map = (uint8_t*)data;
		printk("ChM: %x%x%x%x%x\n", channel_map[0], channel_map[1], channel_map[2], channel_map[3], channel_map[4]);

		if (channel_map[0] != 0xff) {
			gpio_pin_set_dt(&led_blue, 0);
			gpio_pin_set_dt(&led_green, 1);
		}
	}
}

static struct ipc_ept_cfg ept0_cfg = {
   .name = "ept0",
   .cb = {
      .bound    = bound_cb,
      .received = recv_cb,
   },
};

static void stream_started_cb(struct bt_audio_stream *stream)
{
	printk("Stream %p started\n", stream);
	gpio_pin_set_dt(&led_blue, 1);
	streaming = true;
}

static void stream_stopped_cb(struct bt_audio_stream *stream)
{
	printk("Stream %p stopped\n", stream);
	gpio_pin_set_dt(&led_blue, 0);
	gpio_pin_set_dt(&led_green, 0);
	streaming = false;
}

int pscm_zero_pad(void const *const input, size_t input_size,
		  uint8_t pcm_bit_depth, void *output, size_t *output_size)
{
	uint8_t bytes_per_sample = pcm_bit_depth / 8;

	char *pointer_input = (char *)input;
	char *pointer_output = (char *)output;

	for (uint32_t i = 0; i < input_size / bytes_per_sample; i++) {
		for (uint8_t j = 0; j < bytes_per_sample; j++) {
			*pointer_output++ = *pointer_input++;
		}

		for (uint8_t j = 0; j < bytes_per_sample; j++) {
			*pointer_output++ = 0;
		}
	}

	*output_size = input_size * 2;
	return 0;
}

static void stream_recv_cb(struct bt_audio_stream *stream,
			   const struct bt_iso_recv_info *info,
			   struct net_buf *buf)
{
	const uint8_t *in_buf;
	uint8_t err = -1;
	int octets_per_frame = buf->len / frames_per_sdu; // 40

	if (lc3_decoder == NULL) {
		printk("LC3 decoder not setup, cannot decode data.\n");
		return;
	}

	if (!(info->flags & BT_ISO_FLAGS_VALID)) {
		in_buf = NULL;
	} else {
		in_buf = buf->data;
	}

	memset(audio_buf, 0, FRAME_SIZE_BYTES_MONO);
	err = lc3_decode(lc3_decoder, in_buf, octets_per_frame, LC3_PCM_FORMAT_S16, audio_buf, 1);
	if (err < 0) { // 1 = PLC performed
		printk("LC3 decoder failed.\n");
	}

	static int16_t pcm_data_stereo[FRAME_SIZE_BYTES_STEREO];
	memset(pcm_data_stereo, 0, FRAME_SIZE_BYTES_STEREO);
	size_t pcm_size_stereo = 0;
	err = pscm_zero_pad(audio_buf, FRAME_SIZE_BYTES_MONO, CONFIG_AUDIO_BIT_DEPTH_BITS,
				pcm_data_stereo, &pcm_size_stereo);
	if (err) {
		printk("PSCM zero pad failed.\n");
		return;
	}

	for (uint32_t i = 0; i < NUM_BLKS_IN_FRAME; i++) {
		if (IS_ENABLED(CONFIG_AUDIO_BIT_DEPTH_16)) {
			memcpy(&fifo[prod_blk_idx * BLK_STEREO_NUM_SAMPS],
			       &((int16_t *)pcm_data_stereo)[i * BLK_STEREO_NUM_SAMPS],
			       BLK_STEREO_SIZE_OCTETS);
		} else if (IS_ENABLED(CONFIG_AUDIO_BIT_DEPTH_32)) {
			memcpy(&fifo[prod_blk_idx * BLK_STEREO_NUM_SAMPS],
			       &((int32_t *)pcm_data_stereo)[i * BLK_STEREO_NUM_SAMPS],
			       BLK_STEREO_SIZE_OCTETS);
		}
		prod_blk_idx = NEXT_IDX(prod_blk_idx);
	}

	if (start_i2s_out == 0) {
		static int16_t tx_buf_one[32] = { 0 };
		static int16_t tx_buf_two[32] = { 0 };
		audio_i2s_start((uint8_t *)tx_buf_one, NULL);
		audio_i2s_set_next_buf((uint8_t *)tx_buf_two, NULL);
	} else {
		start_i2s_out--;
	}
}

static struct bt_audio_stream_ops stream_ops = {
	.started = stream_started_cb,
	.stopped = stream_stopped_cb,
	.recv = stream_recv_cb
};

static bool scan_recv_cb(const struct bt_le_scan_recv_info *info,
			 struct net_buf_simple *ad,
			 uint32_t broadcast_id)
{
	k_sem_give(&sem_broadcaster_found);

	return true;
}

static void scan_term_cb(int err)
{
	if (err != 0) {
		printk("Scan terminated with error: %d\n", err);
	}
}

static void pa_synced_cb(struct bt_audio_broadcast_sink *sink,
			 struct bt_le_per_adv_sync *sync,
			 uint32_t broadcast_id)
{
	if (broadcast_sink != NULL) {
		printk("Unexpected PA sync\n");
		return;
	}

	printk("PA synced for broadcast sink %p with broadcast ID 0x%06X\n",
	       sink, broadcast_id);

	broadcast_sink = sink;

	k_sem_give(&sem_pa_synced);
}

static void base_recv_cb(struct bt_audio_broadcast_sink *sink,
			 const struct bt_audio_base *base)
{
	uint32_t base_bis_index_bitfield = 0U;

	if (k_sem_count_get(&sem_base_received) != 0U) {
		return;
	}

	printk("Received BASE with %u subgroups from broadcast sink %p\n",
	       base->subgroup_count, sink);

	for (size_t i = 0U; i < base->subgroup_count; i++) {
		for (size_t j = 0U; j < base->subgroups[i].bis_count; j++) {
			const uint8_t index = base->subgroups[i].bis_data[j].index;

			base_bis_index_bitfield |= BIT(index);
		}
	}

	bis_index_bitfield = base_bis_index_bitfield & bis_index_mask;

	k_sem_give(&sem_base_received);
}

static void syncable_cb(struct bt_audio_broadcast_sink *sink, bool encrypted)
{
	if (encrypted) {
		printk("Cannot sync to encrypted broadcast source\n");
		return;
	}

	k_sem_give(&sem_syncable);
}

static void pa_sync_lost_cb(struct bt_audio_broadcast_sink *sink)
{
	if (broadcast_sink == NULL) {
		printk("Unexpected PA sync lost\n");
		return;
	}

	printk("Sink %p disconnected\n", sink);

	broadcast_sink = NULL;

	streaming = false;
}

static struct bt_audio_broadcast_sink_cb broadcast_sink_cbs = {
	.scan_recv = scan_recv_cb,
	.scan_term = scan_term_cb,
	.base_recv = base_recv_cb,
	.syncable = syncable_cb,
	.pa_synced = pa_synced_cb,
	.pa_sync_lost = pa_sync_lost_cb
};

static struct bt_pacs_cap cap = {
	.codec = &codec,
};

static int init(void)
{
	int err;

	err = bt_enable(NULL);
	if (err) {
		printk("Bluetooth enable failed (err %d)\n", err);
		return err;
	}

	printk("Bluetooth initialized\n");

	err = bt_pacs_cap_register(BT_AUDIO_DIR_SINK, &cap);
	if (err) {
		printk("Capability register failed (err %d)\n", err);
		return err;
	}

	bt_audio_broadcast_sink_register_cb(&broadcast_sink_cbs);

	for (size_t i = 0U; i < ARRAY_SIZE(streams); i++) {
		streams[i].ops = &stream_ops;
	}

	return 0;
}

static void reset(void)
{
	int err;

	bis_index_bitfield = 0U;

	k_sem_reset(&sem_broadcaster_found);
	k_sem_reset(&sem_pa_synced);
	k_sem_reset(&sem_base_received);
	k_sem_reset(&sem_syncable);
	k_sem_reset(&sem_pa_sync_lost);

	if (broadcast_sink != NULL) {
		err = bt_audio_broadcast_sink_delete(broadcast_sink);
		if (err) {
			printk("Deleting broadcast sink failed (err %d)\n", err);
			return;
		}

		broadcast_sink = NULL;
	}
}

static void init_lc3(void)
{
	const int freq = bt_codec_cfg_get_freq(&codec);
	const int frame_duration_us = bt_codec_cfg_get_frame_duration_us(&codec);

	if (freq < 0) {
		printk("Error: Codec frequency not set, cannot start codec.");
	}

	if (frame_duration_us < 0) {
		printk("Error: Frame duration not set, cannot start codec.");
	}

	frames_per_sdu = bt_codec_cfg_get_frame_blocks_per_sdu(&codec, true);

	lc3_decoder = lc3_setup_decoder(frame_duration_us, freq,
					freq, &lc3_decoder_mem);

	if (lc3_decoder == NULL) {
		printk("ERROR: Failed to setup LC3 encoder - wrong parameters?\n");
	}
}

static void audio_datapath_i2s_blk_complete(uint32_t frame_start_ts, uint32_t *rx_buf_released,
					    uint32_t const *tx_buf_released)
{
	if (streaming) {
		static uint8_t *tx_buf;
		uint32_t next_out_blk_idx = NEXT_IDX(cons_blk_idx);
		tx_buf = (uint8_t *)&fifo[next_out_blk_idx * 32];
		audio_i2s_set_next_buf(tx_buf, NULL);
		cons_blk_idx = next_out_blk_idx;
	} else {
		hw_codec_soft_reset();
	}
}

void main(void)
{
	struct bt_audio_stream *streams_p[ARRAY_SIZE(streams)];
	const struct device *inst0;
	struct ipc_ept ept0;
	int err;

	inst0 = DEVICE_DT_GET(DT_NODELABEL(ipc0));
	err = ipc_service_open_instance(inst0);
	err = ipc_service_register_endpoint(inst0, &ept0, &ept0_cfg);

	err = init();
	if (err) {
		printk("Init failed (err %d)\n", err);
		return;
	}

	if (!gpio_is_ready_dt(&led_blue) || !gpio_is_ready_dt(&led_green)) {
		printk("Init LEDs failed (err %d)\n", err);
		return;
	}

	err = gpio_pin_configure_dt(&led_blue, GPIO_OUTPUT_INACTIVE);
	err |= gpio_pin_configure_dt(&led_green, GPIO_OUTPUT_INACTIVE);
	if (err < 0) {
		return;
	}

	init_lc3();
	audio_i2s_blk_comp_cb_register(audio_datapath_i2s_blk_complete);
	audio_i2s_init();

	err = hw_codec_init();
	if (err) {
		printk("hw_codec_init failed\n");
	}

	err = hw_codec_default_conf_enable();
	if (err) {
		printk("hw_codec_default_conf_enable failed\n");
	}

	static int16_t tx_buf_one[32] = { 0 };
	static int16_t tx_buf_two[32] = { 0 };
	audio_i2s_start((uint8_t *)tx_buf_one, NULL);
	audio_i2s_set_next_buf((uint8_t *)tx_buf_two, NULL);

	for (size_t i = 0U; i < ARRAY_SIZE(streams_p); i++) {
		streams_p[i] = &streams[i];
	}

	while (true) {
		reset();

		printk("Scanning for broadcast sources\n");
		err = bt_audio_broadcast_sink_scan_start(BT_LE_SCAN_ACTIVE);
		if (err != 0) {
			printk("Unable to start scan for broadcast sources: %d\n",
			       err);
			return;
		}

		/* TODO: Update K_FOREVER with a sane value, and handle error */
		err = k_sem_take(&sem_broadcaster_found, SEM_TIMEOUT);
		if (err != 0) {
			printk("sem_broadcaster_found timed out, resetting\n");
			continue;
		}
		printk("Broadcast source found, waiting for PA sync\n");

		err = k_sem_take(&sem_pa_synced, SEM_TIMEOUT);
		if (err != 0) {
			printk("sem_pa_synced timed out, resetting\n");
			continue;
		}
		printk("Broadcast source PA synced, waiting for BASE\n");

		err = k_sem_take(&sem_base_received, SEM_TIMEOUT);
		if (err != 0) {
			printk("sem_base_received timed out, resetting\n");
			continue;
		}
		printk("BASE received, waiting for syncable\n");

		err = k_sem_take(&sem_syncable, SEM_TIMEOUT);
		if (err != 0) {
			printk("sem_syncable timed out, resetting\n");
			continue;
		}

		printk("Syncing to broadcast\n");
		err = bt_audio_broadcast_sink_sync(broadcast_sink,
						   bis_index_bitfield,
						   streams_p,
						   NULL);
		if (err != 0) {
			printk("Unable to sync to broadcast source: %d\n", err);
			return;
		}

		printk("Waiting for PA disconnected\n");
		k_sem_take(&sem_pa_sync_lost, K_FOREVER);
	}
}
