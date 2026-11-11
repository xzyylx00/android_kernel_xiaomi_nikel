/*
 * yas537.c - Yamaha YAS537 magnetometer driver
 *
 * Minimal driver for the Yamaha YAS537 3-axis magnetometer found on
 * the Redmi Note 4 (nikel, MT6797).  It registers with the MediaTek
 * magnetometer framework (mag.c) and exposes magnetic field data in
 * nano tesla through the usual m_mag_misc/input interface.
 *
 * The register protocol and conversion constants were reconstructed
 * from the vendor kernel shipped with the device.
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation.
 */

#include <linux/module.h>
#include <linux/i2c.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/mutex.h>
#include <linux/atomic.h>
#include <linux/proc_fs.h>
#include <linux/uaccess.h>

#include <cust_mag.h>
#include "mag.h"

#define YAS537_DEV_NAME		"yas537"

#define YAS537_REG_WHOAMI	0x80
#define YAS537_REG_CTRL1	0x81
#define YAS537_REG_CONFIG	0x83
#define YAS537_REG_MEASURE	0xB0
#define YAS537_REG_CAL		0x90
#define YAS537_REG_TRIM		0xC0

#define YAS537_DEVICE_ID	0x07
#define YAS537_DATA_CENTER	8192
#define YAS537_DATA_OVERFLOW	16383

#define YAS537_DEFAULT_DELAY	50

/* Transformation matrices indexed by the board direction (DT "direction"). */
static const s8 yas537_transformation[][9] = {
	{ -1,  0,  0,  0, -1,  0,  0,  0,  1 },
	{  0, -1,  0,  1,  0,  0,  0,  0,  1 },
	{  1,  0,  0,  0,  1,  0,  0,  0,  1 },
	{  0,  1,  0, -1,  0,  0,  0,  0,  1 },
	{  1,  0,  0,  0, -1,  0,  0,  0, -1 },
	{  0,  1,  0,  1,  0,  0,  0,  0, -1 },
	{ -1,  0,  0,  0,  1,  0,  0,  0, -1 },
	{  0, -1,  0, -1,  0,  0,  0,  0, -1 },
};

/*
 * Fixed sensitivity matrix used by the vendor driver.  Values are scaled
 * by 10000 (i.e. 10045 == 1.0045).
 */
static const s16 yas537_static_matrix[9] = {
	10045,  -22,   262,
	  360, 9636,  -136,
	  -93, -336, 10335,
};

static struct i2c_client *yas537_client;
static struct mag_hw *yas537_hw;
static struct mag_hw yas537_mag_cust;
static DEFINE_MUTEX(yas537_mutex);
static atomic_t yas537_enable = ATOMIC_INIT(0);
static int yas537_delay_ms = YAS537_DEFAULT_DELAY;
static int yas537_init_flag = -1;

/* Debug information for board bring-up. */
static u8 yas537_dbg_raw[8];
static u16 yas537_dbg_xy1y2[3];
static int yas537_dbg_xyz[3];
static int yas537_dbg_temp;
static u8 yas537_rcoil_flag;

static void yas537_sensitivity_correction(u16 *v, u16 temp);

static int yas537_i2c_write(u8 reg, u8 val)
{
	struct i2c_msg msg;
	u8 buf[2];
	int ret;

	if (!yas537_client)
		return -ENODEV;

	buf[0] = reg;
	buf[1] = val;
	msg.addr = yas537_client->addr;
	msg.flags = 0;
	msg.len = 2;
	msg.buf = buf;

	ret = i2c_transfer(yas537_client->adapter, &msg, 1);
	return ret == 1 ? 0 : (ret < 0 ? ret : -EIO);
}

static int yas537_i2c_read(u8 reg, u8 *buf, int len)
{
	struct i2c_msg msgs[2];
	int ret;

	if (!yas537_client)
		return -ENODEV;

	msgs[0].addr = yas537_client->addr;
	msgs[0].flags = 0;
	msgs[0].len = 1;
	msgs[0].buf = &reg;
	msgs[1].addr = yas537_client->addr;
	msgs[1].flags = I2C_M_RD;
	msgs[1].len = len;
	msgs[1].buf = buf;

	ret = i2c_transfer(yas537_client->adapter, msgs, 2);
	return ret == 2 ? 0 : (ret < 0 ? ret : -EIO);
}

