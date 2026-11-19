/*
 * E53_IS1 人体红外扩展板驱动（应急物资站 · 红外节点专用）
 * ------------------------------------------------------------
 * 和官方 C5_e53_is1_infrared 例程的区别：
 *   官方例程只给了"寄存器中断 + 回调"，回调里不能等 20ms，做不了消抖；
 *   本驱动把"读电平"做成可以随时调用的普通函数，消抖交给任务层做
 *   （见 ir_debounce.c）。蜂鸣器接口保持和官方一致的名字，方便对照。
 *
 * 板上固定接线（BearPi-HM_Nano + E53_IS1）：
 *   GPIO_7  热释电模块数字输出：有人 = 高电平，无人 = 低电平
 *   GPIO_8  板载蜂鸣器，复用成 PWM1_OUT
 */

#ifndef __E53_IS1_H__
#define __E53_IS1_H__

#include <stdint.h>

/* 下面是海思 Hi3861 的引脚编号 / 复用功能号 / PWM 端口号。
 * 写成数字是为了不依赖 wifiiot_*.h 里枚举的具体名字（不同 OpenHarmony 版本
 * 命名不一样，E1 主控用的是 wifiiot_* 那一代），注释里给出对应枚举名。 */
#define E53_IS1_PIR_GPIO        7   /* WIFI_IOT_IO_NAME_GPIO_7 */
#define E53_IS1_PIR_FUNC        0   /* WIFI_IOT_IO_FUNC_GPIO_7_GPIO */
#define E53_IS1_BEEP_GPIO       8   /* WIFI_IOT_IO_NAME_GPIO_8 */
#define E53_IS1_BEEP_FUNC       5   /* WIFI_IOT_IO_FUNC_GPIO_8_PWM1_OUT */
#define E53_IS1_BEEP_PWM_PORT   1   /* WIFI_IOT_PWM_PORT_PWM1 */

/* PwmStart(port, duty, freq) 的语义（见 SDK 的 hi_pwm.h）：
 *   占空比 = duty / freq，输出频率 = 时钟源频率 / freq
 * 官方 C5 例程用的是 20000/40000：占空比 50%，160MHz/40000 = 4kHz，
 * 正好是无源蜂鸣器的谐振点。这里照抄，保证这块板响得出来。 */
#define E53_IS1_BEEP_DUTY       20000
#define E53_IS1_BEEP_FREQ       40000

/***************************************************************
* 名    称: E53_IS1_Status_ENUM
* 说    明: 蜂鸣器开关状态
***************************************************************/
typedef enum
{
    E53_IS1_OFF = 0,
    E53_IS1_ON  = 1
} E53_IS1_Status_ENUM;

/* 初始化 GPIO_7（输入）和 GPIO_8（PWM 输出） */
void E53_IS1_Init(void);

/* 读一次红外电平：*level = 1 有人，0 无人。读失败按 0 处理 */
void E53_IS1_Read_Level(uint8_t *level);

/* 蜂鸣器开关 */
void E53_IS1_Beep(E53_IS1_Status_ENUM status);

#endif /* __E53_IS1_H__ */
