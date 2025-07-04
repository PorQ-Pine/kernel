// SPDX-License-Identifier: GPL-2.0
/*
 * Parade TrueTouch(TM) Standard Product V5 Module.
 *
 * Copyright (C) 2015 Parade Technologies
 * Copyright (C) 2012-2015 Cypress Semiconductor
 * Copyright (C) 2018 Bootlin
 *
 * Authors: Mylène Josserand <mylene.josserand@bootlin.com>
 *                Alistair Francis <alistair@alistair23.me>
 */

#include <linux/crc-itu-t.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/firmware.h>
#include <linux/gpio/consumer.h>
#include <linux/input/mt.h>
#include <linux/input/touchscreen.h>
#include <linux/interrupt.h>
#include <linux/i2c.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include "linux/pm_runtime.h"
#include <linux/regmap.h>
#include "linux/spinlock.h"
#include <linux/unaligned.h>

#define CYTTSP5_NAME				"cyttsp5"
#define CY_I2C_DATA_SIZE			(2 * 256)
#define HID_VERSION				0x0100
#define CY_MAX_INPUT				512
#define CYTTSP5_PREALLOCATED_CMD_BUFFER		32
#define CY_BITS_PER_BTN				1
#define CY_NUM_BTN_EVENT_ID			GENMASK(CY_BITS_PER_BTN - 1, 0)

#define MAX_AREA				255
#define HID_OUTPUT_BL_SOP			0x1
#define HID_OUTPUT_BL_EOP			0x17
#define HID_OUTPUT_BL_LAUNCH_APP		0x3B
#define HID_OUTPUT_BL_LAUNCH_APP_SIZE		11
#define HID_OUTPUT_GET_SYSINFO			0x2
#define HID_OUTPUT_GET_SYSINFO_SIZE		5
#define HID_OUTPUT_VERIFY_CONFIG_BLOCK_CRC	0x20
#define HID_OUTPUT_GET_CONFIG_ROW_SIZE		0x21
#define HID_OUTPUT_READ_CONF_BLOCK		0x22
#define HID_OUTPUT_WRITE_CONF_BLOCK		0x23
#define HID_OUTPUT_WRITE_CONF_BLOCK_TIMEOUT_MS	400
#define HID_OUTPUT_SUSPEND_SCANNING		3
#define HID_OUTPUT_SUSPEND_SCANNING_TIMEOUT_MS	1000
#define HID_OUTPUT_RESUME_SCANNING		4
#define HID_OUTPUT_MAX_CMD_SIZE			12

#define HID_DESC_REG				0x1
#define HID_INPUT_REG				0x3
#define HID_OUTPUT_REG				0x4
#define HID_COMMAND_REG				0x5

#define REPORT_ID_TOUCH				0x1
#define REPORT_ID_BTN				0x3
#define REPORT_SIZE_5				5
#define REPORT_SIZE_8				8
#define REPORT_SIZE_16				16

/* Touch reports offsets */
/* Header offsets */
#define TOUCH_REPORT_DESC_HDR_CONTACTCOUNT	16
/* Record offsets */
#define TOUCH_REPORT_DESC_CONTACTID		8
#define TOUCH_REPORT_DESC_X			16
#define TOUCH_REPORT_DESC_Y			32
#define TOUCH_REPORT_DESC_P			48
#define TOUCH_REPORT_DESC_MAJ			56
#define TOUCH_REPORT_DESC_MIN			64

/* HID */
#define HID_TOUCH_REPORT_ID			0x1
#define HID_BTN_REPORT_ID			0x3
#define HID_APP_RESPONSE_REPORT_ID		0x1F
#define HID_APP_OUTPUT_REPORT_ID		0x2F
#define HID_BL_RESPONSE_REPORT_ID		0x30
#define HID_BL_OUTPUT_REPORT_ID			0x40
#define HID_RESPONSE_REPORT_ID			0xF0

#define HID_OUTPUT_RESPONSE_REPORT_OFFSET	2
#define HID_OUTPUT_RESPONSE_CMD_OFFSET		4
#define HID_OUTPUT_RESPONSE_CMD_MASK		GENMASK(6, 0)

#define HID_SYSINFO_CYDATA_OFFSET		5
#define HID_SYSINFO_SENSING_OFFSET		33
#define HID_SYSINFO_BTN_OFFSET			48
#define HID_SYSINFO_BTN_MASK			GENMASK(7, 0)
#define HID_SYSINFO_MAX_BTN			8

#define HID_CMD_SET_POWER			0x8

#define HID_POWER_ON				0x0
#define HID_POWER_SLEEP				0x1

#define CY_HID_OUTPUT_TIMEOUT_MS		200
#define CY_HID_OUTPUT_GET_SYSINFO_TIMEOUT_MS	3000
#define CY_HID_GET_HID_DESCRIPTOR_TIMEOUT_MS	4000
#define CY_HID_SET_POWER_TIMEOUT		500

/* maximum number of concurrent tracks */
#define TOUCH_REPORT_SIZE			10
#define TOUCH_INPUT_HEADER_SIZE			7
#define BTN_REPORT_SIZE				9
#define BTN_INPUT_HEADER_SIZE			5

#define MAX_CY_TCH_T_IDS			32

/* All usage pages for Touch Report */
#define TOUCH_REPORT_USAGE_PG_X			0x00010030
#define TOUCH_REPORT_USAGE_PG_Y			0x00010031
#define TOUCH_REPORT_USAGE_PG_P			0x000D0030
#define TOUCH_REPORT_USAGE_PG_CONTACTID		0x000D0051
#define TOUCH_REPORT_USAGE_PG_CONTACTCOUNT	0x000D0054
#define TOUCH_REPORT_USAGE_PG_MAJ		0xFF010062
#define TOUCH_REPORT_USAGE_PG_MIN		0xFF010063
#define TOUCH_COL_USAGE_PG			0x000D0022

#define SET_CMD_LOW(byte, bits) \
	((byte) = (((byte) & 0xF0) | ((bits) & 0x0F)))
#define SET_CMD_HIGH(byte, bits)\
	((byte) = (((byte) & 0x0F) | ((bits) & 0xF0)))
#define SET_CMD_OPCODE(byte, opcode) SET_CMD_LOW(byte, opcode)
#define SET_CMD_REPORT_TYPE(byte, type) SET_CMD_HIGH(byte, ((type) << 4))
#define SET_CMD_REPORT_ID(byte, id) SET_CMD_LOW(byte, id)

#define CY_TCH_PARM_EBID			0
#define CY_DATA_ROW_SIZE			128

#define CY_FIRMWARE_CONFIG			"cypress/cyttsp_config.bin"
MODULE_FIRMWARE(CY_FIRMWARE_CONFIG);

static const u8 cyttps5_security_key[] = {
	0xA5, 0x01, 0x02, 0x03, 0xFF, 0xFE, 0xFD, 0x5A
};