static void yas537_parse_sample(const u8 *buf, u16 *v, u16 *temp)
{
	*temp = ((u16)buf[0] << 8) | buf[1];
	v[0] = buf[3] | (((u16)buf[2] & 0x3F) << 8);
	v[1] = buf[5] | ((u16)buf[4] << 8);
	v[2] = buf[7] | ((u16)buf[6] << 8);
}

static int yas537_single_sample(u16 *v, u16 *temp)
{
	u8 buf[8];
	int ret;

	ret = yas537_i2c_write(YAS537_REG_CTRL1, 0x01);
	if (ret < 0)
		return ret;
	usleep_range(1500, 2500);
	ret = yas537_i2c_read(YAS537_REG_MEASURE, buf, sizeof(buf));
	if (ret < 0)
		return ret;
	yas537_parse_sample(buf, v, temp);
	yas537_sensitivity_correction(v, *temp);
	return 0;
}

/*
 * The vendor driver cancels the sensor's hard-iron offset by running the
 * chip's RCOIL sequence: 0xa0 selects the offset-cancel direction and
 * 0x82=8 triggers a measurement.  The flag bit from the OTP trim data
 * decides which polarity is used.
 */
static int yas537_rcoil(int mode, int count)
{
	u8 val;
	int i;
	int ret;

	if (yas537_rcoil_flag == 1)
		val = mode ? 0x02 : 0x00;
	else
		val = mode ? 0x00 : 0x02;

	ret = yas537_i2c_write(0xA0, val);
	if (ret < 0)
		return ret;

	for (i = 0; i < count; i++) {
		ret = yas537_i2c_write(0x82, 0x08);
		if (ret < 0)
			return ret;
		usleep_range(65, 200);
	}
	return 0;
}

static int yas537_enable_rcoil(void)
{
	u16 base[3], rec[3];
	u16 temp;
	int ret, i, pass, diff;

	ret = yas537_single_sample(base, &temp);
	if (ret < 0)
		return ret;

	for (pass = 0; pass < 2; pass++) {
		ret = yas537_rcoil(0, 1);
		if (ret < 0)
			return ret;
		ret = yas537_rcoil(1, 5);
		if (ret < 0)
			return ret;
		ret = yas537_single_sample(rec, &temp);
		if (ret < 0)
			return ret;

		for (i = 0; i < 3; i++) {
			diff = (int)base[i] - (int)rec[i];
			if (diff < 0)
				diff = -diff;
			if (diff > 1000)
				break;
		}
		if (i == 3)
			break;
	}

	pr_info("yas537: rcoil flag %u base %u %u %u rec %u %u %u pass %d\n",
		yas537_rcoil_flag, base[0], base[1], base[2],
		rec[0], rec[1], rec[2], pass);
	return 0;
}

static void yas537_sensitivity_correction(u16 *v, u16 temp)
{
	int i;
	int denom;

	if (temp == 8176)
		return;

	denom = 11 * (8176 - (int)temp) + 100000;
	for (i = 0; i < 3; i++) {
		int q = (((int)v[i] - YAS537_DATA_CENTER) * 100000) / denom;

		if (q > 8191)
			q = 8191;
		if (q < -8192)
			q = -8192;
		v[i] = (u16)(q + YAS537_DATA_CENTER);
	}
}

/*
 * Reset the chip and load its OTP trim values.  This mirrors the vendor
 * sequence: reset, read 17 trim bytes from 0xC0 and program the
 * compensation registers.
 */
