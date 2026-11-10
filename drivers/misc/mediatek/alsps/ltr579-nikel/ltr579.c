/*
 * Lite-On LTR-579ALS-01 ambient light + proximity sensor driver.
 *
 * Ported for the Xiaomi Redmi Note 4 (nikel, MT6797) from the register
 * sequences used by the official MIUI kernel driver:
 *   ltr579_probe / ltr579_ps_enable / ltr579_als_enable /
 *   ltr579_ps_set_thres / ltr579_ps_read / ltr579_als_read
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation.
 */

#include <linux/module.h>
#include <linux/i2c.h>
#include <linux/of.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/jiffies.h>
#include <linux/miscdevice.h>
#include <linux/uaccess.h>

#include "cust_alsps.h"
#include "alsps.h"
#include "ltr579.h"

/******************************************************************************
 * configuration
 ******************************************************************************/
#define LTR579_DEV_NAME		"ltr579"
#define LTR579_TAG		"[ALS/PS] "
#define LTR579_FUN(f)		pr_debug(LTR579_TAG "%s\n", __func__)
#define LTR579_ERR(fmt, ...)	pr_err(LTR579_TAG "%s %d: " fmt, __func__, __LINE__, ##__VA_ARGS__)
#define LTR579_LOG(fmt, ...)	pr_debug(LTR579_TAG fmt, ##__VA_ARGS__)

enum {
	CMC_BIT_ALS = 1,
	CMC_BIT_PS = 2,
};

struct ltr579_priv {
	struct alsps_hw *hw;
	struct i2c_client *client;

	atomic_t als_debounce;
	atomic_t als_deb_on;
	atomic_t als_deb_end;
	atomic_t ps_debounce;
	atomic_t ps_deb_on;
	atomic_t ps_deb_end;
	atomic_t ps_mask;
	atomic_t als_suspend;
	atomic_t ps_suspend;
	atomic_t trace;
	atomic_t init_done;

	u16 als;
	u16 ps;
	u8 _align;
	u16 als_level_num;
	u16 als_value_num;
	u32 als_level[C_CUST_ALS_LEVEL - 1];
	u32 als_value[C_CUST_ALS_LEVEL];
	int ps_cali;

	atomic_t ps_thd_val_high;
	atomic_t ps_thd_val_low;
	atomic_t als_thd_val_high;
	atomic_t als_thd_val_low;
	ulong enable;
};

static struct alsps_hw alsps_cust;
static struct alsps_hw *hw = &alsps_cust;
static struct ltr579_priv *ltr579_obj;
static struct i2c_client *ltr579_i2c_client;
static DEFINE_MUTEX(ltr579_mutex);
static int ltr579_init_flag = -1;

struct alsps_hw *get_cust_alsps(void)
{
	return &alsps_cust;
}

/******************************************************************************
 * i2c access
 ******************************************************************************/
static int ltr579_i2c_write_reg(struct i2c_client *client, u8 reg, u8 val)
{
	u8 buf[2];
	int ret;

	buf[0] = reg;
	buf[1] = val;

	mutex_lock(&ltr579_mutex);
	ret = i2c_master_send(client, buf, 2);
	mutex_unlock(&ltr579_mutex);

	if (ret < 0) {
		LTR579_ERR("write reg 0x%02x=0x%02x failed: %d\n", reg, val, ret);
		return ret;
	}

	return 0;
}

static int ltr579_i2c_read_reg(struct i2c_client *client, u8 reg)
{
	struct i2c_msg msg[2];
	u8 buf = 0;
	int ret;

	msg[0].addr = client->addr;
	msg[0].flags = 0;
	msg[0].len = 1;
	msg[0].buf = &reg;

	msg[1].addr = client->addr;
	msg[1].flags = I2C_M_RD;
	msg[1].len = 1;
	msg[1].buf = &buf;

	mutex_lock(&ltr579_mutex);
	ret = i2c_transfer(client->adapter, msg, 2);
	mutex_unlock(&ltr579_mutex);

	if (ret != 2) {
		LTR579_ERR("read reg 0x%02x failed: %d\n", reg, ret);
		return (ret < 0) ? ret : -EIO;
	}

	return buf;
}