/* System Information interface definitions */
struct cyttsp5_sensing_conf_data_dev {
	u8 electrodes_x;
	u8 electrodes_y;
	__le16 len_x;
	__le16 len_y;
	__le16 res_x;
	__le16 res_y;
	__le16 max_z;
	u8 origin_x;
	u8 origin_y;
	u8 panel_id;
	u8 btn;
	u8 scan_mode;
	u8 max_num_of_tch_per_refresh_cycle;
} __packed;

struct cyttsp5_sensing_conf_data {
	u16 res_x;
	u16 res_y;
	u16 max_z;
	u16 len_x;
	u16 len_y;
	u8 origin_x;
	u8 origin_y;
	u8 max_tch;
};

enum cyttsp5_tch_abs {	/* for ordering within the extracted touch data array */
	CY_TCH_X,	/* X */
	CY_TCH_Y,	/* Y */
	CY_TCH_P,	/* P (Z) */
	CY_TCH_T,	/* TOUCH ID */
	CY_TCH_MAJ,	/* TOUCH_MAJOR */
	CY_TCH_MIN,	/* TOUCH_MINOR */
	CY_TCH_NUM_ABS
};

struct cyttsp5_tch_abs_params {
	size_t ofs;	/* abs byte offset */
	size_t size;	/* size in bits */
	size_t min;	/* min value */
	size_t max;	/* max value */
	size_t bofs;	/* bit offset */
};

struct cyttsp5_touch {
	int abs[CY_TCH_NUM_ABS];
};

struct cyttsp5_sysinfo {
	struct cyttsp5_sensing_conf_data sensing_conf_data;
	int num_btns;
	struct cyttsp5_tch_abs_params tch_hdr;
	struct cyttsp5_tch_abs_params tch_abs[CY_TCH_NUM_ABS];
	u32 key_code[HID_SYSINFO_MAX_BTN];
	u8 cydata[HID_SYSINFO_SENSING_OFFSET - HID_SYSINFO_CYDATA_OFFSET];
};

struct cyttsp5_hid_desc {
	__le16 hid_desc_len;
	u8 packet_id;
	u8 reserved_byte;
	__le16 bcd_version;
	__le16 report_desc_len;
	__le16 report_desc_register;
	__le16 input_register;
	__le16 max_input_len;
	__le16 output_register;
	__le16 max_output_len;
	__le16 command_register;
	__le16 data_register;
	__le16 vendor_id;
	__le16 product_id;
	__le16 version_id;
	u8 reserved[4];
} __packed;

struct cyttsp5 {
	struct device *dev;
	struct completion cmd_done;
	struct cyttsp5_sysinfo sysinfo;
	struct cyttsp5_hid_desc hid_desc;
	u8 cmd_buf[CYTTSP5_PREALLOCATED_CMD_BUFFER];
	u8 input_buf[CY_MAX_INPUT];
	u8 response_buf[CY_MAX_INPUT];
	struct gpio_desc *reset_gpio;
	struct input_dev *input;
	char phys[NAME_MAX];
	int num_prv_rec;
	struct regmap *regmap;
	struct touchscreen_properties prop;
	struct regulator_bulk_data supplies[2];
	spinlock_t exclusive_lock;
};

/*
 * For what is understood in the datasheet, the register does not
 * matter. For consistency, use the Input Register address
 * but it does mean anything to the device. The important data
 * to send is the I2C address
 */
static int cyttsp5_read(struct cyttsp5 *ts, u8 *buf, u32 max)
{
	int error;
	u32 size;
	u8 temp[2];

	/* Read the frame to retrieve the size */
	error = regmap_bulk_read(ts->regmap, HID_INPUT_REG, temp, sizeof(temp));
	if (error)
		return error;

	size = get_unaligned_le16(temp);
	if (!size || size == 2)
		return 0;

	if (size > max)
		return -EINVAL;

	/* Get the real value */
	return regmap_bulk_read(ts->regmap, HID_INPUT_REG, buf, size);
}

static int cyttsp5_write(struct cyttsp5 *ts, unsigned int reg, u8 *data,
			 size_t size)
{
	u8 cmd[HID_OUTPUT_MAX_CMD_SIZE];

	if (size + 1 > HID_OUTPUT_MAX_CMD_SIZE)
		return -E2BIG;

	/* High bytes of register address needed as first byte of cmd */
	cmd[0] = (reg >> 8) & 0xFF;

	/* Copy the rest of the data */
	if (data)
		memcpy(&cmd[1], data, size);

	/*
	 * The hardware wants to receive a frame with the address register
	 * contained in the first two bytes. As the regmap_write function
	 * add the register adresse in the frame, we use the low byte as
	 * first frame byte for the address register and the first
	 * data byte is the high register + left of the cmd to send
	 */
	return regmap_bulk_write(ts->regmap, reg & 0xFF, cmd, size + 1);
}

static void cyttsp5_get_touch_axis(int *axis, int size, int max, u8 *xy_data,
				   int bofs)
{
	int nbyte;

	for (nbyte = 0, *axis = 0; nbyte < size; nbyte++)
		*axis += ((xy_data[nbyte] >> bofs) << (nbyte * 8));

	*axis &= max - 1;
}

static void cyttsp5_get_touch_record(struct cyttsp5 *ts,
				     struct cyttsp5_touch *touch, u8 *xy_data)
{
	struct cyttsp5_sysinfo *si = &ts->sysinfo;
	enum cyttsp5_tch_abs abs;

	for (abs = CY_TCH_X; abs < CY_TCH_NUM_ABS; abs++)
		cyttsp5_get_touch_axis(&touch->abs[abs],
				       si->tch_abs[abs].size,
				       si->tch_abs[abs].max,
				       xy_data + si->tch_abs[abs].ofs,
				       si->tch_abs[abs].bofs);
}

static void cyttsp5_get_mt_touches(struct cyttsp5 *ts,
				   struct cyttsp5_touch *tch, int num_cur_tch)
{
	struct cyttsp5_sysinfo *si = &ts->sysinfo;
	int i, t = 0, offset = 0;
	DECLARE_BITMAP(ids, MAX_CY_TCH_T_IDS);
	u8 *tch_addr;
	int tmp;

	bitmap_zero(ids, MAX_CY_TCH_T_IDS);
	memset(tch->abs, 0, sizeof(tch->abs));

	switch (ts->input_buf[2]) {
	case HID_TOUCH_REPORT_ID:
		offset = TOUCH_INPUT_HEADER_SIZE;
		break;
	case HID_BTN_REPORT_ID:
		offset = BTN_INPUT_HEADER_SIZE;
		break;
	}

