//SGM3784

#ifdef CONFIG_COMPAT

#include <linux/fs.h>
#include <linux/compat.h>

#endif
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/types.h>
#include <linux/wait.h>
#include <linux/slab.h>
#include <linux/fs.h>
#include <linux/sched.h>
#include <linux/poll.h>
#include <linux/device.h>
#include <linux/interrupt.h>
#include <linux/delay.h>
#include <linux/platform_device.h>
#include <linux/cdev.h>
#include <linux/errno.h>
#include <linux/time.h>
#include "kd_flashlight.h"
#include <asm/io.h>
#include <asm/uaccess.h>
#include "kd_camera_typedef.h"
#include <linux/hrtimer.h>
#include <linux/ktime.h>
#include <linux/version.h>
#include <linux/mutex.h>
#include <linux/gpio.h>
#include <linux/i2c.h>
#include <linux/leds.h>


//#define USE_NEW_DRIVER		//add by lijin 2015.5.20
/******************************************************************************
 * Debug configuration
******************************************************************************/
// availible parameter
// ANDROID_LOG_ASSERT
// ANDROID_LOG_ERROR
// ANDROID_LOG_WARNING
// ANDROID_LOG_INFO
// ANDROID_LOG_DEBUG
// ANDROID_LOG_VERBOSE
#define TAG_NAME "leds_HT_strobe"
#define PK_DBG_NONE(fmt, arg...)    do {} while (0)
#define PK_DBG_FUNC(fmt, arg...)    pr_debug("%s:%d " fmt, __func__ ,__LINE__, ##arg)
#define PK_WARN(fmt, arg...)        pr_debug("%s:%d " fmt, __func__ ,__LINE__, ##arg)
#define PK_NOTICE(fmt, arg...)      pr_debug("%s:%d " fmt, __func__ ,__LINE__, ##arg)
#define PK_INFO(fmt, arg...)        pr_debug("%s:%d " fmt, __func__ ,__LINE__, ##arg)
#define PK_TRC_FUNC(f)              pr_debug("%s:%d " fmt, __func__ ,__LINE__, ##arg)
#define PK_TRC_VERBOSE(fmt, arg...) pr_debug("%s:%d " fmt, __func__ ,__LINE__, ##arg)
#define PK_ERROR(fmt, arg...)       pr_debug("%s:%d " fmt, __func__ ,__LINE__, ##arg)


#define DEBUG_LEDS_STROBE
#ifdef  DEBUG_LEDS_STROBE
	#define PK_DBG PK_DBG_FUNC
	#define PK_VER PK_TRC_VERBOSE
	#define PK_ERR PK_ERROR
#else
	#define PK_DBG(a,...)
	#define PK_VER(a,...)
	#define PK_ERR(a,...)
#endif

/******************************************************************************
 * local variables
******************************************************************************/

static DEFINE_SPINLOCK(g_strobeSMPLock); /* cotta-- SMP proection */


static u32 strobe_Res = 0;
static u32 strobe_Timeus = 0;
static BOOL g_strobe_On = 0;


static int g_timeOutTimeMs=0;
int g_timeOutTimeMs_reg=0;

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,37))
static DEFINE_MUTEX(g_strobeSem);
#else
static DECLARE_MUTEX(g_strobeSem);
#endif


#define STROBE_DEVICE_ID 0x60

#define LED1_WARM
// #define LED1_COLD


static struct work_struct workTimeOut;

#define GPIO_LED_EN  		90//8
#define GPIO_LED_STROBE  	91//9
#define GPIO_LED_GPIO  		255//10

#define SGM3784_REG_ENABLE      0x0F
#define SGM3784_REG_MODE        0x01
#define SGM3784_REG_TIMING      0x02
#define SGM3784_REG_FLASH_LED1  0x06
#define SGM3784_REG_TORCH_LED1  0x08
#define SGM3784_REG_FLASH_LED2  0x09
#define SGM3784_REG_TORCH_LED2  0x0B


