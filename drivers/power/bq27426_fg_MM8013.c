//zhangchao@wind-mobi.com 20180226 begin
/*
 * bqGauge battery driver
 *
 * Copyright (C) 2008 Rodolfo Giometti <giometti@linux.it>
 * Copyright (C) 2008 Eurotech S.p.A. <info@eurotech.it>
 * Copyright (C) 2010-2011 Lars-Peter Clausen <lars@metafoo.de>
 * Copyright (C) 2011 Pali Rohár <pali.rohar@gmail.com>
 *
 * Based on a previous work by Copyright (C) 2008 Texas Instruments, Inc.
 *
 * This package is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * THIS PACKAGE IS PROVIDED ``AS IS'' AND WITHOUT ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, WITHOUT LIMITATION, THE IMPLIED
 * WARRANTIES OF MERCHANTIBILITY AND FITNESS FOR A PARTICULAR PURPOSE.
 *
 */

/*
 * Datasheets:
 */
#define pr_fmt(fmt)	"bq27426- %s: " fmt, __func__
#include <linux/module.h>
#include <linux/param.h>
#include <linux/jiffies.h>
#include <linux/workqueue.h>
#include <linux/delay.h>
#include <linux/platform_device.h>
#include <linux/power_supply.h>
#include <linux/idr.h>
#include <linux/i2c.h>
#include <linux/slab.h>
#include <linux/acpi.h>
#include <asm/unaligned.h>
#include <linux/uaccess.h>
#include <linux/interrupt.h>
#include <linux/gpio/consumer.h>
#include <linux/debugfs.h>
#include <linux/qpnp/qpnp-adc.h>
#include <linux/alarmtimer.h>
#include "mm8013_cmd_type.h"
//#include "bq27426_gmfs.h"
//zhangchao@wind-mobi.com 20180305 begin
#include <linux/gpio.h>
#include <linux/of_gpio.h>
//zhangchao@wind-mobi.com 20180305 end

#if 1
#undef pr_debug
#define pr_debug pr_err
#undef pr_info
#define pr_info pr_err
#undef dev_dbg
#define dev_dbg dev_err
#else
#undef pr_info
#define pr_info pr_debug
#endif

#define	MONITOR_ALARM_CHECK_NS	5000000000
#define	INVALID_REG_ADDR	0xFF
#define BQFS_UPDATE_KEY		0x8F91


#define	FG_FLAGS_OT					BIT(15)
#define	FG_FLAGS_UT					BIT(14)
#define	FG_FLAGS_FC					BIT(9)
#define	FG_FLAGS_CHG				BIT(8)
#define	FG_FLAGS_OCVTAKEN			BIT(7)
#define	FG_FLAGS_SOC1				BIT(2)
#define	FG_FLAGS_SOCF				BIT(1)
#define	FG_FLAGS_DSG				BIT(0)


enum bq_fg_reg_idx {
	BQ_FG_REG_CTRL = 0,
	BQ_FG_REG_TEMP,		/* Battery Temperature */
	BQ_FG_REG_VOLT,		/* Battery Voltage */
	BQ_FG_REG_AI,		/* Average Current */
	BQ_FG_REG_FLAGS,	/* Flags */
	BQ_FG_REG_TTE,		/* Time to Empty */
	BQ_FG_REG_TTF,		/* Time to Full */
	BQ_FG_REG_FCC,		/* Full Charge Capacity */
	BQ_FG_REG_RM,		/* Remaining Capacity */
	BQ_FG_REG_CC,		/* Cycle Count */
	BQ_FG_REG_SOC,		/* Relative State of Charge */
	BQ_FG_REG_SOH,		/* State of Health */
	BQ_FG_REG_DC,		/* Design Capacity */
	
	NUM_REGS,
};
 
enum bq_fg_subcmd {
	FG_SUBCMD_CTRL_STATUS	= 0x0000,
	FG_SUBCMD_PART_NUM		= 0x0001,
	FG_SUBCMD_FW_VER		= 0x0002,
	FG_SUBCMD_SEAL			= 0x0020,
	FG_SUBCMD_PULSE_SOC_INT	= 0x0023,
	FG_SUBCMD_CHEM_A		= 0x0030,
	FG_SUBCMD_CHEM_B		= 0x0031,
	FG_SUBCMD_CHEM_C		= 0x0032,
};


enum {
	SEAL_STATE_FA,
	SEAL_STATE_UNSEALED,
	SEAL_STATE_SEALED,
};


enum bq_fg_device {
	BQ27X00,
	BQ27426,
};

enum {
	UPDATE_REASON_FG_RESET = 1,
	UPDATE_REASON_NEW_VERSION,
	UPDATE_REASON_FORCED,
};

struct fg_batt_profile {
	const bqfs_cmd_t * bqfs_image;
	u32				   array_size;
	u8				   version;
};

struct batt_chem_id {
	u16 id;
	u16 cmd;
};

const unsigned char *device2str[] = {
	"bq27x00",
	"bq27426",
};

const unsigned char *update_reason_str[] = {
	"Reset", 
	"New Version", 
	"Force" 
};

static u8 bq27426_regs[NUM_REGS] = {
	0x00,	/* CONTROL */
	0x06,	/* TEMP */
	0x08,	/* VOLT */
	0x14,	/* AVG CURRENT */
	0x0A,	/* FLAGS */
	0x16,	/* Time to empty */
	0x18,	/* Time to full */
	0x12,	/* Full charge capacity */
	0x10,	/* Remaining Capacity */
	0x2A,	/* CycleCount */
	0x2C,	/* State of Charge */
	0x2E,	/* State of Health */
	0x3C,	/* Design Capacity */
};

struct bq_fg_chip;

enum {
	BATTERY_PROFILE_A,
	BATTERY_PROFILE_B,
	BATTERY_PROFILE_MAX,
};

struct bq_fg_chip {
	struct device		*dev;
	struct i2c_client	*client;


	struct mutex i2c_rw_lock;
	struct mutex data_lock;
	struct mutex update_lock;
	struct mutex irq_complete;

	bool irq_waiting;
	bool irq_disabled;
	bool resume_completed;
	
	int	 force_update;
	int	 fw_ver;
	int	 df_ver;

	u8	chip;
	u8	regs[NUM_REGS];

	int	 batt_id;

	/* status tracking */	

	bool batt_present;
	bool batt_fc;
	bool batt_ot;
	bool batt_ut;
	bool batt_soc1;
	bool batt_socf;
	bool batt_dsg;
	bool allow_chg;
	bool cfg_update_mode;
	bool itpor;