static int yas537_chip_init(void)
{
	u8 id = 0;
	u8 trim[17];
	int i;
	int ret;
	int nonzero = 0;

	mutex_lock(&yas537_mutex);

	ret = yas537_i2c_read(YAS537_REG_WHOAMI, &id, 1);
	if (ret < 0) {
		pr_err("yas537: failed to read chip id (%d)\n", ret);
		goto out;
	}
	if (id != YAS537_DEVICE_ID)
		pr_warn("yas537: unexpected chip id 0x%02x (expected 0x%02x)\n",
			id, YAS537_DEVICE_ID);

	ret = yas537_i2c_write(YAS537_REG_CAL, 0x02);
	if (ret < 0) {
		pr_err("yas537: reset failed (%d)\n", ret);
		goto out;
	}
	msleep(5);

	ret = yas537_i2c_read(YAS537_REG_TRIM, trim, sizeof(trim));
	if (ret < 0) {
		pr_err("yas537: failed to read trim data (%d)\n", ret);
		goto out;
	}

	for (i = 0; i < sizeof(trim); i++)
		nonzero += trim[i] ? 1 : 0;

	pr_info("yas537: chip id 0x%02x, trim %d/%d non-zero\n",
		id, nonzero, (int)sizeof(trim));

	yas537_rcoil_flag = trim[16] >> 6;

	if (nonzero == 0) {
		pr_err("yas537: OTP trim is empty, refusing to start\n");
		ret = -ENODEV;
		goto out;
	}

	for (i = 0; i < 3; i++) {
		ret = yas537_i2c_write(0x93 + i, trim[i]);
		if (ret < 0)
			goto out;
		ret = yas537_i2c_write(0x84 + i, trim[12 + i]);
		if (ret < 0)
			goto out;
	}

	ret = yas537_i2c_write(0x96, (trim[3] & 0xE0) | 0x10);
	if (ret < 0)
		goto out;
	ret = yas537_i2c_write(0x88, (trim[15] >> 3) & 0x1E);
	if (ret < 0)
		goto out;
	ret = yas537_i2c_write(0x89, (trim[15] << 1) & 0x1E);
	if (ret < 0)
		goto out;
	ret = yas537_i2c_write(0x9E, trim[16] & 0x3F);
	if (ret < 0)
		goto out;

	ret = yas537_i2c_write(0x91, 0x03);
	if (ret < 0)
		goto out;
	ret = yas537_i2c_write(0x92, 0xF8);
	if (ret < 0)
		goto out;
	ret = yas537_i2c_write(0x9F, 0xFF);
	if (ret < 0)
		goto out;

	/* Measurement interval for the default 50 ms period. */
	ret = yas537_i2c_write(YAS537_REG_CONFIG, 0x0B);
	if (ret < 0)
		goto out;
	ret = yas537_i2c_write(0x87, 0x70);
	if (ret < 0)
		goto out;

out:
	mutex_unlock(&yas537_mutex);
	return ret;
}

static int yas537_start(void)
{
	int ret;

	mutex_lock(&yas537_mutex);
	ret = yas537_i2c_write(YAS537_REG_CTRL1, 0x21);
	mutex_unlock(&yas537_mutex);

	if (ret < 0)
		pr_err("yas537: failed to start measurement (%d)\n", ret);
	else
		msleep(5);

	return ret;
}

static int yas537_stop(void)
{
	int ret;

	mutex_lock(&yas537_mutex);
	/* The vendor driver resets the chip when measurement is disabled. */
	ret = yas537_i2c_write(YAS537_REG_CAL, 0x02);
	mutex_unlock(&yas537_mutex);

	return ret;
}

