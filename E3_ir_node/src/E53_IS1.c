/*
 * E53_IS1 人体红外扩展板驱动
 * ------------------------------------------------------------
 * 只做三件事：初始化两个引脚、读 GPIO_7 电平、控制 GPIO_8 上的蜂鸣器。
 * 参考官方例程 C5_e53_is1_infrared（E53_IS1_Init / E53_IS1_Read_Data /
 * Beep_StatusSet），差别见 E53_IS1.h 顶部注释。
 */

#include <stdio.h>
#include <unistd.h>

#include "E53_IS1.h"
#include "wifiiot_gpio.h"
#include "wifiiot_gpio_ex.h"
#include "wifiiot_pwm.h"

/***************************************************************
* 函数名称: E53_IS1_IO_Init
* 说    明: 引脚初始化
*           GPIO_7 设为输入（读红外电平）
*           GPIO_8 复用为 PWM1_OUT 并设为输出（驱动蜂鸣器）
***************************************************************/
static void E53_IS1_IO_Init(void)
{
    GpioInit();

    IoSetFunc(E53_IS1_PIR_GPIO, E53_IS1_PIR_FUNC);      /* GPIO_7 当普通 GPIO 用 */
    GpioSetDir(E53_IS1_PIR_GPIO, WIFI_IOT_GPIO_DIR_IN); /* 输入：读模块输出 */
    IoSetPull(E53_IS1_PIR_GPIO, WIFI_IOT_IO_PULL_UP);   /* 上拉，和官方例程一致
                                                         * （板子没插好时不至于读到漂浮电平） */

    IoSetFunc(E53_IS1_BEEP_GPIO, E53_IS1_BEEP_FUNC);      /* GPIO_8 复用为 PWM1_OUT */
    GpioSetDir(E53_IS1_BEEP_GPIO, WIFI_IOT_GPIO_DIR_OUT); /* 输出：推蜂鸣器 */
    PwmInit(E53_IS1_BEEP_PWM_PORT);
}

/***************************************************************
* 函数名称: E53_IS1_Init
* 说    明: 初始化 E53_IS1
***************************************************************/
void E53_IS1_Init(void)
{
    E53_IS1_IO_Init();
}

/***************************************************************
* 函数名称: E53_IS1_Read_Level
* 说    明: 读一次人体红外电平（不消抖，消抖在任务层做）
* 参    数: level，输出，1 = 有人，0 = 无人
***************************************************************/
void E53_IS1_Read_Level(uint8_t *level)
{
    WifiIotGpioValue val = WIFI_IOT_GPIO_VALUE0;

    if (level == NULL) {
        return;
    }

    if (GpioGetInputVal(E53_IS1_PIR_GPIO, &val) != 0) {
        *level = 0;     /* 读失败按“无人”处理，下个周期还会再读 */
        return;
    }

    *level = (val == WIFI_IOT_GPIO_VALUE1) ? 1 : 0;
}

/***************************************************************
* 函数名称: E53_IS1_Beep
* 说    明: 蜂鸣器开关（PWM 驱动无源蜂鸣器）
* 参    数: status，E53_IS1_ON 响，E53_IS1_OFF 停
***************************************************************/
void E53_IS1_Beep(E53_IS1_Status_ENUM status)
{
    if (status == E53_IS1_ON) {
        PwmStart(E53_IS1_BEEP_PWM_PORT, E53_IS1_BEEP_DUTY, E53_IS1_BEEP_FREQ);
    }

    if (status == E53_IS1_OFF) {
        PwmStop(E53_IS1_BEEP_PWM_PORT);
    }
}