	for (i = 0; i < num_cur_tch; i++) {
		tch_addr = ts->input_buf + offset + (i * TOUCH_REPORT_SIZE);
		cyttsp5_get_touch_record(ts, tch, tch_addr);

		/* Convert MAJOR/MINOR from mm to resolution */
		tmp = tch->abs[CY_TCH_MAJ] * 100 * si->sensing_conf_data.res_x;
		tch->abs[CY_TCH_MAJ] = tmp / si->sensing_conf_data.len_x;
		tmp = tch->abs[CY_TCH_MIN] * 100 * si->sensing_conf_data.res_x;
		tch->abs[CY_TCH_MIN] = tmp / si->sensing_conf_data.len_x;

		t = tch->abs[CY_TCH_T];
		input_mt_slot(ts->input, t);
		input_mt_report_slot_state(ts->input, MT_TOOL_FINGER, true);
		__set_bit(t, ids);

		/* position and pressure fields */
		touchscreen_report_pos(ts->input, &ts->prop,
				       tch->abs[CY_TCH_X], tch->abs[CY_TCH_Y],
				       true);
		input_report_abs(ts->input, ABS_MT_PRESSURE,
				 tch->abs[CY_TCH_P]);

		/* Get the extended touch fields */
		input_report_abs(ts->input, ABS_MT_TOUCH_MAJOR,
				 tch->abs[CY_TCH_MAJ]);
		input_report_abs(ts->input, ABS_MT_TOUCH_MINOR,
				 tch->abs[CY_TCH_MIN]);
	}

	ts->num_prv_rec = num_cur_tch;
}

static int cyttsp5_mt_attention(struct device *dev)
{
	struct cyttsp5 *ts = dev_get_drvdata(dev);
	struct cyttsp5_sysinfo *si = &ts->sysinfo;
	int max_tch = si->sensing_conf_data.max_tch;
	struct cyttsp5_touch tch;
	int num_cur_tch;

	cyttsp5_get_touch_axis(&num_cur_tch, si->tch_hdr.size,
			       si->tch_hdr.max,
			       ts->input_buf + 3 + si->tch_hdr.ofs,
			       si->tch_hdr.bofs);

	if (num_cur_tch > max_tch) {
		dev_err(dev, "Num touch err detected (n=%d)\n", num_cur_tch);
		num_cur_tch = max_tch;
	}

	if (num_cur_tch == 0 && ts->num_prv_rec == 0)
		return 0;

	/* extract xy_data for all currently reported touches */
	if (num_cur_tch)
		cyttsp5_get_mt_touches(ts, &tch, num_cur_tch);

	input_mt_sync_frame(ts->input);
	input_sync(ts->input);

	return 0;
}

static int cyttsp5_setup_input_device(struct device *dev)
{
	struct cyttsp5 *ts = dev_get_drvdata(dev);
	struct cyttsp5_sysinfo *si = &ts->sysinfo;
	int max_x, max_y, max_p;
	int max_x_tmp, max_y_tmp;
	int error;

	max_x_tmp = si->sensing_conf_data.res_x;
	max_y_tmp = si->sensing_conf_data.res_y;
	max_x = max_x_tmp - 1;
	max_y = max_y_tmp - 1;
	max_p = si->sensing_conf_data.max_z;

	input_set_abs_params(ts->input, ABS_MT_POSITION_X, 0, max_x, 0, 0);
	input_set_abs_params(ts->input, ABS_MT_POSITION_Y, 0, max_y, 0, 0);
	input_set_abs_params(ts->input, ABS_MT_PRESSURE, 0, max_p, 0, 0);

	input_set_abs_params(ts->input, ABS_MT_TOUCH_MAJOR, 0, MAX_AREA, 0, 0);
	input_set_abs_params(ts->input, ABS_MT_TOUCH_MINOR, 0, MAX_AREA, 0, 0);

	error = input_mt_init_slots(ts->input, si->tch_abs[CY_TCH_T].max,
				    INPUT_MT_DROP_UNUSED | INPUT_MT_DIRECT);
	if (error)
		return error;

	error = input_register_device(ts->input);
	if (error) {
		dev_err(dev, "failed to register input device: %d\n", error);
		return error;
	}

	return error;
}

static int cyttsp5_parse_dt_key_code(struct device *dev)
{
	struct cyttsp5 *ts = dev_get_drvdata(dev);
	struct cyttsp5_sysinfo *si = &ts->sysinfo;

	if (!si->num_btns)
		return 0;

	/* Initialize the button to RESERVED */
	memset32(si->key_code, KEY_RESERVED,  si->num_btns);

	return device_property_read_u32_array(dev, "linux,keycodes",
					      si->key_code, si->num_btns);
}

static int cyttsp5_btn_attention(struct device *dev)
{
	struct cyttsp5 *ts = dev_get_drvdata(dev);
	struct cyttsp5_sysinfo *si = &ts->sysinfo;
	int cur_btn, offset = 0;
	int cur_btn_state;

	switch (ts->input_buf[2]) {
	case HID_TOUCH_REPORT_ID:
		offset = TOUCH_INPUT_HEADER_SIZE;
		break;
	case HID_BTN_REPORT_ID:
		offset = BTN_INPUT_HEADER_SIZE;
		break;
	}

	if (ts->input_buf[2] != HID_BTN_REPORT_ID)
		return 0;

	/* extract button press/release touch information */
	for (cur_btn = 0; cur_btn < si->num_btns; cur_btn++) {
		/* Get current button state */
		cur_btn_state = (ts->input_buf[offset] >> (cur_btn * CY_BITS_PER_BTN))
				& CY_NUM_BTN_EVENT_ID;

		input_report_key(ts->input, si->key_code[cur_btn],
				 cur_btn_state);
		input_sync(ts->input);
	}

	return 0;
}

static int cyttsp5_validate_cmd_response(struct cyttsp5 *ts, u8 code)
{
	u16 size, crc;
	u8 status, report_id;
	int command_code;

	size = get_unaligned_le16(&ts->response_buf[0]);
	if (!size)
		return 0;

	report_id = ts->response_buf[HID_OUTPUT_RESPONSE_REPORT_OFFSET];

	switch (report_id) {
	case HID_BL_RESPONSE_REPORT_ID:
		if (ts->response_buf[4] != HID_OUTPUT_BL_SOP) {
			dev_err(ts->dev, "HID output response, wrong SOP\n");
			return -EPROTO;
		}

		if (ts->response_buf[size - 1] != HID_OUTPUT_BL_EOP) {
			dev_err(ts->dev, "HID output response, wrong EOP\n");
			return -EPROTO;
		}

		crc = crc_itu_t(0xFFFF, &ts->response_buf[4], size - 7);
		if (get_unaligned_le16(&ts->response_buf[size - 3]) != crc) {
			dev_err(ts->dev,
				"HID output response, wrong CRC 0x%X\n",
				crc);
			return -EPROTO;
		}

		status = ts->response_buf[5];
		if (status) {
			dev_err(ts->dev, "HID output response, ERROR:%d\n",
				status);
			return -EPROTO;
		}
		break;

	case HID_APP_RESPONSE_REPORT_ID:
		command_code = ts->response_buf[HID_OUTPUT_RESPONSE_CMD_OFFSET]
			& HID_OUTPUT_RESPONSE_CMD_MASK;
		if (command_code != code) {
			dev_err(ts->dev,
				"HID output response, wrong command_code:%X\n",
				command_code);
			return -EPROTO;
		}
		break;
	}

	return 0;
}