static void ltr579_power(struct alsps_hw *hw, unsigned int on)
{
	/* regulator-less on nikel: VDD/VIO are always on, power_id is -1 */
}

/******************************************************************************
 * hardware control
 ******************************************************************************/
static int ltr579_ps_set_thres(struct i2c_client *client)
{
	struct ltr579_priv *obj = i2c_get_clientdata(client);
	int high = atomic_read(&obj->ps_thd_val_high);
	int low = atomic_read(&obj->ps_thd_val_low);
	int ret;

	ret = ltr579_i2c_write_reg(client, LTR579_REG_PS_THRES_HIGH_L, high & 0xFF);
	if (ret < 0)
		return ret;
	ret = ltr579_i2c_write_reg(client, LTR579_REG_PS_THRES_HIGH_H, (high >> 8) & 0xFF);
	if (ret < 0)
		return ret;
	ret = ltr579_i2c_write_reg(client, LTR579_REG_PS_THRES_LOW_L, low & 0xFF);
	if (ret < 0)
		return ret;
	ret = ltr579_i2c_write_reg(client, LTR579_REG_PS_THRES_LOW_H, (low >> 8) & 0xFF);

	return ret;
}

static int ltr579_ps_enable(struct i2c_client *client, int enable)
{
	struct ltr579_priv *obj = i2c_get_clientdata(client);
	int val;
	int ret;

	if (enable) {
		ret = ltr579_i2c_write_reg(client, LTR579_REG_PS_RATE, 0x0B);
		if (ret < 0)
			return ret;
		ret = ltr579_i2c_write_reg(client, LTR579_REG_PS_LED, 0x36);
		if (ret < 0)
			return ret;
		ret = ltr579_i2c_write_reg(client, LTR579_REG_PS_CONF, 0x5C);
		if (ret < 0)
			return ret;
		ret = ltr579_ps_set_thres(client);
		if (ret < 0)
			return ret;
		ret = ltr579_i2c_write_reg(client, LTR579_REG_PS_OFFSET_L, 0x01);
		if (ret < 0)
			return ret;
		ret = ltr579_i2c_write_reg(client, LTR579_REG_PS_OFFSET_H, 0x02);
		if (ret < 0)
			return ret;

		msleep(100);

		val = ltr579_i2c_read_reg(client, LTR579_REG_CONTR);
		if (val < 0)
			return val;
		ret = ltr579_i2c_write_reg(client, LTR579_REG_CONTR, val | 0x01);
		if (ret < 0)
			return ret;

		atomic_set(&obj->ps_deb_on, 1);
		atomic_set(&obj->ps_deb_end,
			   jiffies + atomic_read(&obj->ps_debounce) / (1000 / HZ));
	} else {
		val = ltr579_i2c_read_reg(client, LTR579_REG_CONTR);
		if (val < 0)
			return val;
		ret = ltr579_i2c_write_reg(client, LTR579_REG_CONTR, val & 0xFE);
		if (ret < 0)
			return ret;
		atomic_set(&obj->ps_deb_on, 0);
	}

	return 0;
}

static int ltr579_als_enable(struct i2c_client *client, int enable)
{
	struct ltr579_priv *obj = i2c_get_clientdata(client);
	int val;
	int ret;

	if (enable) {
		ret = ltr579_i2c_write_reg(client, LTR579_REG_ALS_RATE, 0x01);
		if (ret < 0)
			return ret;
		ret = ltr579_i2c_write_reg(client, LTR579_REG_ALS_CONF, 0x22);
		if (ret < 0)
			return ret;

		val = ltr579_i2c_read_reg(client, LTR579_REG_CONTR);
		if (val < 0)
			return val;
		ret = ltr579_i2c_write_reg(client, LTR579_REG_CONTR, val | 0x02);
		if (ret < 0)
			return ret;

		atomic_set(&obj->als_deb_on, 1);
		atomic_set(&obj->als_deb_end,
			   jiffies + atomic_read(&obj->als_debounce) / (1000 / HZ));
	} else {
		val = ltr579_i2c_read_reg(client, LTR579_REG_CONTR);
		if (val < 0)
			return val;
		ret = ltr579_i2c_write_reg(client, LTR579_REG_CONTR, val & 0xFD);
		if (ret < 0)
			return ret;
		atomic_set(&obj->als_deb_on, 0);
	}

	return 0;
}