/*****************************************************************************
Functions
*****************************************************************************/
extern int iWriteRegI2C(u8 *a_pSendData , u16 a_sizeSendData, u16 i2cId);
extern int iReadRegI2C(u8 *a_pSendData , u16 a_sizeSendData, u8 * a_pRecvData, u16 a_sizeRecvData, u16 i2cId);
static void work_timeOutFunc(struct work_struct *data);

int g_Reg_First_write = 0;	//add by lijin 2015.5.13

static struct i2c_client *SGM3784_i2c_client = NULL;

struct SGM3784_platform_data {
	u8 torch_pin_enable;    // 1:  TX1/TORCH pin isa hardware TORCH enable
	u8 pam_sync_pin_enable; // 1:  TX2 Mode The ENVM/TX2 is a PAM Sync. on input
	u8 thermal_comp_mode_enable;// 1: LEDI/NTC pin in Thermal Comparator Mode
	u8 strobe_pin_disable;  // 1 : STROBE Input disabled
	u8 vout_mode_enable;  // 1 : Voltage Out Mode enable
};

struct SGM3784_chip_data {
	struct i2c_client *client;

	//struct led_classdev cdev_flash;
	//struct led_classdev cdev_torch;
	//struct led_classdev cdev_indicator;

	struct SGM3784_platform_data *pdata;
	struct mutex lock;

	u8 last_flag;
	u8 no_pdata;
};

/* i2c access*/
/*
static int SGM3784_read_reg(struct i2c_client *client, u8 reg,u8 *val)
{
	int ret;
	struct SGM3784_chip_data *chip = i2c_get_clientdata(client);

	mutex_lock(&chip->lock);
	ret = i2c_smbus_read_byte_data(client, reg);
	mutex_unlock(&chip->lock);

	if (ret < 0) {
		PK_ERR("failed reading at 0x%02x error %d\n",reg, ret);
		return ret;
	}
	*val = ret&0xff;

	return 0;
}*/

static int SGM3784_write_reg(struct i2c_client *client, u8 reg, u8 val)
{
	int ret=0;
	struct SGM3784_chip_data *chip = i2c_get_clientdata(client);

	mutex_lock(&chip->lock);
	ret =  i2c_smbus_write_byte_data(client, reg, val);
	mutex_unlock(&chip->lock);

	if (ret < 0)
		PK_ERR("failed writting at 0x%02x\n", reg);
	// else
		// PK_ERR("writting at 0x%02x successful\n", reg);
	return ret;
}

static int SGM3784_read_reg(struct i2c_client *client, u8 reg)
{
	int val=0;
	struct SGM3784_chip_data *chip = i2c_get_clientdata(client);

	mutex_lock(&chip->lock);
	val =  i2c_smbus_read_byte_data(client, reg);
	mutex_unlock(&chip->lock);


	return val;
}




/*
 * LM3643 dual-channel flash LED (i2c "strobe_main", 0x63).
 *
 * Register layout and sequences follow the official MIUI kernel driver
 * (LM3643_probe / setDuty_lm3643_x / flashEnable_lm3643_x) and the mainline
 * drivers/leds/flash/leds-lm3643.c driver:
 *   reg 0x01 ENABLE   : bits 1:0 LED1/LED2, bits 3:2 mode (2=torch, 3=flash)
 *   reg 0x03/0x04     : flash brightness LED1/LED2 (0..0x7f)
 *   reg 0x05/0x06     : torch brightness LED1/LED2 (0..0x7f)
 *   reg 0x0c DEV_ID   : 0x08 / 0x18 identify the chip variant
 */
#define LM3643_REG_ENABLE        0x01
#define LM3643_REG_FLASH_BR_LED1 0x03
#define LM3643_REG_FLASH_BR_LED2 0x04
#define LM3643_REG_TORCH_BR_LED1 0x05
#define LM3643_REG_TORCH_BR_LED2 0x06
#define LM3643_REG_DEV_ID        0x0c

#define LM3643_DUTY_MAX 25

