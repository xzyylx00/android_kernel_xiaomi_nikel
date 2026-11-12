/*
 * bmi160_gyro.c - Bosch BMI160 gyroscope driver
 *
 * The Redmi Note 4 (nikel, MT6797) carries a BMI160 whose accelerometer
 * half is handled by the bmi160_acc driver.  The chip's gyroscope is
 * exposed through the MediaTek gyroscope framework.  Both halves share
 * the same i2c client, so this driver reuses the register accessors that
 * the accelerometer driver exports.
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation.
 */

#include <linux/module.h>
#include <linux/i2c.h>
#include <linux/delay.h>
#include <linux/atomic.h>
#include <linux/input.h>

#include <cust_gyro.h>
#include <hwmsen_helper.h>
#include <gyroscope.h>

#define BMI160_GYRO_DEV_NAME	"bmi160_gyro"

#define BMI160_REG_CMD		0x7E
#define BMI160_REG_GYRO_RANGE	0x0F
#define BMI160_REG_GYRO_CONF	0x10
#define BMI160_REG_GYRO_DATA	0x0C

#define BMI160_CMD_GYRO_SUSPEND	0x14
#define BMI160_CMD_GYRO_NORMAL	0x15

#define BMI160_GYRO_RANGE_2000	0x00
#define BMI160_GYRO_ODR_25HZ	0x06
#define BMI160_GYRO_ODR_50HZ	0x07
#define BMI160_GYRO_ODR_100HZ	0x08
#define BMI160_GYRO_ODR_200HZ	0x09
#define BMI160_GYRO_ODR_400HZ	0x0A
#define BMI160_GYRO_ODR_800HZ	0x0B
#define BMI160_GYRO_ODR_1600HZ	0x0C
#define BMI160_GYRO_ODR_3200HZ	0x0D

/* Input events are in 1/7506 rad/s so the HAL can divide them. */
#define BMI160_GYRO_DIV		7506
#define BMI160_GYRO_SCALE	8

extern struct i2c_client *bmi160_acc_i2c_client;
extern s8 bmi_i2c_read_wrapper(u8 dev_addr, u8 reg_addr, u8 *data, u8 len);
extern s8 bmi_i2c_write_wrapper(u8 dev_addr, u8 reg_addr, u8 *data, u8 len);
extern int gyro_set_data_active(int enable);
extern void gyro_set_poll_delay(unsigned int delay_ms);
extern bool bmi160_ring_is_active(void);

static struct gyro_hw bmi160_gyro_cust;
static struct gyro_hw *bmi160_gyro_hw;
static struct hwmsen_convert bmi160_gyro_cvt;
static atomic_t bmi160_gyro_enable = ATOMIC_INIT(0);
static int bmi160_gyro_delay_ms = 5;
static int bmi160_gyro_range = BMI160_GYRO_RANGE_2000;
static int bmi160_gyro_init_flag = -1;
static struct gyro_init_info bmi160_gyro_init_info;

static int bmi160_gyro_write(u8 reg, u8 val)
{
	if (!bmi160_acc_i2c_client)
		return -ENODEV;
	return bmi_i2c_write_wrapper(0, reg, &val, 1);
}

static int bmi160_gyro_read(u8 reg, u8 *buf, u8 len)
{
	if (!bmi160_acc_i2c_client)
		return -ENODEV;
	return bmi_i2c_read_wrapper(0, reg, buf, len);
}

static int bmi160_gyro_set_odr(int ms)
{
	u8 odr;

	if (ms <= 5)
		odr = BMI160_GYRO_ODR_200HZ;
	else if (ms <= 10)
		odr = BMI160_GYRO_ODR_100HZ;
	else if (ms <= 20)
		odr = BMI160_GYRO_ODR_50HZ;
	else
		odr = BMI160_GYRO_ODR_25HZ;

	return bmi160_gyro_write(BMI160_REG_GYRO_CONF, odr);
}