static int ltr579_ps_read(struct i2c_client *client, u16 *ps)
{
	int lo, hi;

	lo = ltr579_i2c_read_reg(client, LTR579_REG_PS_DATA_L);
	hi = ltr579_i2c_read_reg(client, LTR579_REG_PS_DATA_H);
	if (lo < 0 || hi < 0)
		return LTR579_ERR_I2C;

	*ps = (lo & 0xFF) | ((hi & 0x07) << 8);
	return 0;
}

/*
 * ALS data consists of a 24-bit count (0x0d..0x0f) plus a 24-bit IR ratio
 * (0x0a..0x0c).  The ratio selects one of three correction coefficients and
 * the gain / integration settings scale the result, mirroring the official
 * ltr579_als_read() implementation.
 */
static int ltr579_als_read(struct i2c_client *client, u32 *lux)
{
	int r0, r1, r2, d0, d1, d2, g, i;
	int ratio, data, ratio10, coef, gain_div, integ_div;

	r0 = ltr579_i2c_read_reg(client, LTR579_REG_ALS_RATIO_L);
	r1 = ltr579_i2c_read_reg(client, LTR579_REG_ALS_RATIO_M);
	r2 = ltr579_i2c_read_reg(client, LTR579_REG_ALS_RATIO_H);
	d0 = ltr579_i2c_read_reg(client, LTR579_REG_ALS_DATA_L);
	d1 = ltr579_i2c_read_reg(client, LTR579_REG_ALS_DATA_M);
	d2 = ltr579_i2c_read_reg(client, LTR579_REG_ALS_DATA_H);
	g = ltr579_i2c_read_reg(client, LTR579_REG_ALS_CONF);
	i = ltr579_i2c_read_reg(client, LTR579_REG_ALS_RATE);
	if (r0 < 0 || r1 < 0 || r2 < 0 || d0 < 0 || d1 < 0 || d2 < 0 ||
	    g < 0 || i < 0)
		return LTR579_ERR_I2C;

	ratio = (r0 & 0xFF) | ((r1 & 0xFF) << 8) | ((r2 & 0xFF) << 16);
	data = (d0 & 0xFF) | ((d1 & 0xFF) << 8) | ((d2 & 0xFF) << 16);

	if (data == 0) {
		*lux = 0;
		return 0;
	}

	switch ((g >> 4) & 0x07) {
	case 0:
		gain_div = 400;
		break;
	case 1:
		gain_div = 200;
		break;
	case 2:
		gain_div = 100;
		break;
	case 3:
		gain_div = 50;
		break;
	default:
		gain_div = 25;
		break;
	}

	switch (i & 0x07) {
	case 0:
		integ_div = 1;
		break;
	case 1:
		integ_div = 3;
		break;
	case 2:
		integ_div = 6;
		break;
	case 3:
		integ_div = 9;
		break;
	default:
		integ_div = 18;
		break;
	}

	ratio10 = (ratio / data) * 10;
	if (ratio10 > 85)
		coef = 606;
	else if (ratio10 > 40)
		coef = 754;
	else
		coef = 998;

	data = (int)(((s64)data * coef) / (gain_div * integ_div));
	if (data < 0)
		data = 0;
	if (data > 0xFFFF)
		data = 0xFFFF;

	*lux = data;
	return 0;
}

/******************************************************************************
 * value conversion
 ******************************************************************************/
static int ltr579_get_ps_value(struct ltr579_priv *obj)
{
	int val, status;
	int invalid = 0;

	status = ltr579_i2c_read_reg(obj->client, LTR579_REG_PS_STATUS);
	if (status < 0)
		return -1;

	/* near (0) when the proximity threshold status bit is asserted */
	val = ((status >> 2) & 0x01) ? 0 : 1;

	if (atomic_read(&obj->ps_suspend)) {
		invalid = 1;
	} else if (atomic_read(&obj->ps_deb_on)) {
		unsigned long endt = atomic_read(&obj->ps_deb_end);

		if (time_after(jiffies, endt))
			atomic_set(&obj->ps_deb_on, 0);

		if (atomic_read(&obj->ps_deb_on))
			invalid = 1;
	}

	if (invalid)
		return -1;

	if (!test_bit(CMC_BIT_PS, &obj->enable))
		return -1;

	return val;
}