static int cyttsp5_hid_output_app_write_and_wait(struct cyttsp5 *ts,
						 u8 cmd_code, u8* data,
						 u16 data_len,
						 u16 timeout_ms)
{
	int rc;
	u8 small_cmd[HID_OUTPUT_MAX_CMD_SIZE];
	u8 *cmd;
	u16 total_len = 6 + data_len;

	if (total_len > HID_OUTPUT_MAX_CMD_SIZE) {
		cmd = kzalloc(total_len, GFP_KERNEL);
		if (!cmd)
			return -ENOMEM;
	} else
		cmd = small_cmd;

	cmd[0] = (HID_OUTPUT_REG >> 8) & 0xFF;
	put_unaligned_le16(5 + data_len, cmd + 1);
	cmd[3] = HID_APP_OUTPUT_REPORT_ID;
	cmd[4] = 0x0; /* Reserved */
	cmd[5] = cmd_code;

	if (data_len)
		memcpy(cmd + 6, data, data_len);

	rc = regmap_bulk_write(ts->regmap, HID_OUTPUT_REG & 0xFF, cmd, total_len);
	if (rc) {
		dev_err(ts->dev, "Failed to write command %d\n", rc);
		goto exit;
	}

	if (!timeout_ms)
		timeout_ms = CY_HID_OUTPUT_TIMEOUT_MS;

	rc = wait_for_completion_interruptible_timeout(&ts->cmd_done,
			msecs_to_jiffies(timeout_ms));
	if (rc <= 0) {
		dev_err(ts->dev, "HID output cmd execution timed out\n");
		rc = -ETIMEDOUT;
		goto exit;
	}

	rc = cyttsp5_validate_cmd_response(ts, cmd_code);
	if (rc) {
		dev_err(ts->dev, "Validation of the response failed\n");
		goto exit;
	}

exit:
	if (total_len > HID_OUTPUT_MAX_CMD_SIZE) {
		kfree(cmd);
	}
	return rc;
}

static void cyttsp5_si_get_btn_data(struct cyttsp5 *ts)
{
	struct cyttsp5_sysinfo *si = &ts->sysinfo;
	unsigned int btns = ts->response_buf[HID_SYSINFO_BTN_OFFSET] &
				HID_SYSINFO_BTN_MASK;

	si->num_btns = hweight8(btns);
}

static int cyttsp5_get_sysinfo_regs(struct cyttsp5 *ts)
{
	struct cyttsp5_sensing_conf_data *scd = &ts->sysinfo.sensing_conf_data;
	struct cyttsp5_sensing_conf_data_dev *scd_dev =
		(struct cyttsp5_sensing_conf_data_dev *)
		&ts->response_buf[HID_SYSINFO_SENSING_OFFSET];
	u32 tmp;

	cyttsp5_si_get_btn_data(ts);
	memcpy(ts->sysinfo.cydata, ts->response_buf + HID_SYSINFO_CYDATA_OFFSET, sizeof(ts->sysinfo.cydata));

	scd->max_tch = scd_dev->max_num_of_tch_per_refresh_cycle;

	if (scd->max_tch == 0) {
		dev_dbg(ts->dev, "Max touch points cannot be zero\n");
		scd->max_tch = 2;
	}

	if(device_property_read_u32(ts->dev, "touchscreen-size-x", &tmp))
		scd->res_x = get_unaligned_le16(&scd_dev->res_x);
	else
		scd->res_x = tmp;

	if (scd->res_x == 0) {
		dev_err(ts->dev, "ABS_X cannot be zero\n");
		return -ENODATA;
	}

	if(device_property_read_u32(ts->dev, "touchscreen-size-y", &tmp))
		scd->res_y = get_unaligned_le16(&scd_dev->res_y);
	else
		scd->res_y = tmp;

	if (scd->res_y == 0) {
		dev_err(ts->dev, "ABS_Y cannot be zero\n");
		return -ENODATA;
	}

	if(device_property_read_u32(ts->dev, "touchscreen-max-pressure", &tmp))
		scd->max_z = get_unaligned_le16(&scd_dev->max_z);
	else
		scd->max_z = tmp;

	if (scd->max_z == 0) {
		dev_err(ts->dev, "ABS_PRESSURE cannot be zero\n");
		return -ENODATA;
	}

	if(device_property_read_u32(ts->dev, "touchscreen-x-mm", &tmp))
		scd->len_x = get_unaligned_le16(&scd_dev->len_x);
	else
		scd->len_x = tmp;

	if (scd->len_x == 0) {
		dev_dbg(ts->dev, "Touchscreen size x cannot be zero\n");
		scd->len_x = scd->res_x + 1;
	}

	if(device_property_read_u32(ts->dev, "touchscreen-y-mm", &tmp))
		scd->len_y = get_unaligned_le16(&scd_dev->len_y);
	else
		scd->len_y = tmp;

	if (scd->len_y == 0) {
		dev_dbg(ts->dev, "Touchscreen size y cannot be zero\n");
		scd->len_y = scd->res_y + 1;
	}

	return 0;
}

static int cyttsp5_hid_output_get_sysinfo(struct cyttsp5 *ts)
{
	int rc;
	u8 cmd[HID_OUTPUT_GET_SYSINFO_SIZE];

	/* HI bytes of Output register address */
	put_unaligned_le16(HID_OUTPUT_GET_SYSINFO_SIZE, cmd);
	cmd[2] = HID_APP_OUTPUT_REPORT_ID;
	cmd[3] = 0x0; /* Reserved */
	cmd[4] = HID_OUTPUT_GET_SYSINFO;

	rc = cyttsp5_write(ts, HID_OUTPUT_REG, cmd,
			   HID_OUTPUT_GET_SYSINFO_SIZE);
	if (rc) {
		dev_err(ts->dev, "Failed to write command %d", rc);
		return rc;
	}

	rc = wait_for_completion_interruptible_timeout(&ts->cmd_done,
						msecs_to_jiffies(CY_HID_OUTPUT_GET_SYSINFO_TIMEOUT_MS));
	if (rc <= 0) {
		dev_err(ts->dev, "HID output cmd execution timed out\n");
		rc = -ETIMEDOUT;
		return rc;
	}

	rc = cyttsp5_validate_cmd_response(ts, HID_OUTPUT_GET_SYSINFO);
	if (rc) {
		dev_err(ts->dev, "Validation of the response failed\n");
		return rc;
	}

	return cyttsp5_get_sysinfo_regs(ts);
}

