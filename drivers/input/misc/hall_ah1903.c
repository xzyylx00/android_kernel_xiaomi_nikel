/*
 * AH1903 hall sensor driver for Redmi Note 4 (nikel)
 *
 * Reconstructed from the stock "ah1903" driver used by the vendor kernel.
 * The sensor is wired to two GPIOs:
 *   - GPIO91 (EINT14): hall output
 *   - GPIO90:          "select" line, driven high by the stock driver
 *
 * The stock driver arms GPIO91 as an EINT and re-arms it after every edge.
 * Doing that here proved unreliable (an interrupt storm during boot made the
 * device reboot), so this version polls the pin instead.  The sensor state is
 * exposed through /proc/check_hall_exist (0 = no magnet, 1 = magnet) and
 * reported as SW_LID on an input device called "HALL".
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/input.h>
#include <linux/gpio.h>
#include <linux/pinctrl/consumer.h>
#include <linux/workqueue.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/jiffies.h>
#include <linux/uaccess.h>
#include <linux/delay.h>

#define HALL_EINT_GPIO		91
#define HALL_SELECT_GPIO	90
#define HALL_NAME		"HALL"
#define HALL_PROC_NAME		"check_hall_exist"
#define HALL_DBG_NAME		"hall_debug"
#define HALL_CTL_NAME		"hall_ctl"

/* Poll interval; CIT polls /proc/check_hall_exist every 200 ms. */
#define HALL_POLL_MS		100
/* Give the rest of the system some time to settle before the first read. */
#define HALL_FIRST_POLL_MS	2000

struct ah1903_data {
	int status;
	int raw;
	int have_raw;
	int have_select;
	int select_on;
	struct input_dev *idev;
	struct mutex lock;
	struct pinctrl *pinctrl;
	struct pinctrl_state *pins_select_output1;
	struct delayed_work work;
	struct proc_dir_entry *proc;
	struct proc_dir_entry *dbg;
	struct proc_dir_entry *ctl;
};

static void ah1903_report(struct ah1903_data *d, int on)
{
	if (d->status == !!on)
		return;

	d->status = !!on;

	if (!d->idev)
		return;

	input_event(d->idev, EV_SW, SW_LID, d->status);
	input_sync(d->idev);
}

static void ah1903_poll(struct work_struct *work)
{
	struct ah1903_data *d =
		container_of(to_delayed_work(work), struct ah1903_data, work);
	struct gpio_desc *desc;
	int value;

	/*
	 * Raise the select line once, shortly after probe.  The low -> high
	 * edge is what makes the sensor start driving its output; a line that
	 * is already high (e.g. left by the bootloader/pinctrl) does not.
	 */
	if (!d->select_on) {
		if (d->have_select)
			gpio_set_value(HALL_SELECT_GPIO, 1);
		d->select_on = 1;
		queue_delayed_work(system_freezable_wq, &d->work,
				   msecs_to_jiffies(HALL_POLL_MS));
		return;
	}

	desc = gpio_to_desc(HALL_EINT_GPIO);
	if (desc) {
		value = gpiod_get_raw_value(desc);
		if (value >= 0) {
			mutex_lock(&d->lock);
			d->raw = value;
			d->have_raw = 1;
			/* raw 0 means the magnet is present */
			ah1903_report(d, value == 0);
			mutex_unlock(&d->lock);
		}
	}

	queue_delayed_work(system_freezable_wq, &d->work,
			   msecs_to_jiffies(HALL_POLL_MS));
}

static int ah1903_setup_gpio(struct platform_device *pdev,
			     struct ah1903_data *d)
{
	int ret;

	/* Keep the hall output readable as a plain input. */
	ret = gpio_request(HALL_EINT_GPIO, "hall_eint");
	if (ret)
		dev_warn(&pdev->dev, "cannot request hall eint gpio: %d\n",
			 ret);
	else
		gpio_direction_input(HALL_EINT_GPIO);

	d->pinctrl = devm_pinctrl_get(&pdev->dev);
	if (IS_ERR(d->pinctrl)) {
		dev_warn(&pdev->dev, "cannot get hall pinctrl: %ld\n",
			 PTR_ERR(d->pinctrl));
		d->pinctrl = NULL;
		return 0;
	}

	d->pins_select_output1 =
		pinctrl_lookup_state(d->pinctrl, "h_select_output1");
	if (IS_ERR(d->pins_select_output1)) {
		dev_warn(&pdev->dev, "cannot find hall select state\n");
		d->pins_select_output1 = NULL;
		return 0;
	}

	ret = pinctrl_select_state(d->pinctrl, d->pins_select_output1);
	if (ret)
		dev_warn(&pdev->dev, "cannot select hall select output1: %d\n",
			 ret);

	/*
	 * The pinctrl state claims the pin but on this SoC it does not always
	 * leave the pad actively driven, so drive the select line explicitly.
	 * The sensor only starts converting after seeing a low -> high
	 * transition on this line, so start it low and raise it in the first
	 * poll (a static high leaves the sensor disabled).
	 */
	ret = gpio_request(HALL_SELECT_GPIO, "hall_select");
	if (ret) {
		dev_warn(&pdev->dev, "cannot request hall select gpio: %d\n",
			 ret);
		d->have_select = 0;
	} else {
		gpio_direction_output(HALL_SELECT_GPIO, 0);
		d->have_select = 1;
	}

