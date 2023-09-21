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
#include <zephyr/storage/disk_access.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/fs/fs.h>
#include <lc3.h>
#include <ff.h>

#define NUM_MUSIC_FILES 2
static char* music_filenames[NUM_MUSIC_FILES] = {"MUSIC1.RAW", "MUSIC2.RAW"};

#define DISK_DRIVE_NAME "SD"
#define DISK_MOUNT_PT "/"DISK_DRIVE_NAME":"
#define MAX_PATH 128

static struct bt_audio_lc3_preset preset_16_2_1 =
	BT_AUDIO_LC3_BROADCAST_PRESET_16_2_1(BT_AUDIO_LOCATION_FRONT_LEFT,
					     BT_AUDIO_CONTEXT_TYPE_UNSPECIFIED);
static struct bt_audio_stream streams[CONFIG_BT_AUDIO_BROADCAST_SRC_STREAM_COUNT];
static struct bt_audio_broadcast_source *broadcast_source;
NET_BUF_POOL_FIXED_DEFINE(tx_pool,
			  CONFIG_BT_AUDIO_BROADCAST_SRC_STREAM_COUNT,
			  BT_ISO_SDU_BUF_SIZE(CONFIG_BT_ISO_TX_MTU), 8, NULL);

static K_SEM_DEFINE(sem_started, 0U, ARRAY_SIZE(streams));
static K_SEM_DEFINE(sem_stopped, 0U, ARRAY_SIZE(streams));

static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(DT_ALIAS(led2), gpios);
static const struct gpio_dt_spec button4 = GPIO_DT_SPEC_GET_OR(DT_ALIAS(sw3), gpios, {0});
static const struct gpio_dt_spec button5 = GPIO_DT_SPEC_GET_OR(DT_ALIAS(sw4), gpios, {0});
static struct gpio_callback button4_cb_data;
static struct gpio_callback button5_cb_data;

static lc3_encoder_t lc3_encoder;
static lc3_encoder_mem_16k_t lc3_encoder_mem;
static uint16_t seq_num;
static bool stopping;

static FATFS fat_fs;
static struct fs_mount_t mp = {
	.type = FS_FATFS,
	.fs_data = &fat_fs,
};

static const char *disk_mount_pt = DISK_MOUNT_PT;
static int16_t audio_data[160] = {0};

static struct k_work_delayable audio_send_work;
struct musicFile {
  uint64_t audio_file_size;
  uint64_t fs_seek_offset;
  struct fs_file_t f_entry;
  uint8_t sd_data[160*2];
};

static struct musicFile musicFiles[2];
static uint8_t music_file_idx = 0;

static int lsdir(const char *path)
{
	int res;
	struct fs_dir_t dirp;
	static struct fs_dirent entry;
	int count = 0;

	fs_dir_t_init(&dirp);

	res = fs_opendir(&dirp, path);
	if (res) {
		printk("Error opening dir %s [%d]\n", path, res);
		return res;
	}
	
	printk("\nListing dir %s ...\n", path);
	for (;;) {
		res = fs_readdir(&dirp, &entry);

		if (res || entry.name[0] == 0) {
			break;
		}

		if (entry.type == FS_DIR_ENTRY_DIR) {
			printk("[DIR ] %s\n", entry.name);
		} else {
			for(uint8_t i = 0; i < NUM_MUSIC_FILES; i++) {
				if(strcmp(entry.name, music_filenames[i]) == 0) {
					musicFiles[i].audio_file_size = entry.size;
				}
			} 
			printk("[FILE] %s (size = %zu)\n",
				entry.name, entry.size);
		}
		count++;
	}

	fs_closedir(&dirp);
	if (res == 0) {
		res = count;
	}

	return res;
}

