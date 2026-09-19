#ifndef _SENSOR_H_
#define _SENSOR_H_

#ifdef __cplusplus
extern "C" {
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <linux/i2c.h>
#include <linux/i2c-dev.h>
#include <termios.h>
#include <fcntl.h> 
#include <unistd.h>
#include <sys/ioctl.h>

#include "mipi_api.h"
#include "isp_common.h"
#include "isp_enum.h"
#include "isp_api.h"

#define IMX415_I2C_ADDR	(0x34>>1)
	
#define I2C_DEVICE		"/dev/i2c-0"
#define DEVICE_HIKIO	"/dev/hikio"

#define SENSOR_PARAM "/home/imx415_mipi_attr.hex"


/**错误打印立刻打印*/
#define CHECK_RET(state, error_code)																	\
		if (state)																						\
		{																								\
			printf("[%s]:%d line [%s] return 0x%x ERROR\n",__FILE__,__LINE__ , __func__, error_code);	\
			return error_code;																			\
		}

#define SAFE_CLOSE(fd) do { if (-1 != fd) { close(fd); fd = -1; } } while(0)

#define GPIO_IOC_MAGIC    'G'
#define HAL_GPIO_DIR_OUTPUT _IOW(GPIO_IOC_MAGIC, 2, struct gpio_ctrl*)

typedef struct gpio_ctrl {
	unsigned int gpio;  //管脚号
	unsigned int param; //value
}GPIO_IOCTL;

typedef enum _ISP_PARAM_
{
	ISP_AE 		= 0, //ae
	ISP_AWB 	= 1, //awb
	ISP_COLOR   = 2, //色彩
	ISP_BRIGHT 	= 3, //亮度
	ISP_NR 		= 4, //降噪
	ISP_MF 		= 5, //镜像
}ISP_PARAM;

void isp_sensor_reset();

int sensor_write_reg(void *obj, FH_UINT16 addr, FH_UINT16 data);
int sensor_read_reg(void *obj, FH_UINT16 addr, FH_UINT16 *data);
int sensor_init_imx415(void *obj, Sensor_Init_t* initCfg);
int sensor_set_fmt_imx415(void *obj, int format);
int sensor_set_mipi_imx415();
int sensor_set_exposure_ratio_imx415(void *obj, FH_UINT32 exposure_ratio);
int sensor_get_exposure_ratio_imx415(void *obj, FH_UINT32 *exposure_ratio);
int sensor_get_attribute_imx415(void *obj, char *name, unsigned int *value);
int sensor_set_mirror_flip_imx415(void *obj, FH_UINT32 sensor_en_stat);

int isp_set_param(int key, int param);
int isp_set_ae(int ae_val);
int isp_set_awb(int awb_val);
int isp_set_saturation(int sat_val);
int isp_set_bright(int bright_val);
int isp_set_nr(int nr_val);
int isp_set_mirrorflip(int mf_val);
int isp_server_run();
void isp_server_stop(void);

unsigned short Sensor_Read(unsigned short addr);
void Sensor_Write(unsigned short addr, unsigned short data);

int GetIntt();
int SetIntt(void *obj, FH_UINT32 intt, FH_UINT8 fNo);
int GetGain();
int GetAEInfo(void *obj, Sensor_AE_INFO_S *sensAEInfo);
int GetAEDefault(void *obj, Sensor_AE_Default_S *sensAEDefault);
int CalcSnsValidGain(unsigned int *u32Gain);
int SetGain(void *obj, FH_UINT32 again, FH_UINT8 fNo);

#ifdef __cplusplus
}
#endif

#endif /* _SENSOR_H_ */

