/*
 * 本地指示灯（核心板板载 LED，GPIO_2）实现
 */

#include "local_led.h"
#include "wifiiot_gpio.h"
#include "wifiiot_gpio_ex.h"

/***************************************************************
* 函数名称: LocalLed_Init
* 说    明: 把 GPIO_2 配成普通输出，默认熄灭
* 注    意: 调用前 GPIO 模块必须已初始化（E53_IS1_Init() 里调过 GpioInit）
***************************************************************/
void LocalLed_Init(void)
{
    IoSetFunc(LOCAL_LED_GPIO, LOCAL_LED_FUNC);                  /* 复用为普通 GPIO */
    GpioSetDir(LOCAL_LED_GPIO, WIFI_IOT_GPIO_DIR_OUT);          /* 输出 */
    GpioSetOutputVal(LOCAL_LED_GPIO, WIFI_IOT_GPIO_VALUE0);     /* 上电先灭 */
}

/***************************************************************
* 函数名称: LocalLed_Set
* 说    明: 点亮 / 熄灭本地指示灯
* 参    数: on，1 亮，0 灭
***************************************************************/
void LocalLed_Set(uint8_t on)
{
    GpioSetOutputVal(LOCAL_LED_GPIO,
                     on ? WIFI_IOT_GPIO_VALUE1 : WIFI_IOT_GPIO_VALUE0);
}