	int	seal_state; /* 0 - Full Access, 1 - Unsealed, 2 - Sealed */
	int batt_tte;
	int	batt_soc;
	int batt_fcc;	/* Full charge capacity */
	int batt_rm;	/* Remaining capacity */
	int	batt_dc;	/* Design Capacity */	
	int	batt_volt;
	int	batt_temp;
	int	batt_curr;

	int batt_cyclecnt;	/* cycle count */


	struct work_struct update_work;
	
	unsigned long last_update;

	/* debug */
	int	skip_reads;
	int	skip_writes;

	int fake_soc;
	int fake_temp;

	struct dentry *debug_root;

	struct power_supply fg_psy;

	struct qpnp_vadc_chip	*vadc_dev;
	struct regulator		*vdd;
	int	connected_rid;                  
};



static int __fg_read_byte(struct i2c_client *client, u8 reg, u8 *val)
{
	s32 ret;

	ret = i2c_smbus_read_byte_data(client, reg);
	if (ret < 0) {
		pr_err("i2c read byte fail: can't read from reg 0x%02X\n", reg);
		return ret;
	}

	*val = (u8)ret;
	
	return 0;
}

static int __fg_write_byte(struct i2c_client *client, u8 reg, u8 val)
{
	s32 ret;

	ret = i2c_smbus_write_byte_data(client, reg, val);
	if (ret < 0) {
		pr_err("i2c write byte fail: can't write 0x%02X to reg 0x%02X\n", 
				val, reg);
		return ret;
	}

	return 0;
}


static int __fg_read_word(struct i2c_client *client, u8 reg, u16 *val)
{
	s32 ret;

	ret = i2c_smbus_read_word_data(client, reg);
	if (ret < 0) {
		pr_err("i2c read word fail: can't read from reg 0x%02X\n", reg);
		return ret;
	}

	*val = (u16)ret;
	
	return 0;
}


static int __fg_write_word(struct i2c_client *client, u8 reg, u16 val)
{
	s32 ret;

	ret = i2c_smbus_write_word_data(client, reg, val);
	if (ret < 0) {
		pr_err("i2c write word fail: can't write 0x%02X to reg 0x%02X\n", 
				val, reg);
		return ret;
	}

	return 0;
}

static int __fg_read_block(struct i2c_client *client, u8 reg, u8 *buf, u8 len)
{
	int ret;
	struct i2c_msg msg[2];
	int i;

	msg[0].addr = client->addr;
	msg[0].flags = 0;
	msg[0].buf = &reg;
	msg[0].len = 1;

	msg[1].addr = client->addr;
	msg[1].flags = I2C_M_RD;
	msg[1].buf = buf;
	msg[1].len = len;

	for (i = 0; i < 3; i++) {
		ret = i2c_transfer(client->adapter, msg, ARRAY_SIZE(msg));
		if (ret >= 0)
			return ret;
		else
			msleep(5);
	}
	return ret;
}

static int __fg_write_block(struct i2c_client *client, u8 reg, u8 *buf, u8 len)
{
	int ret;
	struct i2c_msg msg;
	u8 data[64];
	int i = 0;

	data[0] = reg;
	memcpy(&data[1], buf, len);

	msg.addr = client->addr;
	msg.flags = 0;
	msg.buf = data;
	msg.len = len + 1;

	for (i = 0; i < 3; i++) {
		ret = i2c_transfer(client->adapter, &msg, 1);
		if (ret >= 0)
			return ret;
		else
			msleep(5);
	}
	return ret;
}


static int fg_read_byte(struct bq_fg_chip *bq, u8 reg, u8 *val)
{
	int ret;

	if (bq->skip_reads) {
		*val = 0;
		return 0;
	}

	mutex_lock(&bq->i2c_rw_lock);
	ret = __fg_read_byte(bq->client, reg, val);
	mutex_unlock(&bq->i2c_rw_lock);

	return ret;
}

static int fg_write_byte(struct bq_fg_chip *bq, u8 reg, u8 val)
{
	int ret;
	
	if (bq->skip_writes) 
		return 0;

	mutex_lock(&bq->i2c_rw_lock);
	ret = __fg_write_byte(bq->client, reg, val);
	mutex_unlock(&bq->i2c_rw_lock);
	
	return ret;
}

static int fg_read_word(struct bq_fg_chip *bq, u8 reg, u16 *val)
{
	int ret;

	if (bq->skip_reads) {
		*val = 0;
		return 0;
	}
	
	mutex_lock(&bq->i2c_rw_lock);
	ret = __fg_read_word(bq->client, reg, val);
	mutex_unlock(&bq->i2c_rw_lock);

	return ret;
}

static int fg_write_word(struct bq_fg_chip *bq, u8 reg, u16 val)
{
	int ret;

	if (bq->skip_writes) 
		return 0;

	mutex_lock(&bq->i2c_rw_lock);
	ret = __fg_write_word(bq->client, reg, val);
	mutex_unlock(&bq->i2c_rw_lock);
	
	return ret;
}

static int fg_read_block(struct bq_fg_chip *bq, u8 reg, u8 *buf, u8 len)
{
	int ret;

	if (bq->skip_reads)
		return 0;
	mutex_lock(&bq->i2c_rw_lock);
	ret = __fg_read_block(bq->client, reg, buf, len);
	mutex_unlock(&bq->i2c_rw_lock);
	
	return ret;
	
}

static int fg_write_block(struct bq_fg_chip *bq, u8 reg, u8 *data, u8 len)
{
	int ret;

	if (bq->skip_writes)
		return 0;
	
	mutex_lock(&bq->i2c_rw_lock);
	ret = __fg_write_block(bq->client, reg, data, len);
	mutex_unlock(&bq->i2c_rw_lock);

	return ret;
}

#define	CTRL_REG					0x00

#define	FG_DFT_UNSEAL_KEY1				0x80008000
#define	FG_DFT_UNSEAL_KEY2				0x36724614

#define	FG_DFT_UNSEAL_FA_KEY			0xFFFFFFFF

static u8 checksum(u8 *data, u8 len)
{
	u8 i;
	u16 sum = 0;

	for (i = 0; i < len; i++)
		sum += data[i];
	
	sum &= 0xFF;

	return (0xFF - sum);
}

#if 0
static void fg_print_buf(const char *msg, u8 *buf, u8 len)
{
	int i;
	int idx = 0;
	int num;
	u8 strbuf[128];

	pr_err("%s buf: ", msg);
	for(i = 0; i < len; i++) {
		num = sprintf(&strbuf[idx], "%02X ", buf[i]);
		idx += num;
	}
	pr_err("%s\n", strbuf);
}
#else
static void fg_print_buf(const char *msg, u8 *buf, u8 len)
{}
#endif