int sd_card_read(char *const data, uint64_t off, size_t *size, struct fs_file_t *f_entry)
{
	int ret;

	ret = fs_seek(f_entry, off, FS_SEEK_SET);
	if (ret < 0) {
		printk("Seek failed\n");
		return ret;
	}

	ret = fs_read(f_entry, data, *size);
	if (ret < 0) {
		printk("Read file failed\n");
		return ret;
	}

	*size = ret;
	if (*size == 0) {
		printk("File is empty\n");
	}

	return 0;
}

void button5_pressed(const struct device *dev, struct gpio_callback *cb,
		    uint32_t pins)
{
	music_file_idx = 0;
}

void button4_pressed(const struct device *dev, struct gpio_callback *cb,
		    uint32_t pins)
{
	music_file_idx = 1;
}

static void stream_started_cb(struct bt_audio_stream *stream)
{
	k_sem_give(&sem_started);
}

static void stream_stopped_cb(struct bt_audio_stream *stream)
{
	k_sem_give(&sem_stopped);
}

static void lc3_audio_timer_timeout(struct k_work *work)
{
	struct net_buf *buf;
	uint8_t *net_buffer;
	int ret;

	k_work_schedule(&audio_send_work, K_USEC(preset_16_2_1.qos.interval));

	if (lc3_encoder == NULL) {
		printk("LC3 encoder not setup, cannot encode data.\n");
		return;
	}

	size_t data_size = 160*2; // TODO: change to codec setting

	sd_card_read(musicFiles[music_file_idx].sd_data, musicFiles[music_file_idx].fs_seek_offset, 
		&data_size, &musicFiles[music_file_idx].f_entry);
	musicFiles[music_file_idx].fs_seek_offset += data_size;

	if (musicFiles[music_file_idx].fs_seek_offset >= musicFiles[music_file_idx].audio_file_size) {
		musicFiles[music_file_idx].fs_seek_offset = 0;
	}

	for (size_t i = 0; i < data_size / 2; i++) {
		audio_data[i] = ((int16_t)musicFiles[music_file_idx].sd_data[(2*i)+1] << 8) | musicFiles[music_file_idx].sd_data[2*i];
	}

	buf = net_buf_alloc(&tx_pool, K_FOREVER);
	net_buf_reserve(buf, BT_ISO_CHAN_SEND_RESERVE);
	net_buffer = net_buf_tail(buf);
	buf->len += preset_16_2_1.qos.sdu;

	int lc3_ret;
	lc3_ret = lc3_encode(lc3_encoder, LC3_PCM_FORMAT_S16,
					audio_data, 1, preset_16_2_1.qos.sdu,
					net_buffer);
	if (lc3_ret == -1) {
		printk("LC3 encoder failed - wrong parameters?: %d",
			lc3_ret);
		return;
	}

	ret = bt_audio_stream_send(&streams[0], buf, seq_num++,
				   BT_ISO_TIMESTAMP_NONE);
	if (ret < 0) {
		printk("Unable to broadcast data on %p: %d\n", &streams[0], ret);
		net_buf_unref(buf);
		return;
	}
}

static struct bt_audio_stream_ops stream_ops = {
	.started = stream_started_cb,
	.stopped = stream_stopped_cb
};

