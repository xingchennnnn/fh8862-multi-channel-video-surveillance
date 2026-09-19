#ifdef __cplusplus
#if __cplusplus
extern "C"{
#endif
#endif /* End of #ifdef __cplusplus */


#define SYSFS_GPIO_DIR  "/sys/class/gpio"
#define SYSFS_GPIO_NAME "/GPIO"

#define MAX_BUF         64


#include "sensor.h"



const unsigned int IMX415RegToGain[241] =
{
    64,66,69,71,73,76,79,82,84,87,90,94,97,100,104,107,
    111,115,119,123,128,132,137,142,147,152,157,163,168,
    174,180,187,193,200,207,214,222,230,238,246,255,264,
    273,283,293,303,313,324,336,348,360,373,386,399,413,
    428,443,458,474,491,508,526,545,564,584,604,625,647,
    670,694,718,743,769,797,825,853,883,915,947,980,1014,
    1050,1087,1125,1165,1206,1248,1292,1337,1384,1433,1483,
    1535,1589,1645,1703,1763,1825,1889,1955,2024,2095,2169,
    2245,2324,2405,2490,2578,2668,2762,2859,2959,3063,3171,
    3282,3398,3517,3641,3769,3901,4038,4180,4327,4479,4637,
    4800,4968,5143,5324,5511,5704,5905,6112,6327,6549,6780,
    7018,7264,7520,7784,8057,8340,8633,8936,9250,9575,9912,
    10260,10621,10994,11380,11780,12194,12623,13067,13526,
    14001,14493,15003,15530,16076,16641,17225,17831,18457,
    19106,19777,20472,21192,21937,22708,23506,24332,25187,
    26072,26988,27937,28918,29935,30987,32075,33203,34370,
    35577,36828,38122,39462,40848,42284,43770,45308,46900,
    48548,50255,52021,53849,55741,57700,59728,61827,64000,
    66249,68577,70987,73482,76064,78737,81504,84368,87333,
    90402,93579,96868,100272,103796,107443,111219,115128,119174,
    123362,127697,132184,136830,141638,146616,151768,157101,162622,
    168337,174253,180377,186715,193277,200069,207100,214378,221912,
    229710,237783,246139,254789,
};//0~72db


/*
*   Name: isp_sensor_reset
*            sensor复位, 上电后操作sensor前需要复位一次
*
*   Parameters:
*
*            None
*
*   Return:
*            None
*   Note:
*       无
*/
void isp_sensor_reset()
{

    int fd, len;
    char buf[MAX_BUF];

    len = snprintf(buf, sizeof(buf), SYSFS_GPIO_DIR SYSFS_GPIO_NAME "%d/value", 13);

    fd = open(buf, O_WRONLY);
    if (fd < 0)
    {
        printf("gpio/set-value");
    }
	
    write(fd, "0", 2);
	usleep(10*1000);

    write(fd, "1", 2);
	usleep(10*1000);

    close(fd);
}


/*
*   Name: sensor_write_reg
*            I2C写操作，用于控制sensor寄存器
*
*   Parameters:
*
*            None
*
*   Return:
*            0(正确)
*   Note:
*       无
*/
int sensor_write_reg(void *obj, FH_UINT16 addr, FH_UINT16 data)
{
	int ret = 0;
	unsigned int i = 0;
	static int i2c_fd = -1;
	
	struct i2c_rdwr_ioctl_data i2c_data;
	struct i2c_msg i2c_msg_data = {0};
    unsigned char send_buf[4];

	unsigned int device_addr 	= IMX415_I2C_ADDR;
	unsigned int addr_len 		= 2;
	unsigned int data_len 		= 1;
	
	
	if(i2c_fd < 0)
	{	
		i2c_fd = open(I2C_DEVICE, O_RDWR | O_NONBLOCK);
		if (i2c_fd < 0)
		{
			printf("Error: open /dev/i2c-0 is failed\n");
			return -1;
		}
		ret = ioctl(i2c_fd, I2C_TENBIT, 0);
		if(ret < 0)
		{		
			printf("Error: Unable to set address mode!\n");
			goto err_close;	
		}	
		ret = ioctl(i2c_fd, I2C_SLAVE_FORCE, device_addr);	
		if (ret < 0)
		{		
			printf("Error: Unable to set slave address!\n");
			goto err_close;
		}
	}

	send_buf[i++] = ((addr >> 8) & 0xff);
	send_buf[i++] = (addr & 0xff);
	send_buf[i++] = data & 0xFF;

	i2c_data.nmsgs 			= 1;
	i2c_data.msgs 			= &i2c_msg_data;
	i2c_data.msgs[0].len 	= data_len + addr_len;
	i2c_data.msgs[0].addr 	= device_addr;
	i2c_data.msgs[0].flags 	= 0;
	i2c_data.msgs[0].buf 	= send_buf;

	ret = ioctl(i2c_fd, I2C_RDWR, &i2c_data);
	if(ret < 0)
	{
		printf("Error: ioctl failed in sensor_write_reg!\n");
	}	
	SAFE_CLOSE(i2c_fd); 

	return 0;

err_close:
	SAFE_CLOSE(i2c_fd); 
			
	return -1;
}

/*
*   Name: sensor_write_reg
*            I2C写操作，用于读取sensor寄存器的值
*
*   Parameters:
*
*            None
*
*   Return:
*            0(正确)
*   Note:
*       无
*/
int sensor_read_reg(void *obj, FH_UINT16 addr, FH_UINT16 *data)
{
	int ret = 0;
	static int i2c_fd = -1;
	
	struct i2c_rdwr_ioctl_data i2c_data;
	struct i2c_msg i2c_msg_data[2] = {{0},{0}};
	unsigned char send_buf[4] = {0};
    unsigned char recv_buf[4] = {0};

	unsigned int device_addr 	= IMX415_I2C_ADDR;
	unsigned int addr_len 		= 2;
	unsigned int data_len 		= 1;

	if (i2c_fd < 0)
	{	
		i2c_fd = open(I2C_DEVICE, O_RDWR | O_NONBLOCK);
		if (i2c_fd < 0)
		{
			printf("open /dev/i2c-0 is failed\n");
			goto err_close;
		}
		ret = ioctl(i2c_fd, I2C_TENBIT, 0);	
		if(ret < 0)
		{		
			printf("Error: Unable to set address mode!\n");
			goto err_close;
		}	
		ret = ioctl(i2c_fd, I2C_SLAVE_FORCE, device_addr);	
		if (ret < 0)
		{		
			printf("Error: Unable to set slave address!\n");
			goto err_close;
		}
	}
	
	send_buf[0] = ((addr >> 8) & 0xff);
	send_buf[1] = (addr & 0xFF);

	
	i2c_data.nmsgs 			= 2;
	i2c_data.msgs 			= &i2c_msg_data[0];
	i2c_data.msgs[0].addr 	= device_addr;
	i2c_data.msgs[1].addr 	= device_addr;
	i2c_data.msgs[0].flags 	= 0;
	i2c_data.msgs[1].flags 	= I2C_M_RD;
	i2c_data.msgs[0].len	= addr_len;
	i2c_data.msgs[1].len	= data_len;
	i2c_data.msgs[0].buf   	= send_buf;
	i2c_data.msgs[1].buf   	= recv_buf;
	
	ret = ioctl(i2c_fd, I2C_RDWR, &i2c_data);
	if(ret < 0)
	{
		printf("Error: ioctl failed in sensor_read_reg\n");
		goto err_close;		
	}
	if(1 == data_len)
	{
		*data = recv_buf[0];
	}
	else
	{
		*data = recv_buf[1] | (recv_buf[0] << 8);
	}
	SAFE_CLOSE(i2c_fd);

	return 0;

err_close:
	SAFE_CLOSE(i2c_fd);
			
	return -1;
}

/* 寄存器读写接口封装 */
unsigned short Sensor_Read(unsigned short addr)
{
	int ret = 0;
	unsigned short data = 0;

	sensor_read_reg(0, addr, &data);
	
	return data;
}

void Sensor_Write(unsigned short addr, unsigned short data)
{
	sensor_write_reg(0, addr, data);
}


/*
*   Name: sensor_write_firmware
*            sensor写序列
*
*   Parameters:
*
*            None
*
*   Return:
*            0(正确)
*   Note:
*       无
*/
static void sensor_write_firmware(const unsigned short *rom)
{
	int i = 0;
	int addr = 0;
	int data = 0;
	while (1) 
	{
		addr = rom[2*i];
		data = rom[2*i+1];
		if (addr == 0xFFFE) 
		{
			usleep(data*1000);
		} 
		else if (addr == 0xFFFF) 
		{
			break;
		}
		else 
		{
			sensor_write_reg(0, addr, data);
		}
		i++;
	}
}

//MCLK=24Mhz MIPI 4lane 3840*2160 30FPS 10bit Master Mode 720Mbps/lane
const unsigned short Sensor_Cfg_imx415_Linear[]={

	0x3000, 0x01, //stand by
	0x3002, 0x01,
	0x3008, 0x54,
	0x300A, 0x3B,
	0x3024, 0xCB,  //VMAX 0X08CB=2251  30FPS		 
	0x3028, 0x2A,  //HMAX 0X042A=1066		 
	0x3029, 0x04,
	0x3031, 0x00, 
	0x3032, 0x00,
	0x3033, 0x09,
	0x3050, 0x28,
	0x3051, 0x06,
	0x3090, 0x14,
	0x30C1, 0x00,
	0x3116, 0x23,
	0x3118, 0xB4,
	0x311A, 0xFC,
	0x311E, 0x23,
	0x32D4, 0x21,
	0x32EC, 0xA1,
	0x344C, 0x2B,
	0x344D, 0x01,
	0x344E, 0xED,
	0x344F, 0x01,
	0x3450, 0xF6,
	0x3451, 0x02,
	0x3452, 0x7F,
	0x3453, 0x03,
	0x358A, 0x04,
	0x35A1, 0x02,
	0x35EC, 0x27,
	0x35EE, 0x8D,
	0x35F0, 0x8D,
	0x35F2, 0x29,
	0x36BC, 0x0C,
	0x36CC, 0x53,
	0x36CD, 0x00,
	0x36CE, 0x3C,
	0x36D0, 0x8C,
	0x36D1, 0x00,
	0x36D2, 0x71,
	0x36D4, 0x3C,
	0x36D6, 0x53,
	0x36D7, 0x00,
	0x36D8, 0x71,
	0x36DA, 0x8C,
	0x36DB, 0x00,
	0x3701, 0x00,
	0x3720, 0x00,
	0x3724, 0x02,
	0x3726, 0x02,
	0x3732, 0x02,
	0x3734, 0x03,
	0x3736, 0x03,
	0x3742, 0x03,
	0x3862, 0xE0,
	0x38CC, 0x30,
	0x38CD, 0x2F,
	0x395C, 0x0C,
	0x39A4, 0x07,
	0x39A8, 0x32,
	0x39AA, 0x32,
	0x39AC, 0x32,
	0x39AE, 0x32,
	0x39B0, 0x32,
	0x39B2, 0x2F,
	0x39B4, 0x2D,
	0x39B6, 0x28,
	0x39B8, 0x30,
	0x39BA, 0x30,
	0x39BC, 0x30,
	0x39BE, 0x30,
	0x39C0, 0x30,
	0x39C2, 0x2E,
	0x39C4, 0x2B,
	0x39C6, 0x25,
	0x3A42, 0xD1,
	0x3A4C, 0x77,
	0x3AE0, 0x02,
	0x3AEC, 0x0C,
	0x3B00, 0x2E,
	0x3B06, 0x29,
	0x3B98, 0x25,
	0x3B99, 0x21,
	0x3B9B, 0x13,
	0x3B9C, 0x13,
	0x3B9D, 0x13,
	0x3B9E, 0x13,
	0x3BA1, 0x00,
	0x3BA2, 0x06,
	0x3BA3, 0x0B,
	0x3BA4, 0x10,
	0x3BA5, 0x14,
	0x3BA6, 0x18,
	0x3BA7, 0x1A,
	0x3BA8, 0x1A,
	0x3BA9, 0x1A,
	0x3BAC, 0xED,
	0x3BAD, 0x01,
	0x3BAE, 0xF6,
	0x3BAF, 0x02,
	0x3BB0, 0xA2,
	0x3BB1, 0x03,
	0x3BB2, 0xE0, 
	0x3BB3, 0x03, 
	0x3BB4, 0xE0,
	0x3BB5, 0x03,
	0x3BB6, 0xE0,
	0x3BB7, 0x03,
	0x3BB8, 0xE0,
	0x3BBA, 0xE0,
	0x3BBC, 0xDA,
	0x3BBE, 0x88,
	0x3BC0, 0x44,
	0x3BC2, 0x7B,
	0x3BC4, 0xA2,
	0x3BC8, 0xBD,
	0x3BCA, 0xBD,
	0x4000, 0x10,	//连续模式, 0x11  //LP start in V-Blanking; 
	0x4004, 0x00, 
	0x4005, 0x06,
	0x400C, 0x00,
	0x4018, 0x6F,
	0x401A, 0x2F,
	0x401C, 0x2F,
	0x401E, 0xBF,
	0x401F, 0x00,
	0x4020, 0x2F,
	0x4022, 0x57,
	0x4024, 0x2F,
	0x4026, 0x4F,
	0x4028, 0x27,
	//0x4028, 0x11,
	0x4074, 0x01, 

	0x3000, 0x00,
	0xfffe, 0x64, //delay_ms-100ms
	0x3002, 0x00,
	0xfffe, 0x64,
	0xffff, 0xff,
};

/*
*   Name: sensor_init_imx415
*            sensor驱动初始化，MIPI及序列配置
*
*   Parameters:
*
*            None
*
*   Return:
*            0(正确)
*   Note:
*       无
*/
int sensor_init_imx415(void *obj, Sensor_Init_t* initCfg)
{	
	sensor_set_mipi_imx415();
	sensor_write_firmware(Sensor_Cfg_imx415_Linear);

	return 0;
}

/*
*   Name: sensor_set_mipi_imx415
*            MIPI属性配置
*
*   Parameters:
*
*            None
*
*   Return:
*            0(正确)
*   Note:
*       无
*/
int sensor_set_mipi_imx415()
{
	/** mipi init*/
	struct mipi_conf config = {0};

	config.frq_range = R_1400_1499;//MIPI速率范围
	config.sensor_mode = NON_WDR;//非宽动态模式，linear
	config.raw_type = RAW10;//sensor输出数据位宽
	config.lf_vc_id = 0;//长帧数据通道
	config.sf_vc_id = 0;//短帧数据通道，linear 模式为0
    config.lane_num = 4;//MIPI lane
	
	mipi_init(&config);
	usleep(50*1000);
	
    return 0;
}

/*
*   Name: sensor_set_mirror_flip_imx415
*            ISP镜像
*
*   Parameters:
*
*            0:off 1:mirror 2:flip 3:mirror&flip
*
*   Return:
*            0(正确)
*   Note:
*       无
*/
int sensor_set_mirror_flip_imx415(void *obj, FH_UINT32 sensor_en_stat)
{
	switch(sensor_en_stat)
	{
		case 0://off
			sensor_write_reg(0, 0x3030, 0x0);
			break;
			
		case 1://mirror
			sensor_write_reg(0, 0x3030, 0x01);
			break;
			
		case 2://flip
			sensor_write_reg(0, 0x3030, 0x02);
			break;
			
		case 3://mirror&flip
			sensor_write_reg(0, 0x3030, 0x03);
			break;

		default:
			printf("Error: not support\n");
			break;
	}
	
	return 0;
}


/* 以下为AE回调使用，控制sensor寄存器 */
/*
*   Name: GetIntt
*            获取当前曝光时间，单位行
*
*   Parameters:
*
*            None
*
*   Return:
*            曝光行数
*   Note:
*       无
*/
int GetIntt()
{
    int intt, Reg3050, Reg3051, Reg3052;
	unsigned int intt_invert = 0, VMAX = 0;

	VMAX = Sensor_Read(0x3024);
    VMAX |= Sensor_Read(0x3025)<<8;
    VMAX |= (Sensor_Read(0x3026)&0x0f)<<16;
		
    Reg3050 = ((unsigned int)(Sensor_Read(0x3050))) & 0x000000ff;
    Reg3051 = ((unsigned int)(Sensor_Read(0x3051))) & 0x000000ff;
	Reg3052 = ((unsigned int)(Sensor_Read(0x3052))) & 0x000000ff;
    intt_invert = (Reg3052<<16)|((Reg3051<<8)|Reg3050);
	intt = VMAX - intt_invert;
    return intt;
}

/*
*   Name: SetIntt
*            设置当前曝光时间，单位行
*
*   Parameters:
*
*            intt：曝光行数        fNo：帧数
*
*   Return:
*            0
*   Note:
*       无
*/
int SetIntt(void *obj, FH_UINT32 intt, FH_UINT8 fNo)
{
	unsigned short intt_h, intt_l, intt_hh;
	unsigned int intt_invert = 0, VMAX = 0;
	
    if (fNo == 0) // first frame
	{
		VMAX = Sensor_Read(0x3024);
    	VMAX |= Sensor_Read(0x3025)<<8;
    	VMAX |= (Sensor_Read(0x3026)&0x0f)<<16;
		intt_invert = VMAX - intt;
		intt_l = (unsigned short)(intt_invert&0x000000ff);
        intt_h = (unsigned short)((intt_invert>>8)&0x000000ff);
		intt_hh = (unsigned short)((intt_invert>>16)&0x000000ff);
        
        Sensor_Write(0x3050, intt_l);
        Sensor_Write(0x3051, intt_h);
		Sensor_Write(0x3052, intt_hh);
    }
	
    return 0;
}

/*
*   Name: GetGain
*            获取当前增益
*
*   Parameters:
*
*            Nnoe
*
*   Return:
*            增益值
*   Note:
*       无
*/
int GetGain()
{
    int gain, again, dgain, Reg3090, Reg3091, Reg3092, Reg3093;
	
    Reg3090 = Sensor_Read(0x3090) & 0xff;
    Reg3091 = Sensor_Read(0x3091) & 0xff;
    Reg3092 = Sensor_Read(0x3092) & 0xff;
    Reg3093 = Sensor_Read(0x3093) & 0xff;

    dgain = ((Reg3091 << 8) | Reg3090);  // U.10
    again = ((Reg3093 << 8) | Reg3092);  // U.7

    gain = (dgain * again) >> 11;  // U.17->U.6

    return gain;
}

/*
*   Name: GetAEInfo
*            获取该sensor ae信息
*
*   Parameters:
*
*            Nnoe
*
*   Return:
*            ae info
*   Note:
*       无
*/
int GetAEInfo(void *obj, Sensor_AE_INFO_S *sensAEInfo)
{
    sensAEInfo->currIntt = GetIntt();
    sensAEInfo->currAGain = GetGain();
    sensAEInfo->currHsf = 0X08CB; 
    sensAEInfo->currFrameH = 0X08CB;
	
    return 0;
}

/*
*   Name: GetAEDefault
*            获取该sensor 默认ae参数
*
*   Parameters:
*
*            AE默认参数
*
*   Return:
*            0
*   Note:
*       无
*/
int GetAEDefault(void *obj, Sensor_AE_Default_S *sensAEDefault)
{
    sensAEDefault->minIntt = 2;
    sensAEDefault->maxIntt = 0X08C7;
    sensAEDefault->minAGain = 0x40;
    sensAEDefault->maxAGain = 0x2855ebe;
    sensAEDefault->fullLineStd = 0X08CB;
    sensAEDefault->MinRstLine = 4;

    return 0;
}

//empty function
int CalcSnsValidGain(unsigned int *u32Gain)
{
    return 0;
}

/*
*   Name: SetGain
*            配置sensor增益
*
*   Parameters:
*
*            again：增益      fNo：帧数
*
*   Return:
*            0
*   Note:
*       无
*/
int SetGain(void *obj, FH_UINT32 again, FH_UINT8 fNo) 
{
    unsigned int gain_val;
    int i;
    // again += err_sum;

    for(i=0; i<241; i++)
    {
        if(IMX415RegToGain[i] > again)
            break;
    }
    gain_val = (i - 1);

    Sensor_Write(0x3090, (gain_val&0xff));
    Sensor_Write(0x3091, ((gain_val>>8)&0x7));
    Sensor_Write(0x3092, (gain_val&0xff));
    Sensor_Write(0x3093, ((gain_val>>8)&0x7));

    return 0;
}

//empty function
int sensor_set_fmt_imx415(void *obj, int format)
{
	return 0;
}

//empty function
int sensor_get_attribute_imx415(void *obj, char *name, unsigned int *value)
{
	if (strcmp(name, "WDR") == 0)
    {
        *value = 0;//liner
        return 0;
    }
	return -1;
}

//empty function
int sensor_set_exposure_ratio_imx415(void *obj, FH_UINT32 exposure_ratio)
{
	return 0;
}

//empty function
int sensor_get_exposure_ratio_imx415(void *obj, FH_UINT32 *exposure_ratio)
{
	return 0;
}


#ifdef __cplusplus
#if __cplusplus
extern "C"{
#endif
#endif /* End of #ifdef __cplusplus */