static int ltr579_get_als_value(struct ltr579_priv *obj, u16 als)
{
	int idx;
	int invalid = 0;
	int level_high, level_low, level_diff;
	int value_high, value_low, value_diff;
	int value = 0;

	if (0 == obj->als_level_num || 0 == obj->als_value_num) {
		LTR579_ERR("invalid als_level_num=%d als_value_num=%d\n",
			   obj->als_level_num, obj->als_value_num);
		return -1;
	}

	if (atomic_read(&obj->als_deb_on)) {
		unsigned long endt = atomic_read(&obj->als_deb_end);

		if (time_after(jiffies, endt))
			atomic_set(&obj->als_deb_on, 0);

		if (atomic_read(&obj->als_deb_on))
			invalid = 1;
	}

	for (idx = 0; idx < obj->als_level_num; idx++) {
		if (als < obj->hw->als_level[idx])
			break;
	}

	if (idx >= obj->als_level_num || idx >= obj->als_value_num) {
		if (idx < obj->als_value_num)
			value = obj->hw->als_value[idx - 1];
		else
			value = obj->hw->als_value[obj->als_value_num - 1];
	} else {
		level_high = obj->hw->als_level[idx];
		level_low = (idx > 0) ? obj->hw->als_level[idx - 1] : 0;
		level_diff = level_high - level_low;
		value_high = obj->hw->als_value[idx];
		value_low = (idx > 0) ? obj->hw->als_value[idx - 1] : 0;
		value_diff = value_high - value_low;

		if (level_low >= level_high || value_low >= value_high)
			value = value_low;
		else
			value = (level_diff * value_low +
				 (als - level_low) * value_diff +
				 ((level_diff + 1) >> 1)) / level_diff;
	}

	if (invalid)
		return -1;

	return value;
}

/******************************************************************************
 * sensor framework callbacks
 ******************************************************************************/
static int als_open_report_data(int open)
{
	return 0;
}

static int als_enable_nodata(int en)
{
	int res;

	if (!ltr579_obj)
		return -1;

	mutex_lock(&ltr579_mutex);
	if (en)
		set_bit(CMC_BIT_ALS, &ltr579_obj->enable);
	else
		clear_bit(CMC_BIT_ALS, &ltr579_obj->enable);
	mutex_unlock(&ltr579_mutex);

	res = ltr579_als_enable(ltr579_obj->client, en);
	if (res < 0)
		return -1;

	return 0;
}

static int als_set_delay(u64 ns)
{
	return 0;
}

static int als_get_data(int *value, int *status)
{
	struct ltr579_priv *obj = ltr579_obj;
	u32 lux;
	int err;

	if (!obj)
		return -1;

	err = ltr579_als_read(obj->client, &lux);
	if (err) {
		*value = 0;
		*status = SENSOR_STATUS_ACCURACY_LOW;
		return -1;
	}

	obj->als = (u16)lux;
	*value = ltr579_get_als_value(obj, obj->als);
	*status = SENSOR_STATUS_ACCURACY_MEDIUM;

	return 0;
}

static int ps_open_report_data(int open)
{
	return 0;
}

static int ps_enable_nodata(int en)
{
	int res;

	if (!ltr579_obj)
		return -1;

	mutex_lock(&ltr579_mutex);
	if (en)
		set_bit(CMC_BIT_PS, &ltr579_obj->enable);
	else
		clear_bit(CMC_BIT_PS, &ltr579_obj->enable);
	mutex_unlock(&ltr579_mutex);

	res = ltr579_ps_enable(ltr579_obj->client, en);
	if (res < 0)
		return -1;

	return 0;
}

static int ps_set_delay(u64 ns)
{
	return 0;
}

static int ps_get_data(int *value, int *status)
{
	struct ltr579_priv *obj = ltr579_obj;
	int err;

	if (!obj)
		return -1;

	err = ltr579_ps_read(obj->client, &obj->ps);
	if (err) {
		*value = 1;
		*status = SENSOR_STATUS_ACCURACY_LOW;
		return -1;
	}

	*value = ltr579_get_ps_value(obj);
	*status = SENSOR_STATUS_ACCURACY_MEDIUM;

	return 0;
}