static int setup_broadcast_source(struct bt_audio_broadcast_source **source)
{
	struct bt_audio_broadcast_source_stream_param
		stream_params[CONFIG_BT_AUDIO_BROADCAST_SRC_STREAM_COUNT];
	struct bt_audio_broadcast_source_subgroup_param
		subgroup_param[CONFIG_BT_AUDIO_BROADCAST_SRC_SUBGROUP_COUNT];
	struct bt_audio_broadcast_source_create_param create_param;
	const size_t streams_per_subgroup = ARRAY_SIZE(stream_params) / ARRAY_SIZE(subgroup_param);
	int err;

	(void)memset(streams, 0, sizeof(streams));

	for (size_t i = 0U; i < ARRAY_SIZE(subgroup_param); i++) {
		subgroup_param[i].params_count = streams_per_subgroup;
		subgroup_param[i].params = stream_params + i * streams_per_subgroup;
		subgroup_param[i].codec = &preset_16_2_1.codec;
	}

	for (size_t j = 0U; j < ARRAY_SIZE(stream_params); j++) {
		stream_params[j].stream = &streams[j];
		stream_params[j].data = NULL;
		stream_params[j].data_count = 0U;
		bt_audio_stream_cb_register(stream_params[j].stream, &stream_ops);
	}

	create_param.params_count = ARRAY_SIZE(subgroup_param);
	create_param.params = subgroup_param;
	create_param.qos = &preset_16_2_1.qos;
	create_param.encryption = false;
	create_param.packing = BT_ISO_PACKING_SEQUENTIAL;

	printk("Creating broadcast source with %zu subgroups with %zu streams\n",
	       ARRAY_SIZE(subgroup_param),
	       ARRAY_SIZE(subgroup_param) * streams_per_subgroup);

	err = bt_audio_broadcast_source_create(&create_param, source);
	if (err != 0) {
		printk("Unable to create broadcast source: %d\n", err);
		return err;
	}

	return 0;
}

static void init_lc3(void)
{
	int frame_duration_us, freq_hz;

	freq_hz = bt_codec_cfg_get_freq(&preset_16_2_1.codec);
	frame_duration_us = bt_codec_cfg_get_frame_duration_us(&preset_16_2_1.codec);

	if (freq_hz < 0) {
		printk("Error: Codec frequency not set, cannot start codec.");
		return;
	}

	if (frame_duration_us < 0) {
		printk("Error: Frame duration not set, cannot start codec.");
		return;
	}

	lc3_encoder = lc3_setup_encoder(frame_duration_us,
					freq_hz, 0, &lc3_encoder_mem);

	if (lc3_encoder == NULL) {
		printk("ERROR: Failed to setup LC3 encoder - wrong parameters?\n");
	}
}

