/*
 * FPC TEE fingerprint sysfs bridge for MT6797 (nikel)
 *
 * The stock Xiaomi kernel exposes a platform device named
 * "soc:fpc_interrupt@0" (compatible "fpc,fpc_irq") whose sysfs
 * attributes are used by lib_fpc_tac_shared.so:
 *
 *   irq            read: 1 if an interrupt was latched, else 0
 *                  write "1": clear the latch
 *   reset          write "high" / "low": drive the sensor reset GPIO
 *   clk_enable     write "enable" / "disable": gate the SPI clock
 *   wakeup_enable  write "enable" / "disable": IRQ wakeup
 *   do_wakeup      write: wake any waiter
 *
 * This driver recreates that ABI on kernels which only ship the
 * generic FPC driver, so the vendor TEE fingerprint HAL can open.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/of_irq.h>
#include <linux/interrupt.h>
#include <linux/device.h>
#include <linux/sysfs.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/spinlock.h>
#include <linux/wait.h>
#include <linux/clk.h>
#include <linux/gpio.h>

struct fpc_irq_data {
	struct device *dev;
	int irq;
	int reset_gpio;
	bool fired;
	spinlock_t lock;
	wait_queue_head_t wq;
	struct clk *spi_clk;
};

static irqreturn_t fpc_irq_handler(int irq, void *data)
{
	struct fpc_irq_data *d = data;
	unsigned long flags;

	spin_lock_irqsave(&d->lock, flags);
	d->fired = true;
	spin_unlock_irqrestore(&d->lock, flags);

	wake_up_interruptible(&d->wq);
	return IRQ_HANDLED;
}

static ssize_t irq_show(struct device *dev, struct device_attribute *attr,
			char *buf)
{
	struct fpc_irq_data *d = dev_get_drvdata(dev);
	unsigned long flags;
	bool fired;

	spin_lock_irqsave(&d->lock, flags);
	fired = d->fired;
	d->fired = false;
	spin_unlock_irqrestore(&d->lock, flags);

	return scnprintf(buf, PAGE_SIZE, "%d\n", fired ? 1 : 0);
}

static ssize_t irq_store(struct device *dev, struct device_attribute *attr,
			 const char *buf, size_t count)
{
	struct fpc_irq_data *d = dev_get_drvdata(dev);
	unsigned long flags;

	if (sysfs_streq(buf, "1")) {
		spin_lock_irqsave(&d->lock, flags);
		d->fired = false;
		spin_unlock_irqrestore(&d->lock, flags);
	}

	return count;
}
static DEVICE_ATTR_RW(irq);

static ssize_t reset_store(struct device *dev, struct device_attribute *attr,
			   const char *buf, size_t count)
{
	struct fpc_irq_data *d = dev_get_drvdata(dev);

	if (d->reset_gpio < 0)
		return -EINVAL;

	if (sysfs_streq(buf, "high"))
		gpio_set_value(d->reset_gpio, 1);
	else if (sysfs_streq(buf, "low"))
		gpio_set_value(d->reset_gpio, 0);
	else
		return -EINVAL;

	return count;
}
static DEVICE_ATTR_WO(reset);

static ssize_t clk_enable_store(struct device *dev,
				struct device_attribute *attr,
				const char *buf, size_t count)
{
	struct fpc_irq_data *d = dev_get_drvdata(dev);
	int ret;

	if (!d->spi_clk)
		return -ENODEV;

	if (sysfs_streq(buf, "1") || sysfs_streq(buf, "enable")) {
		ret = clk_prepare_enable(d->spi_clk);
		if (ret)
			return ret;
	} else if (sysfs_streq(buf, "0") || sysfs_streq(buf, "disable")) {
		clk_disable_unprepare(d->spi_clk);
	} else {
		return -EINVAL;
	}

	return count;
}
static DEVICE_ATTR_WO(clk_enable);

static ssize_t wakeup_enable_store(struct device *dev,
				   struct device_attribute *attr,
				   const char *buf, size_t count)
{
	struct fpc_irq_data *d = dev_get_drvdata(dev);

	if (sysfs_streq(buf, "enable"))
		enable_irq_wake(d->irq);
	else if (sysfs_streq(buf, "disable"))
		disable_irq_wake(d->irq);
	else
		return -EINVAL;

	return count;
}
static DEVICE_ATTR_WO(wakeup_enable);

static ssize_t do_wakeup_store(struct device *dev,
			       struct device_attribute *attr,
			       const char *buf, size_t count)
{
	struct fpc_irq_data *d = dev_get_drvdata(dev);
	unsigned long flags;

	spin_lock_irqsave(&d->lock, flags);
	d->fired = true;
	spin_unlock_irqrestore(&d->lock, flags);

	wake_up_interruptible(&d->wq);
	return count;
}
static DEVICE_ATTR_WO(do_wakeup);

static struct attribute *fpc_irq_attrs[] = {
	&dev_attr_irq.attr,
	&dev_attr_reset.attr,
	&dev_attr_clk_enable.attr,
	&dev_attr_wakeup_enable.attr,
	&dev_attr_do_wakeup.attr,
	NULL,
};

static const struct attribute_group fpc_irq_attr_group = {
	.attrs = fpc_irq_attrs,
};

static int fpc_irq_probe(struct platform_device *pdev)
{
	struct device_node *np = pdev->dev.of_node;
	struct device_node *spi_np;
	struct fpc_irq_data *d;
	u32 val;
	int ret;

	d = devm_kzalloc(&pdev->dev, sizeof(*d), GFP_KERNEL);
	if (!d)
		return -ENOMEM;

	spin_lock_init(&d->lock);
	init_waitqueue_head(&d->wq);
	d->dev = &pdev->dev;
	d->reset_gpio = -1;
	platform_set_drvdata(pdev, d);

	d->irq = irq_of_parse_and_map(np, 0);
	if (!d->irq) {
		if (!of_property_read_u32(np, "fpc,gpio_irq", &val))
			d->irq = gpio_to_irq((int)val);
	}
	if (!d->irq) {
		dev_err(&pdev->dev, "no usable irq\n");
		return -ENODEV;
	}

	if (!of_property_read_u32(np, "fpc,gpio_reset", &val)) {
		d->reset_gpio = (int)val;
		ret = devm_gpio_request_one(&pdev->dev, d->reset_gpio,
					    GPIOF_OUT_INIT_HIGH, "fpc_reset");
		if (ret) {
			dev_warn(&pdev->dev, "reset gpio %d unavailable: %d\n",
				 d->reset_gpio, ret);
			d->reset_gpio = -1;
		}
	}

	ret = devm_request_irq(&pdev->dev, d->irq, fpc_irq_handler, 0,
			       "fpc_irq", d);
	if (ret) {
		dev_err(&pdev->dev, "request_irq %d failed: %d\n",
			d->irq, ret);
		return ret;
	}

	spi_np = of_parse_phandle(np, "fpc,spi", 0);
	if (spi_np) {
		d->spi_clk = of_clk_get(spi_np, 0);
		if (IS_ERR(d->spi_clk)) {
			dev_warn(&pdev->dev, "no spi clock: %ld\n",
				 PTR_ERR(d->spi_clk));
			d->spi_clk = NULL;
		}
		of_node_put(spi_np);
	}

	ret = sysfs_create_group(&pdev->dev.kobj, &fpc_irq_attr_group);
	if (ret) {
		dev_err(&pdev->dev, "sysfs group failed: %d\n", ret);
		return ret;
	}

	dev_info(&pdev->dev, "registered irq=%d reset=%d clk=%s\n",
		 d->irq, d->reset_gpio, d->spi_clk ? "yes" : "no");
	return 0;
}

static int fpc_irq_remove(struct platform_device *pdev)
{
	struct fpc_irq_data *d = platform_get_drvdata(pdev);

	sysfs_remove_group(&pdev->dev.kobj, &fpc_irq_attr_group);
	if (d->spi_clk)
		clk_disable_unprepare(d->spi_clk);
	return 0;
}

static const struct of_device_id fpc_irq_of_match[] = {
	{ .compatible = "fpc,fpc_irq" },
	{ }
};
MODULE_DEVICE_TABLE(of, fpc_irq_of_match);

static struct platform_driver fpc_irq_driver = {
	.probe = fpc_irq_probe,
	.remove = fpc_irq_remove,
	.driver = {
		.name = "fpc_irq",
		.of_match_table = fpc_irq_of_match,
	},
};

static int __init fpc_irq_init(void)
{
	return platform_driver_register(&fpc_irq_driver);
}
module_init(fpc_irq_init);

static void __exit fpc_irq_exit(void)
{
	platform_driver_unregister(&fpc_irq_driver);
}
module_exit(fpc_irq_exit);

MODULE_DESCRIPTION("FPC TEE fingerprint IRQ/clock/reset sysfs bridge");
MODULE_LICENSE("GPL v2");