#define TIMEOUT_INIT_COMPLETED	100
static int fg_check_init_completed(struct bq_fg_chip *bq)
{
	int ret;
	int i = 0;
	u16 status;

	ret = fg_write_word(bq, bq->regs[BQ_FG_REG_CTRL], FG_SUBCMD_CTRL_STATUS);
	if (ret < 0) {
		pr_err("Failed to write control status cmd, ret = %d\n", ret);
		return ret;
	}
	
	msleep(5);

	while (i++ < TIMEOUT_INIT_COMPLETED) {
		ret = fg_read_word(bq, bq->regs[BQ_FG_REG_CTRL], &status);
		if (ret >= 0 && (status & 0x0080))
			return 0;
		msleep(100);
	}
	pr_err("wait for FG INITCOMP timeout\n");
	return ret;
}

static int fg_get_seal_state(struct bq_fg_chip *bq)
{
	int ret;
	u16 status;

	ret = fg_write_word(bq, bq->regs[BQ_FG_REG_CTRL], FG_SUBCMD_CTRL_STATUS);
	if (ret < 0) {
		pr_err("Failed to write control status cmd, ret = %d\n", ret);
		return ret;
	}
	
	msleep(5);

	ret = fg_read_word(bq, bq->regs[BQ_FG_REG_CTRL], &status);
	if (ret < 0) {
		pr_err("Failed to read control status, ret = %d\n", ret);
		return ret;
	}
	pr_err("control_status = 0x%04X", status);
	if (status & 0x2000)
		bq->seal_state = SEAL_STATE_SEALED;
	else
		bq->seal_state = SEAL_STATE_UNSEALED;

	return 0;
}

static int fg_unseal_fa(struct bq_fg_chip *bq, u32 key)
{
	int ret;
	int retry = 0;

	pr_info(":key - %d\n", key);

	ret = fg_write_word(bq, bq->regs[BQ_FG_REG_CTRL], key & 0xFFFF);
	if (ret < 0) {
		pr_err("unable to write unseal key step 1, ret = %d\n", ret);
		return ret;
	}

	msleep(5);

	ret = fg_write_word(bq, bq->regs[BQ_FG_REG_CTRL], (key >> 16) & 0xFFFF);
	if (ret < 0) {
		pr_err("unable to write unseal key step 2, ret = %d\n", ret);
		return ret;
	}

	msleep(5);

	while (retry++ < 1000) {
		fg_get_seal_state(bq);
		if (bq->seal_state == SEAL_STATE_FA) {
			return 0;
		}
		msleep(10);
	}

	return -1;
}
EXPORT_SYMBOL_GPL(fg_unseal_fa);

static int fg_seal(struct bq_fg_chip *bq)
{
	int ret;
	int retry = 0;

	fg_get_seal_state(bq);

	if (bq->seal_state == SEAL_STATE_SEALED)
		return 0;
	msleep(5);
	ret = fg_write_word(bq, bq->regs[BQ_FG_REG_CTRL], FG_SUBCMD_SEAL);

	if (ret < 0) {
		pr_err("Failed to send seal command\n");
		return ret;
	}

	while (retry++ < 1000) {
		fg_get_seal_state(bq);
		if (bq->seal_state == SEAL_STATE_SEALED)
			return 0;
		msleep(200);
	}

	return -1;
}

#define	CFG_UPDATE_POLLING_RETRY_LIMIT	50
static int fg_dm_pre_access(struct bq_fg_chip *bq)
{
	int ret;

	ret = fg_check_init_completed(bq);
	if (ret < 0)
		return ret; 

	pr_err("Failed to enter cfgupdate mode\n");

	return -1;
}
EXPORT_SYMBOL_GPL(fg_dm_pre_access);

static int fg_dm_post_access(struct bq_fg_chip *bq)
{
	int i = 0;

	i = CFG_UPDATE_POLLING_RETRY_LIMIT;

	if (i == CFG_UPDATE_POLLING_RETRY_LIMIT) {
		pr_err("Failed to exit cfgupdate mode\n");
		return -1;
	} else {
		return fg_seal(bq);
	}
}
EXPORT_SYMBOL_GPL(fg_dm_post_access);

#define	DM_ACCESS_BLOCK_DATA_CHKSUM	0x60
#define	DM_ACCESS_BLOCK_DATA_CTRL	0x61
#define	DM_ACCESS_BLOCK_DATA_CLASS	0x3E
#define	DM_ACCESS_DATA_BLOCK		0x3F
#define	DM_ACCESS_BLOCK_DATA		0x40


static int fg_dm_read_block(struct bq_fg_chip *bq, u8 classid, 
							u8 offset, u8 *buf)
{
	int ret;
	u8 cksum_calc, cksum;
	u8 blk_offset = offset >> 5;

	pr_info("subclass:%d, offset:%d\n", classid, offset);

	ret = fg_write_byte(bq, DM_ACCESS_BLOCK_DATA_CTRL, 0);
	if (ret < 0)
		return ret;
	msleep(5);
	ret = fg_write_byte(bq, DM_ACCESS_BLOCK_DATA_CLASS, classid);
	if (ret < 0)
		return ret;
	msleep(5);
	ret = fg_write_byte(bq, DM_ACCESS_DATA_BLOCK, blk_offset);
	if (ret < 0)
		return ret;
	msleep(5);
	ret = fg_read_block(bq, DM_ACCESS_BLOCK_DATA, buf, 32);
	if (ret < 0)
		return ret;
	
	fg_print_buf(__func__, buf, 32);

	msleep(5);
	cksum_calc = checksum(buf, 32);
	ret = fg_read_byte(bq, DM_ACCESS_BLOCK_DATA_CHKSUM, &cksum);
	if (!ret && cksum_calc == cksum)
		return 0;
	else
		return 1;
}
EXPORT_SYMBOL_GPL(fg_dm_read_block);