static int cyttsp5_power_control(struct cyttsp5 *ts, bool on)
{
	u8 state = on ? HID_POWER_ON : HID_POWER_SLEEP;
	u8 cmd[2] = { 0 };
	int rc;

	SET_CMD_REPORT_TYPE(cmd[0], 0);
	SET_CMD_REPORT_ID(cmd[0], state);
	SET_CMD_OPCODE(cmd[1], HID_CMD_SET_POWER);

	rc = cyttsp5_write(ts, HID_COMMAND_REG, cmd, sizeof(cmd));
	if (rc) {
		dev_err(ts->dev, "Failed to write power command %d", rc);
		return rc;
	}

	rc = wait_for_completion_interruptible_timeout(&ts->cmd_done,
				msecs_to_jiffies(CY_HID_SET_POWER_TIMEOUT));
	if (rc <= 0) {
		dev_err(ts->dev, "HID power cmd execution timed out\n");
		return -ETIMEDOUT;
	}

	if (ts->response_buf[2] != HID_RESPONSE_REPORT_ID ||
	    (ts->response_buf[3] & 0x03) != state ||
	    (ts->response_buf[4] & 0x0f) != HID_CMD_SET_POWER) {
		dev_err(ts->dev, "Validation of the %s response failed\n",
			on ? "wakeup" : "sleep");
		return -EINVAL;
	}

	return 0;
}

static int cyttsp5_hid_output_bl_launch_app(struct cyttsp5 *ts)
{
	int rc;
	u8 cmd[HID_OUTPUT_BL_LAUNCH_APP_SIZE];
	u16 crc;

	put_unaligned_le16(HID_OUTPUT_BL_LAUNCH_APP_SIZE, cmd);
	cmd[2] = HID_BL_OUTPUT_REPORT_ID;
	cmd[3] = 0x0; /* Reserved */
	cmd[4] = HID_OUTPUT_BL_SOP;
	cmd[5] = HID_OUTPUT_BL_LAUNCH_APP;
	put_unaligned_le16(0x00, &cmd[6]);
	crc = crc_itu_t(0xFFFF, &cmd[4], 4);
	put_unaligned_le16(crc, &cmd[8]);
	cmd[10] = HID_OUTPUT_BL_EOP;

	rc = cyttsp5_write(ts, HID_OUTPUT_REG, cmd,
			   HID_OUTPUT_BL_LAUNCH_APP_SIZE);
	if (rc) {
		dev_err(ts->dev, "Failed to write command %d", rc);
		return rc;
	}

	rc = wait_for_completion_interruptible_timeout(&ts->cmd_done,
				msecs_to_jiffies(CY_HID_OUTPUT_TIMEOUT_MS));
	if (rc <= 0) {
		dev_err(ts->dev, "HID output cmd execution timed out\n");
		rc = -ETIMEDOUT;
		return rc;
	}

	rc = cyttsp5_validate_cmd_response(ts, HID_OUTPUT_BL_LAUNCH_APP);
	if (rc) {
		dev_err(ts->dev, "Validation of the response failed\n");
		return rc;
	}

	return 0;
}

static int cyttsp5_get_hid_descriptor(struct cyttsp5 *ts,
				      struct cyttsp5_hid_desc *desc)
{
	struct device *dev = ts->dev;
	int rc;

	rc = cyttsp5_write(ts, HID_DESC_REG, NULL, 0);
	if (rc) {
		dev_err(dev, "Failed to get HID descriptor, rc=%d\n", rc);
		return rc;
	}

	rc = wait_for_completion_interruptible_timeout(&ts->cmd_done,
			msecs_to_jiffies(CY_HID_GET_HID_DESCRIPTOR_TIMEOUT_MS));
	if (rc <= 0) {
		dev_err(ts->dev, "HID get descriptor timed out\n");
		rc = -ETIMEDOUT;
		return rc;
	}

	memcpy(desc, ts->response_buf, sizeof(*desc));

	/* Check HID descriptor length and version */
	if (le16_to_cpu(desc->hid_desc_len) != sizeof(*desc) ||
	    le16_to_cpu(desc->bcd_version) != HID_VERSION) {
		dev_err(dev, "Unsupported HID version\n");
		return -ENODEV;
	}

	return 0;
}

static int fill_tch_abs(struct cyttsp5_tch_abs_params *tch_abs, int report_size,
			int offset)
{
	tch_abs->ofs = offset / 8;
	tch_abs->size = report_size / 8;
	if (report_size % 8)
		tch_abs->size += 1;
	tch_abs->min = 0;
	tch_abs->max = 1 << report_size;
	tch_abs->bofs = offset - (tch_abs->ofs << 3);

	return 0;
}

static int cyttsp5_hid_output_suspend_scanning(struct cyttsp5 *ts)
{
	int rc;
	rc = cyttsp5_hid_output_app_write_and_wait(
			ts, HID_OUTPUT_SUSPEND_SCANNING, NULL, 0,
			HID_OUTPUT_SUSPEND_SCANNING_TIMEOUT_MS);
	if (rc) {
		dev_err(ts->dev, "Failed to suspend scanning %d\n", rc);
		return rc;
	}

	return 0;
}

static int cyttsp5_hid_output_resume_scanning(struct cyttsp5 *ts)
{
	int rc;
	rc = cyttsp5_hid_output_app_write_and_wait(
			ts, HID_OUTPUT_RESUME_SCANNING, NULL, 0, 0);
	if (rc) {
		dev_err(ts->dev, "Failed to resume scanning %d\n", rc);
		return rc;
	}

	return 0;
}

static int
cyttsp5_hid_output_verify_config_block_crc(struct cyttsp5 *ts, u8 ebid, u8 *status,
					   u16 *calculated_crc, u16 *stored_crc)
{
	int rc;
	u8 write_buf[] = { ebid };
	rc = cyttsp5_hid_output_app_write_and_wait(
		ts, HID_OUTPUT_VERIFY_CONFIG_BLOCK_CRC, write_buf,
		ARRAY_SIZE(write_buf), CY_HID_OUTPUT_TIMEOUT_MS);
	if (rc) {
		dev_err(ts->dev, "%s: Failed to verify config block crc", __func__);
		return rc;
	}

	*status = ts->response_buf[5];
	*calculated_crc = get_unaligned_le16(ts->response_buf + 6);
	*stored_crc = get_unaligned_le16(ts->response_buf + 8);

	return 0;
}

static int cyttsp5_hid_output_get_config_row_size(struct cyttsp5 *ts,
						  u16 *row_size)
{
	int rc;
	rc = cyttsp5_hid_output_app_write_and_wait(
			ts, HID_OUTPUT_GET_CONFIG_ROW_SIZE, NULL, 0, 0);
	if (rc) {
		dev_err(ts->dev, "Failed to get config row size %d\n", rc);
		return rc;
	}

	*row_size = get_unaligned_le16(ts->response_buf + 5);
	return 0;
}