static const u8 lm3643_flash_duty[LM3643_DUTY_MAX + 1] = {
	0x03, 0x08, 0x0c, 0x0e, 0x10, 0x14, 0x19, 0x1d, 0x21, 0x25,
	0x2a, 0x2e, 0x32, 0x37, 0x3b, 0x3f, 0x43, 0x48, 0x4c, 0x50,
	0x54, 0x5d, 0x65, 0x6e, 0x76, 0x7f,
};
static const u8 lm3643_torch_duty_a[LM3643_DUTY_MAX + 1] = {
	0x23, 0x47, 0x6a, 0x7f,
};
static const u8 lm3643_torch_duty_b[LM3643_DUTY_MAX + 1] = {
	0x10, 0x21, 0x32, 0x3c,
};

static int g_lm3643_mode = 1;	/* 0: id 0x08, 1: other, 2: id 0x18 */
static u8 g_lm3643_enable_shadow;

/*
 * The camera HAL (libcameracustom.so) opens the relative path
 * "sys/bus/i2c/devices/0-0063/debug/mode" and crashes when it is missing.
 * The official MIUI kernel exposes it through this attribute group.
 */
static int lm3643_i2c_func_ok = 1;

static ssize_t lm3643_mode_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	return sprintf(buf, "%d\n", lm3643_i2c_func_ok);
}

static DEVICE_ATTR(mode, 0444, lm3643_mode_show, NULL);

static struct attribute *lm3643_debug_attrs[] = {
	&dev_attr_mode.attr,
	NULL,
};

static struct attribute_group lm3643_debug_attr_group = {
	.name = "debug",
	.attrs = lm3643_debug_attrs,
};

static int SGM3784_chip_init(struct SGM3784_chip_data *chip)
{
	struct i2c_client *client = chip->client;
	int dev_id;

	/* bring the chip out of shutdown before touching registers */
	flashlight_gpio_set(FLASHLIGHT_PIN_HWEN, STATE_HIGH);

	dev_id = SGM3784_read_reg(client, LM3643_REG_DEV_ID);
	PK_DBG("LM3643 dev_id=0x%02x\n", dev_id);

	if (dev_id == 0x08)
		g_lm3643_mode = 0;
	else if (dev_id == 0x18)
		g_lm3643_mode = 2;
	else
		g_lm3643_mode = 1;

	g_lm3643_enable_shadow = 0;
	SGM3784_write_reg(client, LM3643_REG_ENABLE, 0x00);

	return 0;
}

static int SGM3784_probe(struct i2c_client *client,
			const struct i2c_device_id *id)
{
	struct SGM3784_chip_data *chip;
	struct SGM3784_platform_data *pdata = client->dev.platform_data;

	int err = -1;

	PK_DBG("SGM3784_probe start--->.\n");

	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C)) {
		err = -ENODEV;
		printk(KERN_ERR  "SGM3784 i2c functionality check fail.\n");
		return err;
	}

	chip = kzalloc(sizeof(struct SGM3784_chip_data), GFP_KERNEL);
	chip->client = client;

	mutex_init(&chip->lock);
	i2c_set_clientdata(client, chip);

	if(pdata == NULL){ //values are set to Zero.
		PK_ERR("SGM3784 Platform data does not exist\n");
		pdata = kzalloc(sizeof(struct SGM3784_platform_data),GFP_KERNEL);
		chip->pdata  = pdata;
		chip->no_pdata = 1;
	}

	chip->pdata  = pdata;
	if(SGM3784_chip_init(chip)<0)
		goto err_chip_init;

	lm3643_i2c_func_ok = 1;
	err = sysfs_create_group(&client->dev.kobj, &lm3643_debug_attr_group);
	if (err) {
		PK_ERR("%s, Failed to create sysfs file: %d\n", __func__, err);
		goto err_chip_init;
	}

	SGM3784_i2c_client = client;
	PK_DBG("SGM3784 Initializing is done \n");

	return 0;

err_chip_init:
	i2c_set_clientdata(client, NULL);
	kfree(chip);
	PK_ERR("SGM3784 probe is failed \n");
	return -ENODEV;
}