static int bmi160_gyro_enable_nodata(int en)
{
	int ret;

	if (en) {
		ret = bmi160_gyro_write(BMI160_REG_GYRO_RANGE,
					bmi160_gyro_range);
		if (ret < 0)
			return ret;
		ret = bmi160_gyro_set_odr(bmi160_gyro_delay_ms);
		if (ret < 0)
			return ret;
		ret = bmi160_gyro_write(BMI160_REG_CMD,
					BMI160_CMD_GYRO_NORMAL);
		if (ret < 0)
			return ret;
		msleep(5);
		atomic_set(&bmi160_gyro_enable, 1);
	} else {
		/*
		 * The HAL takes gyro samples from the accelerometer ring
		 * buffer.  Leave the gyro running while that feed is active;
		 * otherwise the fusion loses the angular rate and the
		 * orientation sensor stops tracking.
		 */
		if (!bmi160_ring_is_active()) {
			ret = bmi160_gyro_write(BMI160_REG_CMD,
						BMI160_CMD_GYRO_SUSPEND);
			if (ret < 0)
				return ret;
		}
		atomic_set(&bmi160_gyro_enable, 0);
	}

	return 0;
}

static int bmi160_gyro_open_report_data(int open)
{
	return 0;
}

static int bmi160_gyro_set_delay(u64 ns)
{
	int ms = (int)(ns / 1000000);

	if (ms < 5)
		ms = 5;
	if (ms > 40)
		ms = 40;
	bmi160_gyro_delay_ms = ms;

	if (atomic_read(&bmi160_gyro_enable))
		return bmi160_gyro_set_odr(ms);

	return 0;
}

static int bmi160_gyro_get_data(int *x, int *y, int *z, int *status)
{
	u8 buf[6];
	s16 raw[3];
	s16 data[3];
	int i;

	if (bmi160_gyro_read(BMI160_REG_GYRO_DATA, buf, sizeof(buf)))
		return -EIO;

	for (i = 0; i < 3; i++)
		raw[i] = (s16)((buf[i * 2 + 1] << 8) | buf[i * 2]);

	for (i = 0; i < 3; i++)
		data[bmi160_gyro_cvt.map[i]] =
			bmi160_gyro_cvt.sign[i] * raw[i];

	*x = data[0] * BMI160_GYRO_SCALE;
	*y = data[1] * BMI160_GYRO_SCALE;
	*z = data[2] * BMI160_GYRO_SCALE;
	*status = 3;

	return 0;
}

static ssize_t bmi160_gyro_op_mode_show(struct device_driver *drv, char *buf)
{
	return sprintf(buf, "%s mode\n",
		       atomic_read(&bmi160_gyro_enable) ? "normal" : "suspend");
}

static int bmi160_gyro_activate(int en)
{
	if (en) {
		bmi160_gyro_set_odr(bmi160_gyro_delay_ms);
		gyro_set_poll_delay(bmi160_gyro_delay_ms);
	}
	return gyro_set_data_active(en ? 1 : 0);
}

static ssize_t bmi160_gyro_op_mode_store(struct device_driver *drv,
					 const char *buf, size_t count)
{
	int val;
	int ret;

	ret = kstrtoint(buf, 10, &val);
	if (ret)
		return ret;

	ret = bmi160_gyro_activate(val ? 1 : 0);
	if (ret)
		return ret;

	return count;
}

static ssize_t bmi160_gyro_delay_store(struct device_driver *drv,
				       const char *buf, size_t count)
{
	int ms;
	int ret;

	ret = kstrtoint(buf, 10, &ms);
	if (ret)
		return ret;
	if (ms <= 0)
		return -EINVAL;

	bmi160_gyro_delay_ms = ms;
	if (atomic_read(&bmi160_gyro_enable)) {
		bmi160_gyro_set_odr(ms);
		gyro_set_poll_delay(ms);
	}

	return count;
}

static ssize_t bmi160_gyro_odr_show(struct device_driver *drv, char *buf)
{
	return sprintf(buf, "%d\n", bmi160_gyro_delay_ms);
}

