/*
 * 本地指示灯（核心板板载 LED，GPIO_2）
 * ------------------------------------------------------------
 * W2 任务卡 B 要求"网络断了也要能自洽"：红外节点本地自己判断，
 * 有人亮灯、连续 5 秒没人灭灯，全程只依赖 GPIO 和本地时钟。
 *
 * 这块 LED 在 BearPi-HM_Nano 核心板上（官方 B1_basic_led_blink 用的就是 GPIO_2），
 * 不在 E53_IS1 扩展板上，所以单独一个驱动文件，别塞进 E53_IS1.c 里。
 */

#ifndef __LOCAL_LED_H__
#define __LOCAL_LED_H__

#include <stdint.h>

#define LOCAL_LED_GPIO  2   /* WIFI_IOT_IO_NAME_GPIO_2 */
#define LOCAL_LED_FUNC  0   /* WIFI_IOT_IO_FUNC_GPIO_2_GPIO */

/* 只配置引脚，不调 GpioInit()——GPIO 模块由 E53_IS1_Init() 统一初始化，
 * 重复初始化会把已经配好的 GPIO_7 / GPIO_8 冲掉。 */
void LocalLed_Init(void);

/* 1 = 亮，0 = 灭 */
void LocalLed_Set(uint8_t on);

#endif /* __LOCAL_LED_H__ */