static int yas537_m_get_data(int *x, int *y, int *z, int *status)
{
	u8 buf[8];
	u16 v[3];
	u16 temp;
	s64 t[3];
	s64 p[3];
	s64 o[3];
	const s8 *m;
	int i;
	int dir;
	int ret;

	mutex_lock(&yas537_mutex);

	ret = yas537_i2c_read(YAS537_REG_MEASURE, buf, sizeof(buf));
	if (ret < 0) {
		mutex_unlock(&yas537_mutex);
		return ret;
	}
	memcpy(yas537_dbg_raw, buf, sizeof(buf));

	/* The vendor driver reads the temperature big-endian. */
	yas537_parse_sample(buf, v, &temp);

	yas537_sensitivity_correction(v, temp);

	memcpy(yas537_dbg_xy1y2, v, sizeof(v));
	yas537_dbg_temp = temp;

	t[0] = ((s64)v[0] - YAS537_DATA_CENTER) * 300;
	t[1] = ((s64)v[1] - (s64)v[2]) * 1732 / 10;
	t[2] = ((s64)YAS537_DATA_CENTER * 2 - (s64)v[1] - (s64)v[2]) * 300;

	dir = yas537_hw ? yas537_hw->direction : 2;
	if (dir < 0 || dir > 7)
		dir = 2;
	m = yas537_transformation[dir];

	/* Axis remapping (board direction). */
	p[0] = (s64)m[0] * t[0] + (s64)m[1] * t[1] + (s64)m[2] * t[2];
	p[1] = (s64)m[3] * t[0] + (s64)m[4] * t[1] + (s64)m[5] * t[2];
	p[2] = (s64)m[6] * t[0] + (s64)m[7] * t[1] + (s64)m[8] * t[2];

	/* Fixed sensitivity matrix from the vendor driver. */
	o[0] = ((s64)yas537_static_matrix[0] * p[0] +
		(s64)yas537_static_matrix[1] * p[1] +
		(s64)yas537_static_matrix[2] * p[2]) / 10000;
	o[1] = ((s64)yas537_static_matrix[3] * p[0] +
		(s64)yas537_static_matrix[4] * p[1] +
		(s64)yas537_static_matrix[5] * p[2]) / 10000;
	o[2] = ((s64)yas537_static_matrix[6] * p[0] +
		(s64)yas537_static_matrix[7] * p[1] +
		(s64)yas537_static_matrix[8] * p[2]) / 10000;

	for (i = 0; i < 3; i++) {
		s64 tmp = o[i];
		if (tmp > 2000000)
			tmp = 2000000;
		if (tmp < -2000000)
			tmp = -2000000;
		yas537_dbg_xyz[i] = (int)tmp;
	}

	mutex_unlock(&yas537_mutex);

	*x = yas537_dbg_xyz[0];
	*y = yas537_dbg_xyz[1];
	*z = yas537_dbg_xyz[2];
	*status = 3;

	return 0;
}

static int yas537_o_get_data(int *x, int *y, int *z, int *status)
{
	*x = 0;
	*y = 0;
	*z = 0;
	*status = 3;
	return 0;
}

static int yas537_m_open_report_data(int open)
{
	return 0;
}

static int yas537_m_enable(int en)
{
	int ret;

	pr_info("yas537: %s\n", en ? "enable" : "disable");

	if (en) {
		ret = yas537_chip_init();
		if (ret < 0)
			return ret;

		mutex_lock(&yas537_mutex);
		ret = yas537_enable_rcoil();
		mutex_unlock(&yas537_mutex);
		if (ret < 0)
			pr_warn("yas537: rcoil failed (%d)\n", ret);

		ret = yas537_start();
		if (ret < 0)
			return ret;
		atomic_set(&yas537_enable, 1);
	} else {
		yas537_stop();
		atomic_set(&yas537_enable, 0);
	}

	return 0;
}

static int yas537_m_set_delay(u64 delay)
{
	int ms = (int)(delay / 1000000);

	if (ms < 10)
		ms = 10;
	if (ms > 1000)
		ms = 1000;
	yas537_delay_ms = ms;

	return 0;
}

static int yas537_o_open_report_data(int open)
{
	return 0;
}

static int yas537_o_enable(int en)
{
	return 0;
}

static int yas537_o_set_delay(u64 delay)
{
	return 0;
}

static ssize_t yas537_debug_read(struct file *file, char __user *buf,
				 size_t count, loff_t *ppos)
{
	char tmp[256];
	int len;

	len = snprintf(tmp, sizeof(tmp),
		"raw %02x %02x %02x %02x %02x %02x %02x %02x\n"
		"temp %u xy1y2 %u %u %u xyz %d %d %d\n",
		yas537_dbg_raw[0], yas537_dbg_raw[1], yas537_dbg_raw[2],
		yas537_dbg_raw[3], yas537_dbg_raw[4], yas537_dbg_raw[5],
		yas537_dbg_raw[6], yas537_dbg_raw[7],
		yas537_dbg_temp, yas537_dbg_xy1y2[0], yas537_dbg_xy1y2[1],
		yas537_dbg_xy1y2[2], yas537_dbg_xyz[0], yas537_dbg_xyz[1],
		yas537_dbg_xyz[2]);

	return simple_read_from_buffer(buf, count, ppos, tmp, len);
}

static const struct file_operations yas537_debug_fops = {
	.owner = THIS_MODULE,
	.read = yas537_debug_read,
};