static int SGM3784_remove(struct i2c_client *client)
{
	struct SGM3784_chip_data *chip = i2c_get_clientdata(client);

	sysfs_remove_group(&client->dev.kobj, &lm3643_debug_attr_group);
    if(chip->no_pdata)
		kfree(chip->pdata);
	kfree(chip);
	return 0;
}


#define SGM3784_NAME "leds-LM3643"
#define I2C_STROBE_MAIN_SLAVE_7_BIT_ADDR  0x30
static const struct i2c_device_id SGM3784_id[] = {
	{SGM3784_NAME, 0},
	{}
};
#ifdef CONFIG_OF
static const struct of_device_id SGM3784_of_match[] = {
	{.compatible = "mediatek,strobe_main"},
	{},
};
#endif
static struct i2c_driver SGM3784_i2c_driver = {
	.driver = {
		.name  = SGM3784_NAME,
#ifdef CONFIG_OF
		.of_match_table = SGM3784_of_match,
#endif
	},
	.probe	= SGM3784_probe,
	.remove   = SGM3784_remove,
	.id_table = SGM3784_id,
};

//struct SGM3784_platform_data SGM3784_pdata = {0, 0, 0, 0, 0};
//static struct i2c_board_info __initdata i2c_SGM3784={ I2C_BOARD_INFO(SGM3784_NAME, I2C_STROBE_MAIN_SLAVE_7_BIT_ADDR), 
//							.platform_data = &SGM3784_pdata,};

static int __init SGM3784_init(void)
{
	printk("SGM3784_init\n");
	//i2c_register_board_info(2, &i2c_SGM3784, 1);
	//i2c_register_board_info(1, &i2c_SGM3784, 1);


	return i2c_add_driver(&SGM3784_i2c_driver);
}

static void __exit SGM3784_exit(void)
{
	i2c_del_driver(&SGM3784_i2c_driver);
}


module_init(SGM3784_init);
module_exit(SGM3784_exit);

MODULE_DESCRIPTION("Flash driver for SGM3784");
MODULE_AUTHOR("LiJin <lijin@malatamobile.com>");
MODULE_LICENSE("GPL v2");



int readReg(int reg)
{
#if 1
    int val;
    val = SGM3784_read_reg(SGM3784_i2c_client, reg);
	// PK_DBG("read reg=%x val=%x\n", reg,val);
    return (int)val;
#else	

    char buf[2];
    char bufR[2];
    buf[0]=reg;
    iReadRegI2C(buf , 1, bufR,1, STROBE_DEVICE_ID);
    PK_DBG("qq reg=%x val=%x qq\n", buf[0],bufR[0]);
    return (int)bufR[0];
#endif	
}

int writeReg(int reg, int val)
{
#if 1
    int err;
    //struct SGM3784_chip_data *chip = i2c_get_clientdata(client);
    PK_DBG("writeReg  ---1--- reg=0x%x,val = 0x0%x,g_Reg_First_write=%d\n",reg,val,g_Reg_First_write);
    if(0)//(reg == 0x02)
    {
	if(g_Reg_First_write == 0)
        {
    	    err = SGM3784_write_reg(SGM3784_i2c_client, reg, val);
	    PK_DBG("writeReg ---2--- reg=0x%x,g_Reg_First_write=%d\n",reg,g_Reg_First_write); 
	    //mutex_lock(&chip->lock);
            g_Reg_First_write = 1;
	    //mutex_unlock(&chip->lock);
	    return (int)val;
        }
        return 0;
    }
    else
    {
    	    err = SGM3784_write_reg(SGM3784_i2c_client, reg, val);
			// readReg(reg);
	    return (int)val;
    }
#else	

    char buf[2];
    buf[0]=reg;
    buf[1]=data;
	
    iWriteRegI2C(buf, 2, STROBE_DEVICE_ID);

   return 0;
#endif   
}