static int fg_dm_write_block(struct bq_fg_chip *bq, u8 classid,
							u8 offset, u8 *data)
{
	int ret;
	u8 cksum;
	u8 buf[64];
	u8 blk_offset = offset >> 5;

	pr_info("subclass:%d, offset:%d\n", classid, offset);

	ret = fg_write_byte(bq, DM_ACCESS_BLOCK_DATA_CTRL, 0);
	if (ret < 0)
		return ret;
	msleep(5);
	ret = fg_write_byte(bq, DM_ACCESS_BLOCK_DATA_CLASS, classid);
	if (ret < 0)
		return ret;
	msleep(5);
	ret = fg_write_byte(bq, DM_ACCESS_DATA_BLOCK, blk_offset);
	if (ret < 0)
		return ret;
	ret = fg_write_block(bq, DM_ACCESS_BLOCK_DATA, data, 32);
	msleep(5);
	
	fg_print_buf(__func__, data, 32);
	
	cksum = checksum(data, 32);
	ret = fg_write_byte(bq, DM_ACCESS_BLOCK_DATA_CHKSUM, cksum);
	if (ret < 0)
		return ret;
	msleep(5);

	ret = fg_write_byte(bq, DM_ACCESS_DATA_BLOCK, blk_offset);
	if (ret < 0)
		return ret;
	msleep(5);
	ret = fg_read_block(bq, DM_ACCESS_BLOCK_DATA, buf, 32);
	if (ret < 0)
		return ret;
	if (memcpy(data, buf, 32)) {
		pr_err("Error updating subclass %d offset %d\n",
				classid, offset);
		return 1;
	}
	return 0;
}
EXPORT_SYMBOL_GPL(fg_dm_write_block);

static int fg_read_fw_version(struct bq_fg_chip *bq)
{

	int ret;
	u16 version;

	ret = fg_write_word(bq, bq->regs[BQ_FG_REG_CTRL], 0x0002);

	if (ret < 0) {
		pr_err("Failed to send firmware version subcommand:%d\n", ret);
		return ret;
	}

	mdelay(2);

	ret = fg_read_word(bq, bq->regs[BQ_FG_REG_CTRL], &version);
	if (ret < 0) {
		pr_err("Failed to read firmware version:%d\n", ret);
		return ret;
	}
	
	return version;
}


static int fg_read_status(struct bq_fg_chip *bq)
{
	int ret;
	u16 flags;

	ret = fg_read_word(bq, bq->regs[BQ_FG_REG_FLAGS], &flags);
	if (ret < 0) {
		return ret;
	}

	mutex_lock(&bq->data_lock);
	bq->batt_present	= 1;

	bq->batt_ot			= !!(flags & FG_FLAGS_OT);
	bq->batt_ut			= !!(flags & FG_FLAGS_UT);
	bq->batt_fc			= !!(flags & FG_FLAGS_FC);
	bq->batt_soc1		= !!(flags & FG_FLAGS_SOC1);
	bq->batt_socf		= !!(flags & FG_FLAGS_SOCF);
	bq->batt_dsg		= !!(flags & FG_FLAGS_DSG);
	bq->allow_chg		= !!(flags & FG_FLAGS_CHG);
	mutex_unlock(&bq->data_lock);

	return 0;
}


static int fg_read_rsoc(struct bq_fg_chip *bq)
{
	int ret;
	u16 soc = 0;
	
	ret = fg_read_word(bq, bq->regs[BQ_FG_REG_SOC], &soc);
	if (ret < 0) {
		pr_err("could not read RSOC, ret = %d\n", ret);
		return ret;
	}

	return soc;

}

static int fg_read_temperature(struct bq_fg_chip *bq)
{
	int ret;
	u16 temp = 0;
	
	ret = fg_read_word(bq, bq->regs[BQ_FG_REG_TEMP], &temp);
	if (ret < 0) {
		pr_err("could not read temperature, ret = %d\n", ret);
		return ret;
	}

	return temp;       

}

static int fg_read_volt(struct bq_fg_chip *bq)
{
	int ret;
	u16 volt = 0;
	
	ret = fg_read_word(bq, bq->regs[BQ_FG_REG_VOLT], &volt);
	if (ret < 0) {
		pr_err("could not read voltage, ret = %d\n", ret);
		return ret;
	}

	return volt;

}

static int fg_read_current(struct bq_fg_chip *bq, int *curr)
{
	int ret;
	u16 avg_curr = 0;
	
	ret = fg_read_word(bq, bq->regs[BQ_FG_REG_AI], &avg_curr);
	if (ret < 0) {
		pr_err("could not read current, ret = %d\n", ret);
		return ret;
	}
	*curr = (int)((s16)avg_curr);

	return ret;
}

static int fg_read_fcc(struct bq_fg_chip *bq)
{
	int ret;
	u16 fcc;

	if (bq->regs[BQ_FG_REG_FCC] == INVALID_REG_ADDR) {
		pr_err("FCC command not supported!\n");
		return 0;
	}
	
	ret = fg_read_word(bq, bq->regs[BQ_FG_REG_FCC], &fcc);

	if (ret < 0) {
		pr_err("could not read FCC, ret=%d\n", ret);
	}

	return fcc;
}

static int fg_read_dc(struct bq_fg_chip *bq)
{

	int ret;
	u16 dc;

	if (bq->regs[BQ_FG_REG_DC] == INVALID_REG_ADDR) {
		pr_err("DesignCapacity command not supported!\n");
		return 0;
	}
	
	ret = fg_read_word(bq, bq->regs[BQ_FG_REG_DC], &dc);

	if (ret < 0) {
		pr_err("could not read DC, ret=%d\n", ret);
		return ret;
	}

	return dc;
}


static int fg_read_rm(struct bq_fg_chip *bq)
{
	int ret;
	u16 rm;

	if (bq->regs[BQ_FG_REG_RM] == INVALID_REG_ADDR) {
		pr_err("RemainingCapacity command not supported!\n");
		return 0;
	}
	
	ret = fg_read_word(bq, bq->regs[BQ_FG_REG_RM], &rm);

	if (ret < 0) {
		pr_err("could not read DC, ret=%d\n", ret);
		return ret;
	}

	return rm;

}

static int fg_read_cyclecount(struct bq_fg_chip *bq)
{
	int ret;
	u16 cc;

	if (bq->regs[BQ_FG_REG_CC] == INVALID_REG_ADDR) {
		pr_err("Cycle Count not supported!\n");
		return -1;
	}
	
	ret = fg_read_word(bq, bq->regs[BQ_FG_REG_CC], &cc);

	if (ret < 0) {
		pr_err("could not read Cycle Count, ret=%d\n", ret);
		return ret;
	}

	return cc;
}

static int fg_read_tte(struct bq_fg_chip *bq)
{
	int ret;
	u16 tte;

	if (bq->regs[BQ_FG_REG_TTE] == INVALID_REG_ADDR) {
		pr_err("Time To Empty not supported!\n");
		return -1;
	}
	
	ret = fg_read_word(bq, bq->regs[BQ_FG_REG_TTE], &tte);

	if (ret < 0) {
		pr_err("could not read Time To Empty, ret=%d\n", ret);
		return ret;
	}

	if (ret == 0xFFFF)
		return -ENODATA;

	return tte;
}