static ssize_t bmi160_gyro_odr_store(struct device_driver *drv,
				     const char *buf, size_t count)
{
	static const unsigned int odr_delay[] = {
		40, 20, 10, 5, 3, 2, 1, 1, 1
	};
	int val;
	int ret;

	ret = kstrtoint(buf, 10, &val);
	if (ret)
		return ret;

	if (val >= BMI160_GYRO_ODR_25HZ && val <= BMI160_GYRO_ODR_3200HZ)
		bmi160_gyro_delay_ms = odr_delay[val - BMI160_GYRO_ODR_25HZ];
	else if (val > 0)
		bmi160_gyro_delay_ms = val;
	else
		return -EINVAL;

	if (atomic_read(&bmi160_gyro_enable)) {
		ret = bmi160_gyro_set_odr(bmi160_gyro_delay_ms);
		if (ret < 0)
			return ret;
		gyro_set_poll_delay(bmi160_gyro_delay_ms);
	}

	return count;
}

static ssize_t bmi160_gyro_range_show(struct device_driver *drv, char *buf)
{
	return sprintf(buf, "%d\n", bmi160_gyro_range);
}

static ssize_t bmi160_gyro_range_store(struct device_driver *drv,
				       const char *buf, size_t count)
{
	int val;
	int ret;

	ret = kstrtoint(buf, 10, &val);
	if (ret)
		return ret;
	if (val < 0 || val > 4)
		return -EINVAL;

	bmi160_gyro_range = val;
	if (atomic_read(&bmi160_gyro_enable))
		bmi160_gyro_write(BMI160_REG_GYRO_RANGE, val);

	return count;
}

static ssize_t bmi160_gyro_bandwidth_store(struct device_driver *drv,
					   const char *buf, size_t count)
{
	int val;
	int ret;

	ret = kstrtoint(buf, 10, &val);
	if (ret)
		return ret;

	if (val > 0) {
		bmi160_gyro_delay_ms = val;
		if (atomic_read(&bmi160_gyro_enable))
			bmi160_gyro_set_odr(val);
	}

	return count;
}

static ssize_t bmi160_gyro_fifo_sel_store(struct device_driver *drv,
					  const char *buf, size_t count)
{
	return count;
}

static struct driver_attribute driver_attr_gyro_op_mode = {
	.attr = { .name = "gyro_op_mode", .mode = 0666 },
	.show = bmi160_gyro_op_mode_show,
	.store = bmi160_gyro_op_mode_store,
};

static struct driver_attribute driver_attr_gyro_odr = {
	.attr = { .name = "gyro_odr", .mode = 0666 },
	.show = bmi160_gyro_odr_show,
	.store = bmi160_gyro_odr_store,
};

static struct driver_attribute driver_attr_range = {
	.attr = { .name = "range", .mode = 0666 },
	.show = bmi160_gyro_range_show,
	.store = bmi160_gyro_range_store,
};

static struct driver_attribute driver_attr_gyro_range = {
	.attr = { .name = "gyro_range", .mode = 0666 },
	.show = bmi160_gyro_range_show,
	.store = bmi160_gyro_range_store,
};

static struct driver_attribute driver_attr_enable = {
	.attr = { .name = "enable", .mode = 0666 },
	.show = bmi160_gyro_op_mode_show,
	.store = bmi160_gyro_op_mode_store,
};

static struct driver_attribute driver_attr_op_mode = {
	.attr = { .name = "op_mode", .mode = 0666 },
	.show = bmi160_gyro_op_mode_show,
	.store = bmi160_gyro_op_mode_store,
};

static struct driver_attribute driver_attr_delay = {
	.attr = { .name = "delay", .mode = 0666 },
	.show = bmi160_gyro_odr_show,
	.store = bmi160_gyro_delay_store,
};

static struct driver_attribute driver_attr_bandwidth = {
	.attr = { .name = "bandwidth", .mode = 0666 },
	.show = bmi160_gyro_odr_show,
	.store = bmi160_gyro_bandwidth_store,
};