static int cyttsp5_hid_output_read_conf_block(struct cyttsp5 *ts,
					      u16 row_number, u16 length,
					      u8 ebid, u8 *read_buf, u16 *crc)
{
	int rc;
	int read_ebid;
	int read_length;
	int status;
	u8 write_buf[5];

	put_unaligned_le16(row_number, write_buf);
	put_unaligned_le16(length, write_buf + 2);
	write_buf[4] = ebid;

	rc = cyttsp5_hid_output_app_write_and_wait(ts,
			HID_OUTPUT_READ_CONF_BLOCK, write_buf,
			ARRAY_SIZE(write_buf), 0);
	if (rc) {
		dev_err(ts->dev, "Failed to read config block row=%d\n",
			row_number);
		return rc;
	}

	status = ts->response_buf[5];
	if (status)
		return -EINVAL;

	read_ebid = ts->response_buf[6];
	if ((read_ebid != ebid) || (ts->response_buf[9] != 0)) {
		return -EPROTO;
	}

	read_length = get_unaligned_le16(ts->response_buf + 7);
	if (length > read_length)
		length = read_length;

	memcpy(read_buf, ts->response_buf + 10, min(length, read_length));
	*crc = get_unaligned_le16(ts->response_buf + read_length + 10);

	return 0;
}

static int cyttsp5_hid_output_write_conf_block(struct cyttsp5 *ts,
					      u16 row_number, u16 length,
					      u8 ebid, const u8 *data)
{
	int ret;
	u16 actual_write_len;
	u16 crc;
	u8 status, read_ebid;

	int key_size = ARRAY_SIZE(cyttps5_security_key);
	int write_buf_len = 2 + 2 + 1 + length + key_size + 2;
	u8 *write_buf = kzalloc(write_buf_len, GFP_KERNEL);
	if (!write_buf)
		return -ENOMEM;


	put_unaligned_le16(row_number, write_buf);
	put_unaligned_le16(length, write_buf + 2);
	write_buf[4] = ebid;
	memcpy(write_buf + 5, data, length);
	memcpy(write_buf + 5 + length, cyttps5_security_key,
	       ARRAY_SIZE(cyttps5_security_key));
	crc = crc_itu_t(0xFFFF, data, length);
	put_unaligned_le16(crc, write_buf + 5 + length + key_size);

	ret = cyttsp5_hid_output_app_write_and_wait(ts,
						    HID_OUTPUT_WRITE_CONF_BLOCK,
						    write_buf, write_buf_len,
						    HID_OUTPUT_WRITE_CONF_BLOCK_TIMEOUT_MS);
	if (ret) {
		dev_err(ts->dev, "%s: cmd failed ret=%d", __func__, ret);
		return ret;
	}

	status = ts->response_buf[5];
	if (status) {
		dev_err(ts->dev, "%s: response status=%d", __func__, status);
		return -EINVAL;
	}

	read_ebid = ts->response_buf[6];
	if (read_ebid != ebid) {
		dev_err(ts->dev, "%s: non-matching EBIDs", __func__);
		return -EPROTO;
	}

	actual_write_len = get_unaligned_le16(ts->response_buf + 7);
	if (actual_write_len != length) {
		dev_err(ts->dev,
			"%s: wrong write size length=%d actual_write_len=%d",
			__func__, length, actual_write_len);
		return -EINVAL;
	}
	return 0;
}

static ssize_t cyttsp5_sysfs_dump_cydata(struct device *dev,
				     struct device_attribute *attr, char *buf)
{
	struct cyttsp5 *ts = dev_get_drvdata(dev);
	ssize_t len = sizeof(ts->sysinfo.cydata);
	memcpy(buf, ts->sysinfo.cydata, len);
	return len;
}

static ssize_t cyttsp5_sysfs_dump_config(struct device *dev,
				     struct device_attribute *attr, char *buf)
{
	int rc;
	u8 read_buf[CY_DATA_ROW_SIZE];
	size_t cfg_size, bytes_written = 0, read_len = 0;
	u16 crc;

	struct cyttsp5 *ts = dev_get_drvdata(dev);

	rc = cyttsp5_hid_output_suspend_scanning(ts);
	if (rc)
		return rc;

	spin_lock(&ts->exclusive_lock);

	rc = cyttsp5_hid_output_read_conf_block(ts, 0, CY_DATA_ROW_SIZE,
			CY_TCH_PARM_EBID, read_buf, &crc);
	if (rc) {
		dev_err(dev, "Failed to read first config block %d", rc);
		goto resume_scanning;
	}

	cfg_size = get_unaligned_le16(read_buf) + 2; // CRC
	read_len = min(CY_DATA_ROW_SIZE, cfg_size);
	while (bytes_written < cfg_size) {
		memcpy(buf + bytes_written, read_buf, read_len);
		bytes_written += read_len;
		read_len = min(CY_DATA_ROW_SIZE, cfg_size - bytes_written);
		if (read_len > 0) {
			rc = cyttsp5_hid_output_read_conf_block(ts,
					bytes_written / CY_DATA_ROW_SIZE,
					read_len, CY_TCH_PARM_EBID, read_buf,
					&crc);
		}
		if (rc) {
			dev_err(dev, "Failed to read config block %ld, config"
					" size=%ld rc=%d",
					bytes_written / CY_DATA_ROW_SIZE,
					cfg_size, rc);
			goto resume_scanning;
		}
	}
resume_scanning:
	spin_unlock(&ts->exclusive_lock);
	cyttsp5_hid_output_resume_scanning(ts);

	return bytes_written;
}

static int cyttsp5_verify_ttconfig(struct cyttsp5 *ts,
				   const struct firmware *config,
				   size_t *config_offset, size_t *config_length)
{
	struct cyttsp5_sysinfo *si = &ts->sysinfo;
	int header_size;
	u16 fw_ver_config;
	u32 fw_revctrl_config;
	u16 fw_ver_si;
	u32 fw_revctrl_si;
	u16 config_size;

	if (!si) {
		dev_err(ts->dev,
			"%s: No firmware information found, device firmware may be corrupted",
			__func__);
		return -ENODEV;
	}

	if (config->size < 11) {
		dev_err(ts->dev, "%s: config is too short", __func__);
		return -EINVAL;
	}

	/*
	 * We need 11 bytes for FW version control info and at
	 * least 6 bytes in config (Length + Max Length + CRC)
	 */
	header_size = config->data[0] + 1;
	if (header_size < 11 || header_size >= config->size - 6) {
		dev_err(ts->dev, "%s: Invalid header size %d", __func__,
			header_size);
		return -EINVAL;
	}

	fw_ver_config = get_unaligned_be16(config->data + 1);
	/* 4 middle bytes are not used */
	fw_revctrl_config = get_unaligned_be32(config->data + 7);

	fw_ver_si = get_unaligned_be16(si->cydata + 4);
	fw_revctrl_si = get_unaligned_le32(si->cydata + 6);

	/* FW versions should match */
	if (fw_ver_config != fw_ver_si) {
		dev_err(ts->dev,
			"%s: Firmware version mismatch config=%u si=%u",
			__func__, fw_ver_config, fw_ver_si);
		return -EINVAL;
	}

	/* Firmware revision should match as well */
	if (fw_revctrl_config != fw_revctrl_si) {
		dev_err(ts->dev,
			"%s: Firmware revision mismatch config=%u si=%u",
			__func__, fw_revctrl_config, fw_revctrl_si);
		return -EINVAL;
	}

	config_size = get_unaligned_le16(config->data + header_size);
	if (config_size != config->size - header_size - 2) {
		dev_err(ts->dev, "%s: Invalid config size", __func__);
		return -EINVAL;
	}
	*config_offset = header_size;
	*config_length = config_size;

	return 0;
}