static int fg_get_batt_status(struct bq_fg_chip *bq)
{

	fg_read_status(bq);

	if (!bq->batt_present)
		return POWER_SUPPLY_STATUS_UNKNOWN;
	else if (bq->batt_fc)
		return POWER_SUPPLY_STATUS_FULL;
	else if (bq->batt_dsg)
		return POWER_SUPPLY_STATUS_DISCHARGING;
	else if (bq->batt_curr > 0)
		return POWER_SUPPLY_STATUS_CHARGING;
	else
		return POWER_SUPPLY_STATUS_NOT_CHARGING;

}


static int fg_get_batt_capacity_level(struct bq_fg_chip *bq)
{
	if (!bq->batt_present)
		return POWER_SUPPLY_CAPACITY_LEVEL_UNKNOWN;
	else if (bq->batt_fc)
		return POWER_SUPPLY_CAPACITY_LEVEL_FULL;
	else if (bq->batt_soc1)
		return POWER_SUPPLY_CAPACITY_LEVEL_LOW;
	else if (bq->batt_socf)		
		return POWER_SUPPLY_CAPACITY_LEVEL_CRITICAL;
	else
		return POWER_SUPPLY_CAPACITY_LEVEL_NORMAL;

}


static int fg_get_batt_health(struct bq_fg_chip *bq)
{
	if (!bq->batt_present)
		return POWER_SUPPLY_HEALTH_UNKNOWN;
	else if (bq->batt_ot)
		return POWER_SUPPLY_HEALTH_OVERHEAT;
	else if (bq->batt_ut)
		return POWER_SUPPLY_HEALTH_COLD;
	else
		return POWER_SUPPLY_HEALTH_GOOD;

}
#if 0
static void parse_dt(struct bq_fg_chip *bq)
{
	
}
#endif

static enum power_supply_property fg_props[] = {
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
	POWER_SUPPLY_PROP_CURRENT_NOW,
	POWER_SUPPLY_PROP_CAPACITY,
	POWER_SUPPLY_PROP_CAPACITY_LEVEL,
	POWER_SUPPLY_PROP_TEMP,
//	POWER_SUPPLY_PROP_TIME_TO_EMPTY_NOW,
	POWER_SUPPLY_PROP_CHARGE_FULL,
//	POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN,
//	POWER_SUPPLY_PROP_CYCLE_COUNT,
	POWER_SUPPLY_PROP_HEALTH,
	POWER_SUPPLY_PROP_TECHNOLOGY,
	POWER_SUPPLY_PROP_RESISTANCE_ID,
	POWER_SUPPLY_PROP_UPDATE_NOW,
};

static int fg_get_property(struct power_supply *psy, enum power_supply_property psp,
					union power_supply_propval *val)
{
	struct bq_fg_chip *bq = container_of(psy, struct bq_fg_chip, fg_psy);
	int ret;

	mutex_lock(&bq->update_lock);
	switch (psp) {
	case POWER_SUPPLY_PROP_STATUS:
		val->intval = fg_get_batt_status(bq);
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_NOW:
		ret = fg_read_volt(bq);
		mutex_lock(&bq->data_lock);
		if (ret >= 0)
			bq->batt_volt = ret;
		val->intval = bq->batt_volt * 1000;
		mutex_unlock(&bq->data_lock);

		break;
	case POWER_SUPPLY_PROP_PRESENT:
		val->intval = bq->batt_present;
		break;
	case POWER_SUPPLY_PROP_CURRENT_NOW:
		mutex_lock(&bq->data_lock);
		fg_read_current(bq, &bq->batt_curr);
		val->intval = -bq->batt_curr * 1000;
		pr_info("bq27426 current=%d\n", val->intval);
		mutex_unlock(&bq->data_lock);
		break;

	case POWER_SUPPLY_PROP_CAPACITY:
		if (bq->fake_soc >= 0) {
			val->intval = bq->fake_soc;
			break;
		}
		ret = fg_read_rsoc(bq);
		mutex_lock(&bq->data_lock);
		if (ret >= 0)
			bq->batt_soc = ret;
		val->intval = bq->batt_soc;
		mutex_unlock(&bq->data_lock);
		break;

	case POWER_SUPPLY_PROP_CAPACITY_LEVEL:
		val->intval = fg_get_batt_capacity_level(bq);
		break;

	case POWER_SUPPLY_PROP_TEMP:
		if (bq->fake_temp != -EINVAL){
			val->intval = bq->fake_temp;
			break;
		}
		ret = fg_read_temperature(bq);
		mutex_lock(&bq->data_lock);
		if (ret > 0)
			bq->batt_temp = ret;
		val->intval = bq->batt_temp - 2730;
		mutex_unlock(&bq->data_lock);
		break;

	case POWER_SUPPLY_PROP_TIME_TO_EMPTY_NOW:
		ret = fg_read_tte(bq);
		mutex_lock(&bq->data_lock);
		if (ret >=0)
			bq->batt_tte = ret;
	
		val->intval = bq->batt_tte;
		mutex_unlock(&bq->data_lock);
		break;

	case POWER_SUPPLY_PROP_CHARGE_FULL:
		ret = fg_read_fcc(bq);
		mutex_lock(&bq->data_lock);
		if (ret > 0)
			bq->batt_fcc = ret;
		val->intval = bq->batt_fcc * 1000;
		mutex_unlock(&bq->data_lock);
		break;

	case POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN:
		ret = fg_read_dc(bq);
		mutex_lock(&bq->data_lock);
		if (ret > 0)
			bq->batt_dc = ret;
		val->intval = bq->batt_dc * 1000;
		mutex_unlock(&bq->data_lock);
		break;

	case POWER_SUPPLY_PROP_CYCLE_COUNT:
		ret = fg_read_cyclecount(bq);
		mutex_lock(&bq->data_lock);
		if (ret >= 0)
			bq->batt_cyclecnt = ret;
		val->intval = bq->batt_cyclecnt;
		mutex_unlock(&bq->data_lock);
		break;

	case POWER_SUPPLY_PROP_HEALTH:
		val->intval = fg_get_batt_health(bq);
		break;

	case POWER_SUPPLY_PROP_TECHNOLOGY:
		val->intval = POWER_SUPPLY_TECHNOLOGY_LIPO;
		break;

	case POWER_SUPPLY_PROP_RESISTANCE_ID:
		val->intval = bq->connected_rid ;
		break;
	case POWER_SUPPLY_PROP_UPDATE_NOW:
		val->intval = 0;
		break;

	default:
		mutex_unlock(&bq->update_lock);
		return -EINVAL;
	}
	mutex_unlock(&bq->update_lock);
	return 0;
}
static void fg_dump_registers(struct bq_fg_chip *bq);