/******************************************************************************
 * i2c driver
 ******************************************************************************/
static int ltr579_init_client(struct i2c_client *client)
{
	struct ltr579_priv *obj = i2c_get_clientdata(client);
	int id;
	int ret;

	id = ltr579_i2c_read_reg(client, LTR579_REG_PART_ID);
	if (id < 0) {
		LTR579_ERR("read part id failed: %d\n", id);
		return id;
	}
	LTR579_LOG("LTR579 part id = 0x%02x\n", id);

	if (id != LTR579_PART_ID_VALUE) {
		LTR579_ERR("unexpected part id 0x%02x (want 0x%02x)\n",
			   id, LTR579_PART_ID_VALUE);
		return -ENODEV;
	}

	ret = ltr579_ps_enable(client, 1);
	if (ret < 0) {
		LTR579_ERR("ps enable failed: %d\n", ret);
		return ret;
	}

	ret = ltr579_als_enable(client, 1);
	if (ret < 0) {
		LTR579_ERR("als enable failed: %d\n", ret);
		return ret;
	}

	atomic_set(&obj->init_done, 1);
	LTR579_LOG("LTR579 init done\n");

	return 0;
}

static int ltr579_i2c_probe(struct i2c_client *client,
			    const struct i2c_device_id *id)
{
	struct ltr579_priv *obj;
	struct als_control_path als_ctl = { 0 };
	struct als_data_path als_data = { 0 };
	struct ps_control_path ps_ctl = { 0 };
	struct ps_data_path ps_data = { 0 };
	int err;

	LTR579_FUN();

	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C)) {
		LTR579_ERR("i2c functionality check failed\n");
		return -ENODEV;
	}

	obj = kzalloc(sizeof(*obj), GFP_KERNEL);
	if (!obj)
		return -ENOMEM;

	obj->hw = hw;
	obj->client = client;
	i2c_set_clientdata(client, obj);
	ltr579_obj = obj;

	atomic_set(&obj->als_debounce, 200);
	atomic_set(&obj->als_deb_on, 0);
	atomic_set(&obj->als_deb_end, 0);
	atomic_set(&obj->ps_debounce, 200);
	atomic_set(&obj->ps_deb_on, 0);
	atomic_set(&obj->ps_deb_end, 0);
	atomic_set(&obj->ps_mask, 0);
	atomic_set(&obj->als_suspend, 0);
	atomic_set(&obj->ps_suspend, 0);
	atomic_set(&obj->ps_thd_val_high, obj->hw->ps_threshold_high);
	atomic_set(&obj->ps_thd_val_low, obj->hw->ps_threshold_low);
	atomic_set(&obj->als_thd_val_high, obj->hw->als_threshold_high);
	atomic_set(&obj->als_thd_val_low, obj->hw->als_threshold_low);
	atomic_set(&obj->init_done, 0);

	obj->enable = 0;
	obj->ps_cali = 0;
	obj->als_level_num = ARRAY_SIZE(obj->als_level);
	obj->als_value_num = ARRAY_SIZE(obj->als_value);
	memcpy(obj->als_level, obj->hw->als_level, sizeof(obj->als_level));
	memcpy(obj->als_value, obj->hw->als_value, sizeof(obj->als_value));

	clear_bit(CMC_BIT_ALS, &obj->enable);
	clear_bit(CMC_BIT_PS, &obj->enable);

	ltr579_i2c_client = client;

	err = ltr579_init_client(client);
	if (err)
		goto exit_free;

	als_ctl.is_use_common_factory = false;
	als_ctl.open_report_data = als_open_report_data;
	als_ctl.enable_nodata = als_enable_nodata;
	als_ctl.set_delay = als_set_delay;
	als_ctl.is_report_input_direct = false;
	als_ctl.is_support_batch = false;
	err = als_register_control_path(&als_ctl);
	if (err)
		goto exit_free;

	als_data.get_data = als_get_data;
	als_data.vender_div = 100;
	err = als_register_data_path(&als_data);
	if (err)
		goto exit_free;

	ps_ctl.is_use_common_factory = false;
	ps_ctl.open_report_data = ps_open_report_data;
	ps_ctl.enable_nodata = ps_enable_nodata;
	ps_ctl.set_delay = ps_set_delay;
	ps_ctl.is_report_input_direct = false;
	ps_ctl.is_support_batch = false;
	err = ps_register_control_path(&ps_ctl);
	if (err)
		goto exit_free;

	ps_data.get_data = ps_get_data;
	ps_data.vender_div = 100;
	err = ps_register_data_path(&ps_data);
	if (err)
		goto exit_free;

	batch_register_support_info(ID_LIGHT, als_ctl.is_support_batch, 100, 0);
	batch_register_support_info(ID_PROXIMITY, ps_ctl.is_support_batch, 100, 0);

	ltr579_init_flag = 0;
	LTR579_LOG("%s: OK\n", __func__);

	return 0;