static int cyttsp5_update_config(struct device *dev)
{
	struct cyttsp5 *ts = dev_get_drvdata(dev);
	const struct firmware *config;
	int i, ret;
	int row_count, remainder;
	u8 ebid = CY_TCH_PARM_EBID;
	u8 crc_status;
	u16 calculated_crc, stored_crc;
	size_t config_length;
	size_t offset;

	// 1. Load firmware
	ret = request_firmware(&config, CY_FIRMWARE_CONFIG, dev);
	if (ret) {
		dev_err(dev, "Unable to load config firmware %s", CY_FIRMWARE_CONFIG);
		return -EINVAL;
	}

	// 2. Validate firmware
	ret = cyttsp5_verify_ttconfig(ts, config, &offset, &config_length);
	if (ret) {
		dev_err(dev, "Configuration validation failed");
		return ret;
	}

	// 3. Runtime: get sync
	pm_runtime_get_sync(dev);

	spin_lock(&ts->exclusive_lock);

	// 4. Suspend scanning
	ret = cyttsp5_hid_output_suspend_scanning(ts);
	if (ret) {
		dev_err(dev, "Failed to suspend scanning ret=%d", ret);
		return ret;
	}

	row_count = config_length / CY_DATA_ROW_SIZE;
	remainder = config_length % CY_DATA_ROW_SIZE;

	for (i = 0; i < row_count; ++i) {
		ret = cyttsp5_hid_output_write_conf_block(
			ts, i, CY_DATA_ROW_SIZE, ebid,
			config->data + offset);
		offset += CY_DATA_ROW_SIZE;
		if (ret) {
			dev_err(dev, "Failed to write row i=%d ret=%d", i, ret);
			break;
		}
	}
	if (!ret && remainder) {
		ret = cyttsp5_hid_output_write_conf_block(
			ts, row_count, remainder, ebid,
			config->data + offset);
		if (ret)
			dev_err(dev, "Failed to write remainder row i=%d ret=%d", i, ret);
	}

	if (!ret)
		dev_dbg(dev, "Wrote all %zu config bytes", config_length);

	ret = cyttsp5_hid_output_verify_config_block_crc(ts, ebid, &crc_status, &calculated_crc, &stored_crc);
	if (ret || crc_status)
		dev_err(dev,
			"%s: CRC failed ret=%d crc_status=%d calculated_crc=%d stored_crc=%d",
			__func__, ret, crc_status, calculated_crc, stored_crc);
	else
		dev_dbg(dev, "%s: CRC pass", __func__);

	ret = cyttsp5_hid_output_resume_scanning(ts);
	if (ret) {
		dev_err(dev, "Failed to resume scanning");
		return ret;
	}

	spin_unlock(&ts->exclusive_lock);

	// TODO: consider request_restart

	pm_runtime_put_sync(dev);

	return 0;
}

static ssize_t update_config_store(struct device *dev,
				   struct device_attribute *attr,
				   const char *buf, size_t len)
{
	int ret = cyttsp5_update_config(dev);
	if (ret)
		dev_err(dev, "Failed to update config ret=%d", ret);
	else
		dev_info(dev, "Updated config");

	return len;
}

static DEVICE_ATTR(dump_config, S_IRUGO, cyttsp5_sysfs_dump_config, NULL);
static DEVICE_ATTR(dump_cydata, S_IRUGO, cyttsp5_sysfs_dump_cydata, NULL);
DEVICE_ATTR_WO(update_config);

static irqreturn_t cyttsp5_handle_irq(int irq, void *handle)
{
	struct cyttsp5 *ts = handle;
	int report_id;
	int size;
	int error;

	error = cyttsp5_read(ts, ts->input_buf, CY_MAX_INPUT);
	if (error)
		return IRQ_HANDLED;

	size = get_unaligned_le16(&ts->input_buf[0]);
	if (size == 0) {
		/* reset */
		report_id = 0;
		size = 2;
	} else {
		report_id = ts->input_buf[2];
	}

	switch (report_id) {
	case HID_TOUCH_REPORT_ID:
		cyttsp5_mt_attention(ts->dev);
		break;
	case HID_BTN_REPORT_ID:
		cyttsp5_btn_attention(ts->dev);
		break;
	case HID_RESPONSE_REPORT_ID:
		memcpy(ts->response_buf, ts->input_buf, size);
		complete(&ts->cmd_done);
		break;
	default:
		/* It is not an input but a command response */
		memcpy(ts->response_buf, ts->input_buf, size);
		complete(&ts->cmd_done);
	}

	return IRQ_HANDLED;
}

static int cyttsp5_deassert_int(struct cyttsp5 *ts)
{
	u16 size;
	u8 buf[2];
	int error;

	error = regmap_bulk_read(ts->regmap, HID_INPUT_REG, buf, sizeof(buf));
	if (error < 0)
		return error;

	size = get_unaligned_le16(&buf[0]);
	if (size == 2 || size == 0)
		return 0;

	return -EINVAL;
}

static int cyttsp5_fill_all_touch(struct cyttsp5 *ts)
{
	struct cyttsp5_sysinfo *si = &ts->sysinfo;

	fill_tch_abs(&si->tch_abs[CY_TCH_X], REPORT_SIZE_16,
		     TOUCH_REPORT_DESC_X);
	fill_tch_abs(&si->tch_abs[CY_TCH_Y], REPORT_SIZE_16,
		     TOUCH_REPORT_DESC_Y);
	fill_tch_abs(&si->tch_abs[CY_TCH_P], REPORT_SIZE_8,
		     TOUCH_REPORT_DESC_P);
	fill_tch_abs(&si->tch_abs[CY_TCH_T], REPORT_SIZE_5,
		     TOUCH_REPORT_DESC_CONTACTID);
	fill_tch_abs(&si->tch_hdr, REPORT_SIZE_5,
		     TOUCH_REPORT_DESC_HDR_CONTACTCOUNT);
	fill_tch_abs(&si->tch_abs[CY_TCH_MAJ], REPORT_SIZE_8,
		     TOUCH_REPORT_DESC_MAJ);
	fill_tch_abs(&si->tch_abs[CY_TCH_MIN], REPORT_SIZE_8,
		     TOUCH_REPORT_DESC_MIN);

	return 0;
}