	return 0;
}

static int ah1903_setup_input(struct ah1903_data *d)
{
	int ret;

	d->idev = input_allocate_device();
	if (!d->idev)
		return -ENOMEM;

	d->idev->name = HALL_NAME;
	d->idev->phys = HALL_NAME "/input0";
	d->idev->id.bustype = BUS_HOST;
	input_set_capability(d->idev, EV_SW, SW_LID);

	ret = input_register_device(d->idev);
	if (ret) {
		dev_err(&d->idev->dev, "cannot register hall input device\n");
		input_free_device(d->idev);
		d->idev = NULL;
		return ret;
	}

	return 0;
}

static int check_hall_exist_show(struct seq_file *m, void *v)
{
	struct ah1903_data *d = m->private;

	seq_printf(m, "%d\n", d->status);

	return 0;
}

static int check_hall_exist_open(struct inode *inode, struct file *file)
{
	return single_open(file, check_hall_exist_show, PDE_DATA(inode));
}

static const struct file_operations check_hall_exist_fops = {
	.owner		= THIS_MODULE,
	.open		= check_hall_exist_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
};

static int hall_debug_show(struct seq_file *m, void *v)
{
	struct ah1903_data *d = m->private;
	int sel = d->have_select ? gpio_get_value(HALL_SELECT_GPIO) : -1;

	seq_printf(m, "raw %d valid %d status %d sel %d\n",
		   d->raw, d->have_raw, d->status, sel);

	return 0;
}

static int hall_debug_open(struct inode *inode, struct file *file)
{
	return single_open(file, hall_debug_show, PDE_DATA(inode));
}

static const struct file_operations hall_debug_fops = {
	.owner		= THIS_MODULE,
	.open		= hall_debug_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
};

static ssize_t hall_ctl_write(struct file *file, const char __user *ubuf,
			      size_t count, loff_t *ppos)
{
	struct ah1903_data *d = PDE_DATA(file_inode(file));
	char buf[16];

	if (!d->have_select)
		return -ENODEV;

	if (count >= sizeof(buf))
		return -EINVAL;

	if (copy_from_user(buf, ubuf, count))
		return -EFAULT;
	buf[count] = '\0';

	if (!strncmp(buf, "sel0", 4)) {
		gpio_set_value(HALL_SELECT_GPIO, 0);
	} else if (!strncmp(buf, "sel1", 4)) {
		gpio_set_value(HALL_SELECT_GPIO, 1);
	} else if (!strncmp(buf, "pulse", 5)) {
		gpio_set_value(HALL_SELECT_GPIO, 0);
		msleep(30);
		gpio_set_value(HALL_SELECT_GPIO, 1);
	} else {
		return -EINVAL;
	}

	return count;
}

static const struct file_operations hall_ctl_fops = {
	.owner		= THIS_MODULE,
	.write		= hall_ctl_write,
	.llseek		= no_llseek,
};

static int ah1903_probe(struct platform_device *pdev)
{
	struct ah1903_data *d;
	int ret;

	d = devm_kzalloc(&pdev->dev, sizeof(*d), GFP_KERNEL);
	if (!d)
		return -ENOMEM;

	platform_set_drvdata(pdev, d);

	mutex_init(&d->lock);
	INIT_DELAYED_WORK(&d->work, ah1903_poll);
	d->status = 0;
	d->raw = -1;

	ah1903_setup_gpio(pdev, d);

	ret = ah1903_setup_input(d);
	if (ret)
		return ret;

	d->proc = proc_create_data(HALL_PROC_NAME, 0644, NULL,
				   &check_hall_exist_fops, d);
	d->dbg = proc_create_data(HALL_DBG_NAME, 0644, NULL,
				  &hall_debug_fops, d);
	d->ctl = proc_create_data(HALL_CTL_NAME, 0200, NULL,
				  &hall_ctl_fops, d);

	queue_delayed_work(system_freezable_wq, &d->work,
			   msecs_to_jiffies(HALL_FIRST_POLL_MS));

	pr_info("[ah1903] hall sensor ready\n");

	return 0;
}

static int ah1903_remove(struct platform_device *pdev)
{
	struct ah1903_data *d = platform_get_drvdata(pdev);

	cancel_delayed_work_sync(&d->work);

	if (d->proc)
		remove_proc_entry(HALL_PROC_NAME, NULL);
	if (d->dbg)
		remove_proc_entry(HALL_DBG_NAME, NULL);
	if (d->ctl)
		remove_proc_entry(HALL_CTL_NAME, NULL);
	if (d->idev)
		input_unregister_device(d->idev);
	gpio_free(HALL_EINT_GPIO);
	gpio_free(HALL_SELECT_GPIO);

	platform_set_drvdata(pdev, NULL);

	return 0;
}

static const struct of_device_id ah1903_of_match[] = {
	{ .compatible = "mediatek,hall" },
	{ }
};
MODULE_DEVICE_TABLE(of, ah1903_of_match);

static struct platform_driver ah1903_driver = {
	.probe		= ah1903_probe,
	.remove		= ah1903_remove,
	.driver		= {
		.name	= "ah1903",
		.owner	= THIS_MODULE,
		.of_match_table = ah1903_of_match,
	},
};

module_platform_driver(ah1903_driver);

MODULE_DESCRIPTION("AH1903 hall sensor driver");
MODULE_LICENSE("GPL");