static int yas537_i2c_probe(struct i2c_client *client,
			    const struct i2c_device_id *id)
{
	struct mag_control_path ctl = { 0 };
	struct mag_data_path data = { 0 };
	int ret;

	pr_info("yas537: probing at i2c-%d addr 0x%02x\n",
		client->adapter->nr, client->addr);

	yas537_client = client;
	mutex_init(&yas537_mutex);

	if (yas537_chip_init() < 0) {
		pr_err("yas537: chip init failed\n");
		yas537_client = NULL;
		return -ENODEV;
	}

	ctl.m_open_report_data = yas537_m_open_report_data;
	ctl.m_set_delay = yas537_m_set_delay;
	ctl.m_enable = yas537_m_enable;
	ctl.o_open_report_data = yas537_o_open_report_data;
	ctl.o_set_delay = yas537_o_set_delay;
	ctl.o_enable = yas537_o_enable;
	ctl.is_report_input_direct = false;
	ctl.is_support_batch = false;
	ctl.is_use_common_factory = false;

	ret = mag_register_control_path(&ctl);
	if (ret) {
		pr_err("yas537: register control path failed (%d)\n", ret);
		goto err_client;
	}

	data.div_m = 1000;
	data.div_o = 1000;
	data.get_data_m = yas537_m_get_data;
	data.get_data_o = yas537_o_get_data;
	data.get_raw_data = NULL;

	ret = mag_register_data_path(&data);
	if (ret) {
		pr_err("yas537: register data path failed (%d)\n", ret);
		goto err_client;
	}

	proc_create("yas537_debug", 0, NULL, &yas537_debug_fops);

	yas537_init_flag = 0;
	pr_info("yas537: probe done\n");
	return 0;

err_client:
	yas537_client = NULL;
	return ret;
}

static int yas537_i2c_remove(struct i2c_client *client)
{
	remove_proc_entry("yas537_debug", NULL);
	yas537_client = NULL;
	yas537_init_flag = -1;
	i2c_set_clientdata(client, NULL);
	return 0;
}

static const struct i2c_device_id yas537_i2c_id[] = {
	{ "msensor", 0 },
	{ YAS537_DEV_NAME, 0 },
	{ }
};

#ifdef CONFIG_OF
static const struct of_device_id yas537_of_match[] = {
	{ .compatible = "mediatek,msensor" },
	{ }
};
#endif

static struct i2c_driver yas537_i2c_driver = {
	.driver = {
		.name = YAS537_DEV_NAME,
#ifdef CONFIG_OF
		.of_match_table = yas537_of_match,
#endif
	},
	.probe = yas537_i2c_probe,
	.remove = yas537_i2c_remove,
	.id_table = yas537_i2c_id,
};

static int yas537_local_init(void)
{
	int ret;

	yas537_hw = get_mag_dts_func("mediatek,yamaha537", &yas537_mag_cust);
	if (!yas537_hw) {
		pr_err("yas537: no DTS configuration found\n");
		return -ENODEV;
	}

	pr_info("yas537: i2c_num=%d addr=0x%02x direction=%d\n",
		yas537_hw->i2c_num, yas537_hw->i2c_addr[0],
		yas537_hw->direction);

	ret = i2c_add_driver(&yas537_i2c_driver);
	if (ret) {
		pr_err("yas537: i2c_add_driver failed (%d)\n", ret);
		return ret;
	}

	if (yas537_init_flag != 0) {
		pr_err("yas537: device was not probed\n");
		i2c_del_driver(&yas537_i2c_driver);
		return -ENODEV;
	}

	return 0;
}

static int yas537_remove(void)
{
	i2c_del_driver(&yas537_i2c_driver);
	return 0;
}

static struct mag_init_info yas537_init_info = {
	.name = "yas537",
	.init = yas537_local_init,
	.uninit = yas537_remove,
};

static int __init yas537_init(void)
{
	mag_driver_add(&yas537_init_info);
	return 0;
}

static void __exit yas537_exit(void)
{
	i2c_del_driver(&yas537_i2c_driver);
}

module_init(yas537_init);
module_exit(yas537_exit);

MODULE_DESCRIPTION("Yamaha YAS537 magnetometer driver");
MODULE_LICENSE("GPL");