static int cyttsp5_startup(struct cyttsp5 *ts)
{
	int error;

	error = cyttsp5_deassert_int(ts);
	if (error) {
		dev_err(ts->dev, "Error on deassert int r=%d\n", error);
		return -ENODEV;
	}

	/*
	 * Launch the application as the device starts in bootloader mode
	 * because of a power-on-reset
	 */
	error = cyttsp5_hid_output_bl_launch_app(ts);
	if (error < 0) {
		dev_err(ts->dev, "Error on launch app r=%d\n", error);
		return error;
	}

	error = cyttsp5_get_hid_descriptor(ts, &ts->hid_desc);
	if (error < 0) {
		dev_err(ts->dev, "Error on getting HID descriptor r=%d\n", error);
		return error;
	}

	error = cyttsp5_fill_all_touch(ts);
	if (error < 0) {
		dev_err(ts->dev, "Error on report descriptor r=%d\n", error);
		return error;
	}

	error = cyttsp5_hid_output_get_sysinfo(ts);
	if (error) {
		dev_err(ts->dev, "Error on getting sysinfo r=%d\n", error);
		return error;
	}

	return error;
}

static struct attribute *cyttsp5_attrs[] = {
	&dev_attr_dump_config.attr,
	&dev_attr_dump_cydata.attr,
	&dev_attr_update_config.attr,
	NULL
};

static const struct attribute_group cyttsp5_attr_group = {
	.attrs	= cyttsp5_attrs,
};

static void cyttsp5_cleanup(void *data)
{
	struct cyttsp5 *ts = data;

	regulator_bulk_disable(ARRAY_SIZE(ts->supplies), ts->supplies);
}

static int cyttsp5_probe(struct device *dev, struct regmap *regmap, int irq,
			 const char *name)
{
	struct cyttsp5 *ts;
	struct cyttsp5_sysinfo *si;
	int error, i;

	ts = devm_kzalloc(dev, sizeof(*ts), GFP_KERNEL);
	if (!ts)
		return -ENOMEM;

	/* Initialize device info */
	ts->regmap = regmap;
	ts->dev = dev;
	si = &ts->sysinfo;
	dev_set_drvdata(dev, ts);

	init_completion(&ts->cmd_done);

	/* Power up the device */
	ts->supplies[0].supply = "vdd";
	ts->supplies[1].supply = "vddio";
	error = devm_regulator_bulk_get(dev, ARRAY_SIZE(ts->supplies),
					ts->supplies);
	if (error) {
		dev_err(ts->dev, "Failed to get regulators, error %d\n", error);
		return error;
	}

	error = devm_add_action_or_reset(dev, cyttsp5_cleanup, ts);
	if (error)
		return error;

	error = regulator_bulk_enable(ARRAY_SIZE(ts->supplies), ts->supplies);
	if (error) {
		dev_err(ts->dev, "Failed to enable regulators, error %d\n", error);
		return error;
	}

	error = sysfs_create_group(&dev->kobj, &cyttsp5_attr_group);
	if (error) {
		dev_err(dev,
			"Failed to create sysfs attributes, err: %d\n", error);
		return error;
	}

	ts->input = devm_input_allocate_device(dev);
	if (!ts->input) {
		dev_err(dev, "Error, failed to allocate input device\n");
		return -ENODEV;
	}

	spin_lock_init(&ts->exclusive_lock);

	ts->input->name = "cyttsp5";
	scnprintf(ts->phys, sizeof(ts->phys), "%s/input0", dev_name(dev));
	ts->input->phys = ts->phys;
	input_set_drvdata(ts->input, ts);

	/* Reset the gpio to be in a reset state */
	ts->reset_gpio = devm_gpiod_get_optional(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(ts->reset_gpio)) {
		error = PTR_ERR(ts->reset_gpio);
		dev_err(dev, "Failed to request reset gpio, error %d\n", error);
		return error;
	}
	gpiod_set_value_cansleep(ts->reset_gpio, 0);

	/* Need a delay to have device up */
	msleep(20);

	error = devm_request_threaded_irq(dev, irq, NULL, cyttsp5_handle_irq,
					  IRQF_ONESHOT, name, ts);
	if (error) {
		dev_err(dev, "unable to request IRQ\n");
		return error;
	}

	error = cyttsp5_startup(ts);
	if (error) {
		dev_err(ts->dev, "Fail initial startup r=%d\n", error);
		return error;
	}

	error = cyttsp5_parse_dt_key_code(dev);
	if (error < 0) {
		dev_err(ts->dev, "Error while parsing dts %d\n", error);
		return error;
	}

	touchscreen_parse_properties(ts->input, true, &ts->prop);

	__set_bit(EV_KEY, ts->input->evbit);
	for (i = 0; i < si->num_btns; i++)
		__set_bit(si->key_code[i], ts->input->keybit);

	return cyttsp5_setup_input_device(dev);
}

static int cyttsp5_i2c_probe(struct i2c_client *client)
{
	struct regmap *regmap;
	static const struct regmap_config config = {
		.reg_bits = 8,
		.val_bits = 8,
	};

	regmap = devm_regmap_init_i2c(client, &config);
	if (IS_ERR(regmap)) {
		dev_err(&client->dev, "regmap allocation failed: %ld\n",
			PTR_ERR(regmap));
		return PTR_ERR(regmap);
	}

	return cyttsp5_probe(&client->dev, regmap, client->irq, client->name);
}

static const struct of_device_id cyttsp5_of_match[] = {
	{ .compatible = "cypress,tt21000", },
	{ .compatible = "cypress,tma448", },
	{ }
};
MODULE_DEVICE_TABLE(of, cyttsp5_of_match);

static const struct i2c_device_id cyttsp5_i2c_id[] = {
	{ CYTTSP5_NAME },
	{ }
};
MODULE_DEVICE_TABLE(i2c, cyttsp5_i2c_id);

static int __maybe_unused cyttsp5_suspend(struct device *dev)
{
	struct cyttsp5 *ts = dev_get_drvdata(dev);

	if (!device_may_wakeup(dev))
		cyttsp5_power_control(ts, false);

	return 0;
}

static int __maybe_unused cyttsp5_resume(struct device *dev)
{
	struct cyttsp5 *ts = dev_get_drvdata(dev);

	if (!device_may_wakeup(dev))
		cyttsp5_power_control(ts, true);

	return 0;
}

static SIMPLE_DEV_PM_OPS(cyttsp5_pm, cyttsp5_suspend, cyttsp5_resume);

static struct i2c_driver cyttsp5_i2c_driver = {
	.driver = {
		.name = CYTTSP5_NAME,
		.of_match_table = cyttsp5_of_match,
		.pm = &cyttsp5_pm,
	},
	.probe = cyttsp5_i2c_probe,
	.id_table = cyttsp5_i2c_id,
};
module_i2c_driver(cyttsp5_i2c_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Touchscreen driver for Cypress TrueTouch Gen 5 Product");
MODULE_AUTHOR("Mylène Josserand <mylene.josserand@bootlin.com>");
