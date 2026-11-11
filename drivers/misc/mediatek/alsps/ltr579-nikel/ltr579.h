/*
 * Definitions for the Lite-On LTR-579ALS-01 ambient light / proximity sensor.
 *
 * Register map reconstructed from the official MIUI nikel kernel driver
 * (ltr579_probe / ltr579_ps_enable / ltr579_als_enable / ltr579_als_read).
 */

#ifndef __LTR579_H__
#define __LTR579_H__

/* main control: bit0 = PS enable, bit1 = ALS enable */
#define LTR579_REG_CONTR		0x00
#define LTR579_REG_PS_LED		0x01
#define LTR579_REG_PS_RATE		0x02
#define LTR579_REG_PS_CONF		0x03
#define LTR579_REG_ALS_CONF		0x04
#define LTR579_REG_ALS_RATE		0x05
#define LTR579_REG_PART_ID		0x06
#define LTR579_REG_PS_STATUS		0x07
#define LTR579_REG_PS_DATA_L		0x08
#define LTR579_REG_PS_DATA_H		0x09
#define LTR579_REG_ALS_RATIO_L		0x0A
#define LTR579_REG_ALS_RATIO_M		0x0B
#define LTR579_REG_ALS_RATIO_H		0x0C
#define LTR579_REG_ALS_DATA_L		0x0D
#define LTR579_REG_ALS_DATA_M		0x0E
#define LTR579_REG_ALS_DATA_H		0x0F
#define LTR579_REG_PS_OFFSET_L		0x19
#define LTR579_REG_PS_OFFSET_H		0x1A
/* 0x1B/0x1C are the HIGH threshold, 0x1D/0x1E the LOW threshold */
#define LTR579_REG_PS_THRES_HIGH_L	0x1B
#define LTR579_REG_PS_THRES_HIGH_H	0x1C
#define LTR579_REG_PS_THRES_LOW_L	0x1D
#define LTR579_REG_PS_THRES_LOW_H	0x1E

#define LTR579_PART_ID_VALUE		0xB1

#define LTR579_SUCCESS			0
#define LTR579_ERR_I2C			-1

#endif /* __LTR579_H__ */