static int fg_set_property(struct power_supply *psy,
				       enum power_supply_property prop,
				       const union power_supply_propval *val)
{
	struct bq_fg_chip *bq = container_of(psy, struct bq_fg_chip, 
									fg_psy);
	switch (prop) {
	case POWER_SUPPLY_PROP_TEMP:
		bq->fake_temp = val->intval;
		break;
	case POWER_SUPPLY_PROP_CAPACITY:
		bq->fake_soc = val->intval;
		power_supply_changed(&bq->fg_psy);
		break;
	case POWER_SUPPLY_PROP_UPDATE_NOW:
		fg_dump_registers(bq);
		break;
	default:
		return -EINVAL;
	}

	return 0;
}


static int fg_prop_is_writeable(struct power_supply *psy,
				       enum power_supply_property prop)
{
	int ret;

	switch (prop) {
	case POWER_SUPPLY_PROP_TEMP:
	case POWER_SUPPLY_PROP_CAPACITY:
	case POWER_SUPPLY_PROP_UPDATE_NOW:
		ret = 1;
		break;
	default:
		ret = 0;
		break;
	}
	return ret;
}



static void fg_external_power_changed(struct power_supply *psy)
{
#if 0
	struct bq_fg_chip *bq = container_of(psy, struct bq_fg_chip, fg_psy);

	cancel_delayed_work(&bq->monitor_work);
	schedule_delayed_work(&bq->monitor_work, 0);
#endif

}

static int fg_psy_register(struct bq_fg_chip *bq)
{
	int ret;

	bq->fg_psy.name = "ex_fg";
	bq->fg_psy.type = POWER_SUPPLY_TYPE_BMS;
	bq->fg_psy.properties = fg_props;
	bq->fg_psy.num_properties = ARRAY_SIZE(fg_props);
	bq->fg_psy.get_property = fg_get_property;
	bq->fg_psy.set_property = fg_set_property;
	bq->fg_psy.external_power_changed = fg_external_power_changed;
	bq->fg_psy.property_is_writeable = fg_prop_is_writeable;


	ret = power_supply_register(bq->dev, &bq->fg_psy);
	if (ret < 0) {
		pr_err("Failed to register fg_psy:%d\n", ret);
		return ret;
	}

	return 0;
}


static void fg_psy_unregister(struct bq_fg_chip *bq)
{

	power_supply_unregister(&bq->fg_psy);
}

static const u8 fg_dump_regs[] = {
	0x00, 0x06, 0x08, 0x0A, 
	0x0C, 0x0E, 0x10, 0x12, 
	0x14, 0x16, 0x18, 0x22,
	0x2C, 0x28, 0x2E, 0x2A, 
	0x24, 0x26, 0x34, 0x36,		// MM8013 dummy
	0x66, 0x68, 0x6C, 0x6E,		// MM8013 dummy
	0x70,						// MM8013 dummy
};

static int show_registers(struct seq_file *m, void *data)
{
	struct bq_fg_chip *bq = m->private;
	int i;
	int ret;
	u16 val = 0;
	
	for (i = 0; i < ARRAY_SIZE(fg_dump_regs); i++) {
		msleep(5);
		ret = fg_read_word(bq, fg_dump_regs[i], &val);
		if (!ret)
			seq_printf(m, "Reg[%02X] = 0x%04X\n", 
						fg_dump_regs[i], val);
	}
	return 0;	
}


static int reg_debugfs_open(struct inode *inode, struct file *file)
{
	struct bq_fg_chip *bq = inode->i_private;
	
	return single_open(file, show_registers, bq);
}

static const struct file_operations reg_debugfs_ops = {
	.owner		= THIS_MODULE,
	.open		= reg_debugfs_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
};

static void create_debugfs_entry(struct bq_fg_chip *bq)
{
	bq->debug_root = debugfs_create_dir("bq_fg", NULL);
	if (!bq->debug_root)
		pr_err("Failed to create debug dir\n");
	
	if (bq->debug_root) {
		
		debugfs_create_file("registers", S_IFREG | S_IRUGO,
						bq->debug_root, bq, &reg_debugfs_ops);

		debugfs_create_x32("fake_soc",
					  S_IFREG | S_IWUSR | S_IRUGO,
					  bq->debug_root,
					  &(bq->fake_soc));

		debugfs_create_x32("fake_temp",
					  S_IFREG | S_IWUSR | S_IRUGO,
					  bq->debug_root,
					  &(bq->fake_temp));
	
		debugfs_create_x32("skip_reads",
					  S_IFREG | S_IWUSR | S_IRUGO,
					  bq->debug_root,
					  &(bq->skip_reads));
		debugfs_create_x32("skip_writes",
					  S_IFREG | S_IWUSR | S_IRUGO,
					  bq->debug_root,
					  &(bq->skip_writes));
	}	
}

static ssize_t fg_attr_show_Ra_table(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct bq_fg_chip *bq = i2c_get_clientdata(client);

	int ret;
	u8 rd_buf[64];
	u8 temp_buf[100];
	int len, i, idx;
	u8 *err_str[] = {
		"Failed to enter configure mode",
		"Failed to Read Ra Table",
		"Failed to exit configure mode",
	};

	memset(buf, 0, 64);
	mutex_lock(&bq->update_lock);
	ret = fg_dm_pre_access(bq);
	if (ret) {
		sprintf(buf,"%s", err_str[0]);
		mutex_unlock(&bq->update_lock);
		return strlen(err_str[0]);
	}

	ret = fg_dm_read_block(bq, 89, 0, rd_buf);	//Ra Table
	if (ret) {
		sprintf(buf,"%s", err_str[1]);
		fg_dm_post_access(bq);
		mutex_unlock(&bq->update_lock);
		return strlen(err_str[1]);
	}

	fg_dm_post_access(bq);

	idx = 0;
	for (i = 0; i < 30; i++) {
		len = sprintf(temp_buf, "%02X ", rd_buf[i]);
		memcpy(&buf[idx], temp_buf, len);
		idx += len;
	}
	mutex_unlock(&bq->update_lock);
	return idx;
}