#define e_DutyNum 16
//torch 300mA
#define TORCHDUTYNUM 2 
int isMovieMode[e_DutyNum+1][e_DutyNum+1] = 
{ 
	{-1,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0},//duty1=-1,duty2=-1,0,1
	{ 1,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0},//duty1=0, duty2=-1,0,1
	{ 1,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0},//duty1=1, duty2=-1,0,1
	{ 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
	{ 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
	{ 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
	{ 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
	{ 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
	{ 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
	{ 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
	{ 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
	{ 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
	{ 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
	{ 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
	{ 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
	{ 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
	{ 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
	//{0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0}
};

//static int torchLED1Reg[e_DutyNum+1] = {0,63,127,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0};//93,187

#if defined(USE_NEW_DRIVER)
static int __maybe_unused torchLED1Reg[e_DutyNum] = {3,6,0,0,0,0,0,0,0,0,0,0,0,0,0,0};//56,111,167,204,259,300     //56,111
static int __maybe_unused torchLED2Reg[e_DutyNum] = {3,6,0,0,0,0,0,0,0,0,0,0,0,0,0,0};//56,111,167,204,259,300	//56,111
#else
static int __maybe_unused torchLED1Reg[e_DutyNum+1] = {0,3,6,0,0,0,0,0,0,0,0,0,0,0,0,0,0};//56,111,//167,204,259,300 //Reg0x08
static int __maybe_unused torchLED2Reg[e_DutyNum+1] = {0,3,6,0,0,0,0,0,0,0,0,0,0,0,0,0,0};//56,111,//167,204,259,300 //Reg0x0B
#endif


//static int flashLED1Reg[e_DutyNum+1] = {0,7,15,23,31,39,47,55,63,71,79,87,95,103,111,119,127};//93,187,280,374,468,562,655,749,843,937,1030,1124,1218,1312,1405,1499

#if defined(USE_NEW_DRIVER)
static int __maybe_unused flashLED1Reg[e_DutyNum] = {3,6,8,10,13,16,19,22,25,27,30,32,35,38,41,43};//50,111,150,200,250,300,352,407,463,500,555,592,648,703,759 ,796  ,//851,907,944,1086
static int __maybe_unused flashLED2Reg[e_DutyNum] = {3,6,8,10,13,16,19,22,25,27,30,32,35,38,41,43};//50,111,150,200,250,300,352,407,463,500,555,592,648,703,759 ,796  ,//851,907,944,1086
#else
static int __maybe_unused flashLED1Reg[e_DutyNum+1] = {0,3,6,8,11,14,16,19,22,25,27,30,32,35,38,41,43/* ,54 */};//50,100,150,200,250,300,352,407,463,500,555,592,648,703,759 ,796  ,//851,907,944,1086
//50,111,150,200,250,300,352,407,463,500,555,592,648,703,759 ,796,1000  I_FL1 × 18.5mA
static int __maybe_unused flashLED2Reg[e_DutyNum+1] = {0,3,6,8,11,14,16,19,22,25,27,30,32,35,38,41,43/* ,54 */};//50,100,150,200,250,300,352,407,463,500,555,592,648,703,759 ,796  ,//851,907,944,1086
//duty =-1时 hal层不会执行CMD_SETDUTY 直接执行CMD_SETFLASHOFF
#endif

#if defined(USE_NEW_DRIVER)
static int __maybe_unused isMovieModeLED1[e_DutyNum] = {1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0};
static int __maybe_unused isMovieModeLED2[e_DutyNum] = {1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0};
#endif

int m_duty1=0;
int m_duty2=0;
int LED1Closeflag = 0;
int LED2Closeflag = 0;
char g_EnableReg = 0;
char g_ModeReg = 0;
char g_TimeReg = 0;
// char g_FlashReg1 = 0;
// char g_FlashReg2 = 0;
// char g_TorchReg1 = 0;
// char g_TorchReg2 = 0;


static void lm3643_write_reg(u8 reg, u8 val)
{
	if (SGM3784_i2c_client)
		SGM3784_write_reg(SGM3784_i2c_client, reg, val);
}

static void lm3643_set_duty(int ch, int duty)
{
	u8 torch;

	if (duty < 0)
		duty = 0;
	if (duty > LM3643_DUTY_MAX)
		duty = LM3643_DUTY_MAX;

	if (g_lm3643_mode == 1)
		torch = lm3643_torch_duty_a[duty];
	else
		torch = lm3643_torch_duty_b[duty];

	if (ch == 1) {
		lm3643_write_reg(LM3643_REG_TORCH_BR_LED1, torch);
		lm3643_write_reg(LM3643_REG_FLASH_BR_LED1,
				 lm3643_flash_duty[duty]);
	} else {
		lm3643_write_reg(LM3643_REG_TORCH_BR_LED2, torch);
		lm3643_write_reg(LM3643_REG_FLASH_BR_LED2,
				 lm3643_flash_duty[duty]);
	}
}

/* duty 0..3 selects torch mode, anything above selects flash mode
 * (matches official flashEnable_lm3643_x).
 */
static void lm3643_enable(int ch, int duty)
{
	u8 led = (ch == 1) ? 0x01 : 0x02;
	u8 other = (ch == 1) ? 0x02 : 0x01;
	u8 mode = (duty <= 3) ? 0x08 : 0x0c;

	if (!(g_lm3643_enable_shadow & other))
		g_lm3643_enable_shadow &= ~0x0c;

	g_lm3643_enable_shadow |= led | mode;
	lm3643_write_reg(LM3643_REG_ENABLE, g_lm3643_enable_shadow);
	PK_DBG("lm3643 enable ch%d duty=%d reg=0x%02x\n", ch, duty,
	       g_lm3643_enable_shadow);
}

static void lm3643_disable(int ch)
{
	u8 led = (ch == 1) ? 0x01 : 0x02;
	u8 other = (ch == 1) ? 0x02 : 0x01;

	if (g_lm3643_enable_shadow & other)
		g_lm3643_enable_shadow &= ~led;
	else
		g_lm3643_enable_shadow &= ~(led | 0x0c);

	lm3643_write_reg(LM3643_REG_ENABLE, g_lm3643_enable_shadow);
	PK_DBG("lm3643 disable ch%d reg=0x%02x\n", ch,
	       g_lm3643_enable_shadow);
}

int FlashIc_Enable(void)
{
	flashlight_gpio_set(FLASHLIGHT_PIN_HWEN, STATE_HIGH);
	PK_DBG("LM3643 HWEN high\n");
	return 0;
}

int FlashIc_Disable(void)
{
	lm3643_disable(1);
	lm3643_disable(2);
	flashlight_gpio_set(FLASHLIGHT_PIN_HWEN, STATE_LOW);
	PK_DBG("LM3643 HWEN low\n");
	return 0;
}

int flashEnable_SGM3784_1(void)
{
	lm3643_enable(1, m_duty1);
	return 0;
}

int setDuty_SGM3784_1(int duty)
{
	m_duty1 = duty;
	lm3643_set_duty(1, duty);
	return 0;
}

int flashEnable_SGM3784_2(void)
{
	lm3643_enable(2, m_duty2);
	return 0;
}

int flashDisable_SGM3784_1(void)
{
	lm3643_disable(1);
	return 0;
}

int flashDisable_SGM3784_2(void)
{
	lm3643_disable(2);
	return 0;
}

int setDuty_SGM3784_2(int duty)
{
	m_duty2 = duty;
	lm3643_set_duty(2, duty);
	return 0;
}

/* Externs declared in kd_flashlight_type.h for the dual-LED interface. */
int flashEnable_lm3643_1(void) { return flashEnable_SGM3784_1(); }
int flashEnable_lm3643_2(void) { return flashEnable_SGM3784_2(); }
int flashDisable_lm3643_1(void) { return flashDisable_SGM3784_1(); }
int flashDisable_lm3643_2(void) { return flashDisable_SGM3784_2(); }
int setDuty_lm3643_1(int duty) { return setDuty_SGM3784_1(duty); }
int setDuty_lm3643_2(int duty) { return setDuty_SGM3784_2(duty); }
int FL_Enable(void)
{
	PK_DBG(" FL_Enable line=%d\n",__LINE__);
    flashEnable_SGM3784_2();

	return 0;
}



int FL_Disable(void)
{
	PK_DBG(" FL_Disable line=%d\n",__LINE__);
    flashDisable_SGM3784_2();

	return 0;
}

int FL_dim_duty(kal_uint32 duty)
{
    setDuty_SGM3784_2(duty);

    PK_DBG(" FL_dim_duty line=%d\n",__LINE__);
    return 0;
}




int FL_Init(void)
{
	//int ret;
	PK_DBG("LED1_FL_Init!\n");
    FlashIc_Enable();

    INIT_WORK(&workTimeOut, work_timeOutFunc);
    PK_DBG(" FL_Init line=%d\n",__LINE__);
    return 0;
}


int FL_Uninit(void)
{
	PK_DBG("LED1_FL_Uninit!\n");
	FlashIc_Disable();
    return 0;
}

/*****************************************************************************
User interface
*****************************************************************************/

static void work_timeOutFunc(struct work_struct *data)
{
    FL_Disable();
    PK_DBG("LED1TimeOut_callback\n");
}



enum hrtimer_restart ledTimeOutCallback(struct hrtimer *timer)
{
    schedule_work(&workTimeOut);
    return HRTIMER_NORESTART;
}
static struct hrtimer g_timeOutTimer;
void timerInit(void)
{
  INIT_WORK(&workTimeOut, work_timeOutFunc);
	g_timeOutTimeMs=1000; //1s
	hrtimer_init( &g_timeOutTimer, CLOCK_MONOTONIC, HRTIMER_MODE_REL );
	g_timeOutTimer.function=ledTimeOutCallback;

}



static int constant_flashlight_ioctl(unsigned int cmd, unsigned long arg)
{
	int i4RetValue = 0;
	int ior_shift;
	int iow_shift;
	int iowr_shift;
	ior_shift = cmd - (_IOR(FLASHLIGHT_MAGIC,0, int));
	iow_shift = cmd - (_IOW(FLASHLIGHT_MAGIC,0, int));
	iowr_shift = cmd - (_IOWR(FLASHLIGHT_MAGIC,0, int));
	PK_DBG("SGM3784_LED1_constant_flashlight_ioctl() line=%d ior_shift=%d, iow_shift=%d iowr_shift=%d arg=%ld\n",__LINE__, ior_shift, iow_shift, iowr_shift, arg);
    switch(cmd)
    {

		case FLASH_IOC_SET_TIME_OUT_TIME_MS:
			// if(arg>1600)
				// arg= 1600;//最大支持1.6秒亮灯
			// g_timeOutTimeMs_reg=arg/100-1;  //预闪的时间长一点 计算主闪的duty值就会更准确  因为电流小 亮灯不超时都不会烧灯
			// if(g_timeOutTimeMs_reg>15)
				// g_timeOutTimeMs_reg = 15;
			g_timeOutTimeMs = arg;//用于给torch mode的亮灯计时
			#ifdef LED1_WARM
			PK_DBG("FLASH_IOC_SET_TIME_OUT_TIME_MS LED2: %ld g_timeOutTimeMs_reg=0x%x \n",arg,g_timeOutTimeMs_reg);
			#else
			PK_DBG("FLASH_IOC_SET_TIME_OUT_TIME_MS LED1: %ld g_timeOutTimeMs_reg=0x%x \n",arg,g_timeOutTimeMs_reg);
			#endif
		break;


    	case FLASH_IOC_SET_DUTY :
			#ifdef LED1_WARM
    		PK_DBG("FLASH_IOC_SET_DUTY LED2: %ld\n",arg);
			#else
    		PK_DBG("FLASH_IOC_SET_DUTY LED1: %ld\n",arg);
			#endif
			// if(arg < 0)
				// arg = 0;
			// if(arg>=e_DutyNum)
				// arg=e_DutyNum-1;
			#ifdef LED1_WARM
			m_duty2 = arg;
			#else
			m_duty1 = arg;
			#endif
    		break;


    	case FLASH_IOC_SET_STEP:
    		PK_DBG("FLASH_IOC_SET_STEP: %ld\n",arg);

    		break;

    	case FLASH_IOC_SET_ONOFF :
			#ifdef LED1_WARM
    		PK_DBG("FLASH_IOC_SET_ONOFF LED2: %ld\n",arg);
			#else
    		PK_DBG("FLASH_IOC_SET_ONOFF LED1: %ld\n",arg);
			#endif
    		if(arg==1)
    		{
				if(g_timeOutTimeMs!=0)
	            {
	            	ktime_t ktime;
					ktime = ktime_set( 0, g_timeOutTimeMs*1000000 );
					hrtimer_start( &g_timeOutTimer, ktime, HRTIMER_MODE_REL );
	            }
				#ifdef LED1_WARM
				LED2Closeflag = 0;//亮冷灯
				#else
				LED1Closeflag = 0;
				#endif
    			FlashIc_Enable();
				#ifdef LED1_WARM
				setDuty_SGM3784_2(m_duty2);
    			flashEnable_SGM3784_2();
				#else
				setDuty_SGM3784_1(m_duty1);//调用setDuty_SGM3784_2 函数里面区分是LED1还是LED2
    			flashEnable_SGM3784_1(); //调用flashEnable_SGM3784_2 函数里面区分是LED1还是LED2
				#endif
    		}
    		else
    		{
				#ifdef LED1_WARM
				LED2Closeflag = 1;//灭冷灯
				#else
    			LED1Closeflag = 1;
				#endif
    			FlashIc_Enable();
				// FL_dim_duty(m_duty1);
				#ifdef LED1_WARM
				flashDisable_SGM3784_2();
				#else
				flashDisable_SGM3784_1(); //调用flashDisable_SGM3784_2 函数里面区分是LED1还是LED2
				#endif
				hrtimer_cancel( &g_timeOutTimer );
    		}
    		break;
    	case FLASH_IOC_SET_REG_ADR:
    	    break;
    	case FLASH_IOC_SET_REG_VAL:
    	    break;
    	case FLASH_IOC_SET_REG:
    	    break;
    	case FLASH_IOC_GET_REG:
    	    break;



		default :
    		PK_DBG(" No such command \n");
    		i4RetValue = -EPERM;
    		break;
    }
    return i4RetValue;
}




static int constant_flashlight_open(void *pArg)
{
    int i4RetValue = 0;
    PK_DBG("constant_flashlight_open line=%d\n", __LINE__);

	if (0 == strobe_Res)
	{
	    FL_Init();
		timerInit();
	}
	PK_DBG("constant_flashlight_open line=%d\n", __LINE__);
	spin_lock_irq(&g_strobeSMPLock);


    if(strobe_Res)
    {
        PK_ERR(" busy!\n");
        i4RetValue = -EBUSY;
    }
    else
    {
        strobe_Res += 1;
    }


    spin_unlock_irq(&g_strobeSMPLock);
    PK_DBG("constant_flashlight_open line=%d\n", __LINE__);

    return i4RetValue;

}


static int constant_flashlight_release(void *pArg)
{
    PK_DBG(" constant_flashlight_release\n");

    if (strobe_Res)
    {
        spin_lock_irq(&g_strobeSMPLock);

        strobe_Res = 0;
        strobe_Timeus = 0;

        /* LED On Status */
        g_strobe_On = FALSE;

        spin_unlock_irq(&g_strobeSMPLock);

    	FL_Uninit();
    }

    PK_DBG(" Done\n");

    return 0;

}


FLASHLIGHT_FUNCTION_STRUCT	constantFlashlightFunc=
{
	constant_flashlight_open,
	constant_flashlight_release,
	constant_flashlight_ioctl
};


MUINT32 constantFlashlightInit(PFLASHLIGHT_FUNCTION_STRUCT *pfFunc)
{
    if (pfFunc != NULL)
    {
        *pfFunc = &constantFlashlightFunc;
    }
    return 0;
}



/* LED flash control for high current capture mode*/
ssize_t strobe_VDIrq(void)
{

    return 0;
}

EXPORT_SYMBOL(strobe_VDIrq);