exit_free:
	ltr579_obj = NULL;
	ltr579_i2c_client = NULL;
	kfree(obj);
	ltr579_init_flag = -1;
	return err;
}

static int ltr579_i2c_remove(struct i2c_client *client)
{
	ltr579_obj = NULL;
	ltr579_i2c_client = NULL;
	kfree(i2c_get_clientdata(client));
	return 0;
}

static int ltr579_i2c_detect(struct i2c_client *client,
			     struct i2c_board_info *info)
{
	strlcpy(info->type, LTR579_DEV_NAME, sizeof(info->type));
	return 0;
}

static int ltr579_i2c_suspend(struct i2c_client *client, pm_message_t msg)
{
	struct ltr579_priv *obj = i2c_get_clientdata(client);

	if (!obj)
		return 0;

	atomic_set(&obj->als_suspend, 1);
	if (test_bit(CMC_BIT_ALS, &obj->enable))
		ltr579_als_enable(client, 0);

	return 0;
}

static int ltr579_i2c_resume(struct i2c_client *client)
{
	struct ltr579_priv *obj = i2c_get_clientdata(client);

	if (!obj)
		return 0;

	atomic_set(&obj->als_suspend, 0);
	if (test_bit(CMC_BIT_ALS, &obj->enable))
		ltr579_als_enable(client, 1);

	return 0;
}

static const struct i2c_device_id ltr579_i2c_id[] = {
	{ LTR579_DEV_NAME, 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, ltr579_i2c_id);

static const struct of_device_id ltr579_of_match[] = {
	{ .compatible = "mediatek,alsps" },
	{ }
};
MODULE_DEVICE_TABLE(of, ltr579_of_match);

static struct i2c_driver ltr579_i2c_driver = {
	.probe = ltr579_i2c_probe,
	.remove = ltr579_i2c_remove,
	.detect = ltr579_i2c_detect,
	.suspend = ltr579_i2c_suspend,
	.resume = ltr579_i2c_resume,
	.id_table = ltr579_i2c_id,
	.driver = {
		.name = LTR579_DEV_NAME,
#ifdef CONFIG_OF
		.of_match_table = ltr579_of_match,
#endif
	},
};

/******************************************************************************
 * module init
 ******************************************************************************/
static int ltr579_local_init(void)
{
	ltr579_power(hw, 1);

	if (i2c_add_driver(&ltr579_i2c_driver)) {
		LTR579_ERR("add i2c driver failed\n");
		return -1;
	}

	if (ltr579_init_flag == -1)
		return -1;

	return 0;
}

static int ltr579_remove(void)
{
	ltr579_power(hw, 0);
	i2c_del_driver(&ltr579_i2c_driver);
	return 0;
}

static struct alsps_init_info ltr579_init_info = {
	.name = "LTR579",
	.init = ltr579_local_init,
	.uninit = ltr579_remove,
};

static int __init ltr579_init(void)
{
	hw = get_alsps_dts_func("mediatek,ltr579-new", hw);
	if (!hw) {
		LTR579_ERR("get dts info failed\n");
		return -1;
	}

	alsps_driver_add(&ltr579_init_info);
	return 0;
}

static void __exit ltr579_exit(void)
{
	LTR579_FUN();
}

module_init(ltr579_init);
module_exit(ltr579_exit);

MODULE_AUTHOR("nikel kernel port");
MODULE_DESCRIPTION("LTR-579ALS-01 ALS/PS driver");
MODULE_LICENSE("GPL");
