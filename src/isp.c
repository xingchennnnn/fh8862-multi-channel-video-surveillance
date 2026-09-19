#ifdef __cplusplus
#if __cplusplus
extern "C"{
#endif
#endif /* End of #ifdef __cplusplus */


#include "isp_common.h"
#include "sensor.h"

/*
*   Name: isp_set_ae
*            ae使能
*
*   Parameters:
*
*            0:disable 1:enable
*
*   Return:
*            0(正确), �?(错误)
*   Note:
*       �?*/
int isp_set_ae(int ae_val)
{
	int ret;
	AE_DEFAULT_CFG stAeDefaultCfg;

	if(0 != ae_val && 1 != ae_val){
		printf("Error: ae_val is %d, out of [0, 1]\n", ae_val);
		return -1;
	}
	
	//ae enable
	ret = API_ISP_GetAeDefaultCfg(0, &stAeDefaultCfg);
	stAeDefaultCfg.stAeEnableCfg.bAecEn = ae_val;
	ret |= API_ISP_SetAeDefaultCfg(0, &stAeDefaultCfg);

	return ret;
}

/*
*   Name: isp_set_awb
*            awb使能
*
*   Parameters:
*
*            0:disable 1:enable
*
*   Return:
*            0(正确), �?(错误)
*   Note:
*       �?*/
int isp_set_awb(int awb_val)
{
	int ret;
	AWB_DEFAULT_CFG stAwbDefaultCfg;

	if(0 != awb_val && 1 != awb_val){
		printf("Error: awb_val is %d, out of [0, 1]\n", awb_val);
		return -1;
	}
	
	//awb enable
	ret = API_ISP_GetAwbDefaultCfg(0, &stAwbDefaultCfg);
	stAwbDefaultCfg.stAwbEnableCfg.bAwbEn = awb_val;
	ret |= API_ISP_SetAwbDefaultCfg(0, &stAwbDefaultCfg);

	return ret;
}

/*
*   Name: isp_set_saturation
*            色彩配置
*
*   Parameters:
*
*            sat_val 饱和度等级，0-255
*
*   Return:
*            0(正确), �?(错误)
*   Note:
*       �?*/
int isp_set_saturation(int sat_val)
{
	int ret;
	int i = 0;
	ISP_SAT_CFG stCeCfg;

	if(1 > sat_val || 255 < sat_val){
		printf("Error: sat_val is %d, out of [0, 255]\n", sat_val);
		return -1;
	}
	
	ret = API_ISP_GetSaturation(0, &stCeCfg);
	for(i = 0; i < 12; i++)
	{
		stCeCfg.u08SatMap[i] = (unsigned char)sat_val;
		printf("sat[%d] = %d, ", i, stCeCfg.u08SatMap[i]);
	}
	stCeCfg.u08Sat = (unsigned char)sat_val;
	printf("\n sat = %d\n ",stCeCfg.u08Sat);
	ret |= API_ISP_SetSaturation(0, &stCeCfg);

	return ret;
}

/*
*   Name: isp_set_bright
*            亮度配置
*
*   Parameters:
*
*            bright_val 亮度等级�?-255
*
*   Return:
*            0(正确), �?(错误)
*   Note:
*       �?*/
int isp_set_bright(int bright_val)
{
	int ret;
	int i = 0;
	ISP_BRIGHTNESS_CFG stBrightnessCfg;

	if(0 > bright_val || 255 < bright_val){
		printf("Error: bright_val is %d, out of [0, 255]\n", bright_val);
		return -1;
	}

	bright_val -= 128;//[-128, 127]
	
	ret = API_ISP_GetBrightnessCfg(0, &stBrightnessCfg);
	for(i = 0; i < 12; i++)
	{
		stBrightnessCfg.s08BrtMap[i] = (char)bright_val;
	}
	stBrightnessCfg.s08Brt = (char)bright_val;
	ret |= API_ISP_SetBrightnessCfg(0, &stBrightnessCfg);
	
	return ret;
}

/*
*   Name: isp_set_nr
*            降噪配置
*
*   Parameters:
*
*            0:disable 1:enable
*
*   Return:
*            0(正确), �?(错误)
*   Note:
*       �?*/
int isp_set_nr(int nr_val)
{
	int ret;
	ISP_YNR_CFG stYnrCfg;
	ISP_NR3D_CFG stNr3dCfg;

	if(0 != nr_val && 1 != nr_val){
		printf("Error: nr_val is %d, out of [0, 1]\n", nr_val);
		return -1;
	}

	ret = API_ISP_GetYnrCfg(0, &stYnrCfg);
	stYnrCfg.bYnrEn = nr_val;
	ret |= API_ISP_SetYnrCfg(0, &stYnrCfg);
	
	ret = API_ISP_GetNr3dCfg(0, &stNr3dCfg);
	stNr3dCfg.bNr3dEn = nr_val;
	ret |= API_ISP_SetNr3dCfg(0, &stNr3dCfg);

	return ret;
}

/*
*   Name: isp_set_mirrorflip
*            镜像功能配置
*
*   Parameters:
*
*            0:off 1:mirror 2:flip 3:mirror&flip
*
*   Return:
*            0(正确), �?(错误)
*   Note:
*       �?*/
int isp_set_mirrorflip(int mf_val)
{
	int ret;
	FH_BOOL mirror;
	FH_BOOL flip;
	FH_UINT32 bayer;

	printf("0:off, 1:mirror, 2:flip, 3:mirror&flip\n");

	ret = API_ISP_GetMirrorAndflip(0, &mirror, &flip);
	
	if(0 > mf_val || 4 < mf_val){
		printf("Error: mf_val is %d, out of [0, 4]\n", mf_val);
		return -1;
	}

	if(0 == mf_val)
	{
		mirror = 0;
		flip = 0;
	}
	else if(1 == mf_val)
	{
		mirror = 1;
		flip = 0;
	}
	else if(2 == mf_val)
	{
		mirror = 0;
		flip = 1;
	}
	else if(3 == mf_val)
	{
		mirror = 1;
		flip = 1;
	}

	ret |= API_ISP_SetMirrorAndflip(0, mirror, flip);
	
	return ret;
}

/*
*   Name: isp_server_run
*            ISP运行线程
*
*   Parameters:
*
*            None
*
*   Return:
*            None
*   Note:
*       �?*/
static int g_isp_stop = 0;
static struct dev_isp_info isp_info;

FH_VOID *isp_proc(FH_VOID *arg)
{
	int ret = 0;
    (void)arg;
    char name[20];

    isp_info.running = 1;
    while(!g_isp_stop)
    {
       ret = API_ISP_Run(0);
	   if(0 != ret)
	   	{
	   		printf("API_ISP_Run error with ret = 0x%x\n",ret);
	   	}
        usleep(10000);
    }
	
    isp_info.bStop = 0;
	isp_info.running = 0;
    return NULL;
}

/*
*   Name: isp_server_run
*            ISP运行线程创建
*
*   Parameters:
*
*            None
*
*   Return:
*            0(正确), �?(错误)
*   Note:
*       �?*/
FH_SINT32 isp_server_run()
{
    FH_UINT32 ret;
    pthread_t isp_thread;
    pthread_attr_t attr;
    struct sched_param param;

    g_isp_stop = 0;
    memset(&isp_info, 0, sizeof(isp_info));
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&attr, 10 * 1024);
    pthread_attr_setschedparam(&attr, &param);
    ret = pthread_create(&isp_thread, &attr, isp_proc, NULL);
    if (ret != 0)
    {
       printf("Error: create ISP thread failed!\n");
       return -1;
    }
	
    return ret;
}

void isp_server_stop(void)
{
    int timeout = 0;

    g_isp_stop = 1;

    /* 轮询等待 ISP 线程退出 (设置 running=0)，超时 2 秒 */
    while (isp_info.running && timeout < 200)
    {
        usleep(10000);
        timeout++;
    }

    if (timeout >= 200)
    {
        printf("[ISP] stop timeout, force exit\n");
    }
}

#ifdef __cplusplus
#if __cplusplus
extern "C"{
#endif
#endif /* End of #ifdef __cplusplus */

