/*
 * 20ms 消抖（纯逻辑，不依赖任何 OS / 硬件）
 * ------------------------------------------------------------
 * 为什么单独抽出来：
 *   1) 消抖的正确性可以用 PC 上的脚本验（tools/test_debounce_logic.py），
 *      不用每次都烧板子试；
 *   2) 以后 E4 的倾斜/震动检测也是"电平/状态抖"，换阈值就能复用。
 *
 * 算法（取值必须"连续保持"才算数）：
 *   每次采样拿到的电平和"已确认状态"不同，就记下这次变化的时间和值；
 *   只有同一个新值一直保持到 stable_ticks 之后，才认账并返回 1。
 *   中间只要抖回原值，候选就作废，重新计时。
 *
 * 时间用 RTOS 的 tick 传进来，比较时用无符号减法，
 * 所以 32 位 tick 回绕（约 497 天）也不会误判。
 */

#ifndef __IR_DEBOUNCE_H__
#define __IR_DEBOUNCE_H__

#include <stdint.h>

typedef struct {
    uint8_t  stable;        /* 已确认状态：1 = 有人，0 = 无人 */
    uint8_t  cand;          /* 正在观察的新状态 */
    uint8_t  has_cand;      /* cand 是否有效（没有候选就是 0） */
    uint32_t cand_tick;     /* 新状态第一次被看到的时刻（tick） */
} IrDebounce_t;

/* 上电初始化：把第一次读到的电平当已确认状态 */
void IrDebounceInit(IrDebounce_t *db, uint8_t level);

/* 喂一次采样。返回 1 表示状态已改变并确认，db->stable 是新状态；否则返回 0 */
int IrDebounceUpdate(IrDebounce_t *db, uint8_t level,
                     uint32_t now_tick, uint32_t stable_ticks);

#endif /* __IR_DEBOUNCE_H__ */