static ssize_t fg_attr_show_Qmax(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct bq_fg_chip *bq = i2c_get_clientdata(client);

	int ret;
	u8 rd_buf[64];
	int len;
	u8 *err_str[] = {
		"Failed to enter configure mode",
		"Failed to Qmax",
	};

	memset(buf, 0, 64);
	mutex_lock(&bq->update_lock);
	ret = fg_dm_pre_access(bq);
	if (ret) {
		sprintf(buf,"%s", err_str[0]);
		mutex_unlock(&bq->update_lock);
		return strlen(err_str[0]);
	}

	ret = fg_dm_read_block(bq, 82, 0, rd_buf);	//Qmax, offset 0
	if (ret) {
		sprintf(buf,"%s", err_str[1]);
		fg_dm_post_access(bq);
		mutex_unlock(&bq->update_lock);
		return strlen(err_str[1]);
	}
	
	len = sprintf(buf, "Qmax Cell 0 = %d\n", (rd_buf[0] << 8) | rd_buf[1]);
	fg_dm_post_access(bq);

	mutex_unlock(&bq->update_lock);
	return len;
}

static ssize_t fg_attr_store_update(struct device *dev,
				struct device_attribute *attr, const char *buf, size_t count)
{

	struct i2c_client *client = to_i2c_client(dev);
	struct bq_fg_chip *bq = i2c_get_clientdata(client);
	unsigned int key = 0;

	bq->connected_rid = 1;  //zhangchao
	sscanf(buf, "%x", &key);
	
	return count;	
}

static ssize_t fg_attr_show_dmcode(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct bq_fg_chip *bq = i2c_get_clientdata(client);
	
	bq->connected_rid = 1;  //zhangchao

	return sprintf(buf, "Read DM code error");
}
	


static DEVICE_ATTR(RaTable, S_IRUGO, fg_attr_show_Ra_table, NULL);
static DEVICE_ATTR(Qmax, S_IRUGO, fg_attr_show_Qmax, NULL);
static DEVICE_ATTR(update, S_IWUSR, NULL, fg_attr_store_update);
static DEVICE_ATTR(dmcode, S_IRUGO, fg_attr_show_dmcode, NULL);

static struct attribute *fg_attributes[] = {
	&dev_attr_RaTable.attr,
	&dev_attr_Qmax.attr,
	&dev_attr_update.attr,
	&dev_attr_dmcode.attr,
	NULL,
};

static const struct attribute_group fg_attr_group = {
	.attrs = fg_attributes,
};

static void fg_update_bqfs_workfunc(struct work_struct *work)
{
		struct bq_fg_chip *bq = container_of(work, 
							struct bq_fg_chip, update_work);

		bq->connected_rid = 1;  //zhangchao
}

static void fg_dump_registers(struct bq_fg_chip *bq)
{
	int i;
	int ret;
	u16 val;

	for (i = 0; i < ARRAY_SIZE(fg_dump_regs); i++) {
		msleep(5);
		ret = fg_read_word(bq, fg_dump_regs[i], &val);
		if (!ret)
			pr_err("Reg[%02X] = 0x%04X\n", fg_dump_regs[i], val);
	}
}

static irqreturn_t fg_irq_thread(int irq, void *dev_id)
{
	struct bq_fg_chip *bq = dev_id;
	bool last_batt_present;

	mutex_lock(&bq->irq_complete);
	bq->irq_waiting = true;
	if (!bq->resume_completed) {
		pr_info("IRQ triggered before device resume\n");
		if (!bq->irq_disabled) {
			disable_irq_nosync(irq);
			bq->irq_disabled = true;
		}
		mutex_unlock(&bq->irq_complete);
		return IRQ_HANDLED;
	}
	bq->irq_waiting = false;

	last_batt_present = bq->batt_present;

	mutex_lock(&bq->update_lock);
	fg_read_status(bq);
	mutex_unlock(&bq->update_lock);

	fg_dump_registers(bq);

	pr_info("itpor=%d, cfg_mode = %d, seal_state=%d, batt_present=%d", 
			bq->itpor, bq->cfg_update_mode, bq->seal_state, bq->batt_present);
	
	if (!last_batt_present && bq->batt_present ) {/* battery inserted */
		pr_info("Battery inserted\n");
	} else if (last_batt_present && !bq->batt_present) {/* battery removed */
		pr_info("Battery removed\n");
		bq->batt_soc	= -ENODATA;
		bq->batt_fcc	= -ENODATA;
		bq->batt_rm		= -ENODATA;
		bq->batt_volt	= -ENODATA;
		bq->batt_curr	= -ENODATA;
		bq->batt_temp	= -ENODATA;
		bq->batt_cyclecnt = -ENODATA;
	}
	
	if (bq->batt_present) {
		mutex_lock(&bq->update_lock);
		
		bq->batt_soc = fg_read_rsoc(bq);
		bq->batt_volt = fg_read_volt(bq);
		fg_read_current(bq, &bq->batt_curr);
		bq->batt_temp = fg_read_temperature(bq);
		bq->batt_rm = fg_read_rm(bq);

		mutex_unlock(&bq->update_lock);
		pr_err("RSOC:%d, Volt:%d, Current:%d, Temperature:%d, connected_rid = %d\n",
			bq->batt_soc, bq->batt_volt, bq->batt_curr, bq->batt_temp - 2730, bq->connected_rid);
	}

	power_supply_changed(&bq->fg_psy);
	mutex_unlock(&bq->irq_complete);

	return IRQ_HANDLED;
}


static void determine_initial_status(struct bq_fg_chip *bq)
{
	fg_irq_thread(bq->client->irq, bq);
}

#define SMB_VTG_MIN_UV		1800000
#define SMB_VTG_MAX_UV		1800000

static int bq_parse_dt(struct bq_fg_chip *bq)
{
	return 0;
}

//zhangchao@wind-mobi.com 20180305 begin
int g_otg_flag_pin;
static int otg_flag_enable(struct bq_fg_chip *bq, int flag)
{
	int val, otg_flag_pin;
	struct device_node *node = bq->dev->of_node;
	val = flag;
	otg_flag_pin = of_get_named_gpio(node, "ti,otg_flag", 0);
	g_otg_flag_pin = otg_flag_pin;
	if(gpio_is_valid(otg_flag_pin)){
		gpio_request(otg_flag_pin,"ti,otg_flag");
		gpio_direction_output(otg_flag_pin,val);
		pr_err("wind-log otg_flag val = %d\n",val);
	}
	return 0;
}