void main(void)
{
	struct bt_le_ext_adv *adv;
	int err;

	if (!gpio_is_ready_dt(&led) || !gpio_is_ready_dt(&button4) || !gpio_is_ready_dt(&button5)) {
		printk("Error LED or Buttons not ready.\n");
		return;
	}

	err = gpio_pin_configure_dt(&led, GPIO_OUTPUT_INACTIVE);
	err |= gpio_pin_configure_dt(&button4, GPIO_INPUT);
	err |= gpio_pin_configure_dt(&button5, GPIO_INPUT);
	if (err < 0) {
		return;
	}

	err = gpio_pin_interrupt_configure_dt(&button4, GPIO_INT_EDGE_TO_ACTIVE);
	err |= gpio_pin_interrupt_configure_dt(&button5, GPIO_INT_EDGE_TO_ACTIVE);
	if (err != 0) {
		return;
	}

	gpio_init_callback(&button4_cb_data, button4_pressed, BIT(button4.pin));
	gpio_init_callback(&button5_cb_data, button5_pressed, BIT(button5.pin));
	gpio_add_callback(button4.port, &button4_cb_data);
	gpio_add_callback(button5.port, &button5_cb_data);

	mp.mnt_point = disk_mount_pt;

	if (fs_mount(&mp) == FR_OK) {
		lsdir(disk_mount_pt); // lsdir + extract filesize
	} else {
		printk("Error mounting disk.\n");
		return;
	}

	init_lc3();

	// Open music files on SD
	for(uint8_t i = 0; i < NUM_MUSIC_FILES; i++) {
		char abs_path_name[MAX_PATH + 1] = DISK_MOUNT_PT;
		strcat(abs_path_name, "/");
		strcat(abs_path_name, music_filenames[i]);
		fs_file_t_init(&musicFiles[i].f_entry);

		err = fs_open(&musicFiles[i].f_entry, abs_path_name, FS_O_READ);
		if (err) {
			printk("Open file failed\n");
			return;
		}
	} 
	
	k_work_init_delayable(&audio_send_work, lc3_audio_timer_timeout);

	err = bt_enable(NULL);
	if (err) {
		printk("Bluetooth init failed (err %d)\n", err);
		return;
	}
	printk("Bluetooth initialized\n");

	while (true) {
		/* Broadcast Audio Streaming Endpoint advertising data */
		NET_BUF_SIMPLE_DEFINE(ad_buf,
				      BT_UUID_SIZE_16 + BT_AUDIO_BROADCAST_ID_SIZE);
		NET_BUF_SIMPLE_DEFINE(base_buf, 128);
		struct bt_data ext_ad;
		struct bt_data per_ad;
		uint32_t broadcast_id;

		/* Create a non-connectable non-scannable advertising set */
		err = bt_le_ext_adv_create(BT_LE_EXT_ADV_NCONN_NAME, NULL, &adv);
		if (err != 0) {
			printk("Unable to create extended advertising set: %d\n",
			       err);
			return;
		}

		/* Set periodic advertising parameters */
		err = bt_le_per_adv_set_param(adv, BT_LE_PER_ADV_DEFAULT);
		if (err) {
			printk("Failed to set periodic advertising parameters"
			" (err %d)\n", err);
			return;
		}

		printk("Creating broadcast source\n");
		err = setup_broadcast_source(&broadcast_source);
		if (err != 0) {
			printk("Unable to setup broadcast source: %d\n", err);
			return;
		}

		err = bt_audio_broadcast_source_get_id(broadcast_source,
						       &broadcast_id);
		if (err != 0) {
			printk("Unable to get broadcast ID: %d\n", err);
			return;
		}

		/* Setup extended advertising data */
		net_buf_simple_add_le16(&ad_buf, BT_UUID_BROADCAST_AUDIO_VAL);
		net_buf_simple_add_le24(&ad_buf, broadcast_id);
		ext_ad.type = BT_DATA_SVC_DATA16;
		ext_ad.data_len = ad_buf.len;
		ext_ad.data = ad_buf.data;
		err = bt_le_ext_adv_set_data(adv, &ext_ad, 1, NULL, 0);
		if (err != 0) {
			printk("Failed to set extended advertising data: %d\n",
			       err);
			return;
		}

		/* Setup periodic advertising data */
		err = bt_audio_broadcast_source_get_base(broadcast_source,
							 &base_buf);
		if (err != 0) {
			printk("Failed to get encoded BASE: %d\n", err);
			return;
		}

		per_ad.type = BT_DATA_SVC_DATA16;
		per_ad.data_len = base_buf.len;
		per_ad.data = base_buf.data;
		err = bt_le_per_adv_set_data(adv, &per_ad, 1);
		if (err != 0) {
			printk("Failed to set periodic advertising data: %d\n",
			       err);
			return;
		}

		/* Start extended advertising */
		err = bt_le_ext_adv_start(adv, BT_LE_EXT_ADV_START_DEFAULT);
		if (err) {
			printk("Failed to start extended advertising: %d\n",
			       err);
			return;
		}

		/* Enable Periodic Advertising */
		err = bt_le_per_adv_start(adv);
		if (err) {
			printk("Failed to enable periodic advertising: %d\n",
			       err);
			return;
		}

		printk("Starting broadcast source\n");
		stopping = false;
		err = bt_audio_broadcast_source_start(broadcast_source, adv);
		if (err != 0) {
			printk("Unable to start broadcast source: %d\n", err);
			return;
		}

		/* Wait for all to be started */
		for (size_t i = 0U; i < ARRAY_SIZE(streams); i++) {
			k_sem_take(&sem_started, K_FOREVER);
		}
		printk("Broadcast source started\n");

		/* Start send timer */
		k_work_schedule(&audio_send_work, K_MSEC(0));

		gpio_pin_set_dt(&led, 1);
	}
}