static struct driver_attribute driver_attr_fifo_data_sel = {
	.attr = { .name = "fifo_data_sel", .mode = 0666 },
	.show = bmi160_gyro_odr_show,
	.store = bmi160_gyro_fifo_sel_store,
};

static int bmi160_gyro_create_attr(struct device_driver *driver)
{
	int ret;

	ret = driver_create_file(driver, &driver_attr_gyro_op_mode);
	if (ret)
		return ret;
	ret = driver_create_file(driver, &driver_attr_gyro_odr);
	if (ret)
		return ret;
	ret = driver_create_file(driver, &driver_attr_range);
	if (ret)
		return ret;
	ret = driver_create_file(driver, &driver_attr_gyro_range);
	if (ret)
		return ret;
	ret = driver_create_file(driver, &driver_attr_enable);
	if (ret)
		return ret;
	ret = driver_create_file(driver, &driver_attr_op_mode);
	if (ret)
		return ret;
	ret = driver_create_file(driver, &driver_attr_delay);
	if (ret)
		return ret;
	ret = driver_create_file(driver, &driver_attr_bandwidth);
	if (ret)
		return ret;
	ret = driver_create_file(driver, &driver_attr_fifo_data_sel);
	if (ret)
		return ret;

	return 0;
}

static struct gyro_control_path bmi160_gyro_ctl = {
	.open_report_data = bmi160_gyro_open_report_data,
	.enable_nodata = bmi160_gyro_enable_nodata,
	.set_delay = bmi160_gyro_set_delay,
	.is_report_input_direct = false,
	.is_support_batch = false,
	.is_use_common_factory = false,
};

static struct gyro_data_path bmi160_gyro_data = {
	.get_data = bmi160_gyro_get_data,
	.vender_div = BMI160_GYRO_DIV,
};

static int bmi160_gyro_local_init(struct platform_device *pdev)
{
	int ret;

	bmi160_gyro_hw = get_gyro_dts_func("mediatek,bmi160_gyro",
					   &bmi160_gyro_cust);
	if (!bmi160_gyro_hw) {
		pr_err("bmi160_gyro: no DTS configuration found\n");
		return -ENODEV;
	}

	ret = hwmsen_get_convert(bmi160_gyro_hw->direction, &bmi160_gyro_cvt);
	if (ret) {
		pr_err("bmi160_gyro: invalid direction %d\n",
		       bmi160_gyro_hw->direction);
		return ret;
	}

	ret = gyro_register_control_path(&bmi160_gyro_ctl);
	if (ret) {
		pr_err("bmi160_gyro: register control path failed (%d)\n", ret);
		return ret;
	}

	ret = gyro_register_data_path(&bmi160_gyro_data);
	if (ret) {
		pr_err("bmi160_gyro: register data path failed (%d)\n", ret);
		return ret;
	}

	ret = bmi160_gyro_create_attr(
		&(bmi160_gyro_init_info.platform_diver_addr->driver));
	if (ret)
		pr_err("bmi160_gyro: create attributes failed (%d)\n", ret);

	bmi160_gyro_init_flag = 0;
	pr_info("bmi160_gyro: probe done, direction=%d\n",
		bmi160_gyro_hw->direction);
	return 0;
}

static int bmi160_gyro_remove(void)
{
	bmi160_gyro_init_flag = -1;
	return 0;
}

static struct gyro_init_info bmi160_gyro_init_info = {
	.name = BMI160_GYRO_DEV_NAME,
	.init = bmi160_gyro_local_init,
	.uninit = bmi160_gyro_remove,
};

static int __init bmi160_gyro_init(void)
{
	gyro_driver_add(&bmi160_gyro_init_info);
	return 0;
}

static void __exit bmi160_gyro_exit(void)
{
}

module_init(bmi160_gyro_init);
module_exit(bmi160_gyro_exit);

MODULE_DESCRIPTION("Bosch BMI160 gyroscope driver");
MODULE_LICENSE("GPL");