int g_pogo_en_pin;
static int pogo_enable(struct bq_fg_chip *bq, int flag)
{
	int val, pogo_en_pin;
	struct device_node *node = bq->dev->of_node;
	val = flag;
	pogo_en_pin = of_get_named_gpio(node, "ti,pogo_en", 0);
	g_pogo_en_pin = pogo_en_pin;
	if(gpio_is_valid(pogo_en_pin)){
		gpio_request(pogo_en_pin,"ti,pogo_en");
		gpio_direction_output(pogo_en_pin,val);
		pr_err("wind-log pogo_en val = %d\n",val);
	}
	return 0;
}
//zhangchao@wind-mobi.com 20180305 end

static int bq_fg_probe(struct i2c_client *client, 
							const struct i2c_device_id *id)
{

	int ret;
	struct bq_fg_chip *bq;
	u8 *regs;

	bq = devm_kzalloc(&client->dev, sizeof(*bq), GFP_KERNEL);

	if (!bq) {
		pr_err("Could not allocate memory\n");
		return -ENOMEM;
	}

	bq->dev = &client->dev;
	bq->client = client;
	bq->chip = id->driver_data;

	bq->batt_soc	= -ENODATA;
	bq->batt_fcc	= -ENODATA;
	bq->batt_rm		= -ENODATA;
	bq->batt_dc		= -ENODATA;
	bq->batt_volt	= -ENODATA;
	bq->batt_temp	= -ENODATA;
	bq->batt_curr	= -ENODATA;
	bq->batt_cyclecnt = -ENODATA;

	bq->fake_soc 	= -EINVAL;
	bq->fake_temp	= -EINVAL;

	if (bq->chip == BQ27426) {
		regs = bq27426_regs; 
	} else {
		pr_err("unexpected fuel gauge: %d\n", bq->chip);
		regs = bq27426_regs;
	}
	
	memcpy(bq->regs, regs, NUM_REGS);

	i2c_set_clientdata(client, bq);

	mutex_init(&bq->i2c_rw_lock);
	mutex_init(&bq->data_lock);
	mutex_init(&bq->update_lock);
	mutex_init(&bq->irq_complete);

	bq->resume_completed = true;
	bq->irq_waiting = false;
	
	ret = bq_parse_dt(bq);
	if (ret < 0) {
		dev_err(&client->dev, "Unable to parse DT nodes\n");
	}
	INIT_WORK(&bq->update_work, fg_update_bqfs_workfunc);


	if (client->irq) {
		ret = devm_request_threaded_irq(&client->dev, client->irq, NULL,
			fg_irq_thread, 
			IRQF_TRIGGER_FALLING | IRQF_ONESHOT,
			"bq fuel gauge irq", bq);
		if (ret < 0) {
			pr_err("request irq for irq=%d failed, ret = %d\n", client->irq, ret);
			goto err_1;
		}
		enable_irq_wake(client->irq);
	}
	
	device_init_wakeup(bq->dev, 1);

	bq->fw_ver = fg_read_fw_version(bq);

	fg_psy_register(bq);

	create_debugfs_entry(bq);
	ret = sysfs_create_group(&bq->dev->kobj, &fg_attr_group);
	if (ret) {
		pr_err("Failed to register sysfs, err:%d\n", ret);
	}

	determine_initial_status(bq);
	
	otg_flag_enable(bq,0);  //zhangchao@wind-mobi.com 20180305 modify
	pogo_enable(bq,0);      //zhangchao@wind-mobi.com 20180312 modify

	pr_err("bq fuel gauge probe successfully, %s FW ver:%d\n", 
			device2str[bq->chip], bq->fw_ver);

	return 0;

err_1:
	fg_psy_unregister(bq);
	return ret;
}


static inline bool is_device_suspended(struct bq_fg_chip *bq)
{
	return !bq->resume_completed;
}


static int bq_fg_suspend(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct bq_fg_chip *bq = i2c_get_clientdata(client);

	mutex_lock(&bq->irq_complete);
	bq->resume_completed = false;
	mutex_unlock(&bq->irq_complete);

	return 0;
}

static int bq_fg_suspend_noirq(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct bq_fg_chip *bq = i2c_get_clientdata(client);

	if (bq->irq_waiting) {
		pr_err_ratelimited("Aborting suspend, an interrupt was detected while suspending\n");
		return -EBUSY;
	}
	return 0;

}


static int bq_fg_resume(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct bq_fg_chip *bq = i2c_get_clientdata(client);

	mutex_lock(&bq->irq_complete);
	bq->resume_completed = true;
	if (bq->irq_waiting) {
		bq->irq_disabled = false;
		enable_irq(client->irq);
		mutex_unlock(&bq->irq_complete);
		fg_irq_thread(client->irq, bq);
	} else {
		mutex_unlock(&bq->irq_complete);
	}

	power_supply_changed(&bq->fg_psy);

	return 0;


}

static int bq_fg_remove(struct i2c_client *client)
{
	struct bq_fg_chip *bq = i2c_get_clientdata(client);

	fg_psy_unregister(bq);

	mutex_destroy(&bq->data_lock);
	mutex_destroy(&bq->i2c_rw_lock);
	mutex_destroy(&bq->update_lock);
	mutex_destroy(&bq->irq_complete);

	debugfs_remove_recursive(bq->debug_root);
	sysfs_remove_group(&bq->dev->kobj, &fg_attr_group);

	return 0;

}

static void bq_fg_shutdown(struct i2c_client *client)
{
	pr_info("bq fuel gauge driver shutdown!\n");
}

static struct of_device_id bq_fg_match_table[] = {
	{.compatible = "ti,bq27426",},
	{},
};
MODULE_DEVICE_TABLE(of,bq_fg_match_table);

static const struct i2c_device_id bq_fg_id[] = {
	{ "bq27426", BQ27426 },
	{},
};
MODULE_DEVICE_TABLE(i2c, bq_fg_id);

static const struct dev_pm_ops bq_fg_pm_ops = {
	.resume		= bq_fg_resume,
	.suspend_noirq = bq_fg_suspend_noirq,
	.suspend	= bq_fg_suspend,
};

static struct i2c_driver bq_fg_driver = {
	.driver 	= {
		.name 	= "bq_fg",
		.owner 	= THIS_MODULE,
		.of_match_table = bq_fg_match_table,
		.pm		= &bq_fg_pm_ops,
	},
	.id_table	= bq_fg_id,
	
	.probe		= bq_fg_probe,
	.remove		= bq_fg_remove,
	.shutdown	= bq_fg_shutdown,
	
};

module_i2c_driver(bq_fg_driver);

MODULE_DESCRIPTION("TI BQ2742x Gauge Driver");
MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Texas Instruments");
//zhangchao@wind-mobi.com 20180226 end
