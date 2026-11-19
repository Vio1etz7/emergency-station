/*
 * 20ms 消抖（纯逻辑，不依赖任何 OS / 硬件）
 * 算法说明见 ir_debounce.h。
 */

#include <stddef.h>

#include "ir_debounce.h"

/***************************************************************
* 函数名称: IrDebounceInit
* 说    明: 上电初始化，把当前电平当作"已确认状态"
***************************************************************/
void IrDebounceInit(IrDebounce_t *db, uint8_t level)
{
    if (db == NULL) {
        return;
    }

    db->stable    = (level != 0) ? 1 : 0;
    db->cand      = db->stable;
    db->has_cand  = 0;
    db->cand_tick = 0;
}

/***************************************************************
* 函数名称: IrDebounceUpdate
* 说    明: 喂一次采样，判断状态是否已稳定变化
* 参    数: db           消抖状态
*           level        本次采到的电平（0/1）
*           now_tick     本次采样时刻（RTOS tick）
*           stable_ticks 需要连续保持的 tick 数（20ms -> 2 tick）
* 返 回 值: 1 = 状态变化已确认；0 = 没变化 / 还在消抖
***************************************************************/
int IrDebounceUpdate(IrDebounce_t *db, uint8_t level,
                     uint32_t now_tick, uint32_t stable_ticks)
{
    uint8_t v;

    if (db == NULL) {
        return 0;
    }

    v = (level != 0) ? 1 : 0;

    /* 抖回原值：候选作废，重新计时 */
    if (v == db->stable) {
        db->has_cand = 0;
        return 0;
    }

    /* 第一次看到这个新值，或者新值又变了：重新开始计时 */
    if ((db->has_cand == 0) || (db->cand != v)) {
        db->cand      = v;
        db->cand_tick = now_tick;
        db->has_cand  = 1;
        return 0;
    }

    /* 新值已经连续保持了足够久，认账 */
    if ((uint32_t)(now_tick - db->cand_tick) >= stable_ticks) {
        db->stable   = v;
        db->has_cand = 0;
        return 1;
    }

    return 0;
}
