#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""20ms 消抖逻辑自测（PC 上跑，不用板子）

这里把 src/ir_debounce.c 的算法等价抄了一份：逻辑改了，两边一起改。
它验证的是"设计对不对"（20ms 才认账、毛刺滤掉、不重复报、tick 回绕不误判）；
板子上的 C 代码是否编译通过、GPIO 是否真的读到，还得靠编译 + 烧录验。

运行：python tools/test_debounce_logic.py
"""

import sys

STABLE_TICKS = 2        # 20ms / 10ms per tick


class Debounce(object):
    """与 IrDebounce_t / IrDebounceUpdate 一一对应"""

    def __init__(self, level):
        self.stable = 1 if level else 0
        self.cand = self.stable
        self.has_cand = False
        self.cand_tick = 0

    def update(self, level, now_tick):
        v = 1 if level else 0
        if v == self.stable:
            self.has_cand = False
            return 0
        if (not self.has_cand) or (self.cand != v):
            self.cand = v
            self.cand_tick = now_tick
            self.has_cand = True
            return 0
        if (now_tick - self.cand_tick) & 0xFFFFFFFF >= STABLE_TICKS:
            self.stable = v
            self.has_cand = False
            return 1
        return 0


def run(levels, start_tick=0, init_level=0):
    """levels：每个 tick 的电平序列；返回 [(tick, 新状态), ...]"""
    db = Debounce(init_level)
    events = []
    for i, level in enumerate(levels):
        t = (start_tick + i) & 0xFFFFFFFF
        if db.update(level, t):
            events.append((i, db.stable))
    return events


CASES = []


def case(name):
    def deco(fn):
        CASES.append((name, fn))
        return fn
    return deco


@case("稳定 20ms 才认账：第 3 个采样点（t=2）才报")
def _():
    events = run([1, 1, 1, 1, 1])
    assert events == [(2, 1)], events
    return "events=%s" % events


@case("10ms 毛刺被滤掉：只高一拍不报")
def _():
    events = run([1, 0, 0, 0])
    assert events == [], events
    return "events=%s" % events


@case("抖动 0-1-0-1-0 不报")
def _():
    events = run([1, 0, 1, 0, 1, 0, 0, 0])
    assert events == [], events
    return "events=%s" % events


@case("有人持续不重复报：只报 1 次")
def _():
    events = run([1] * 50)
    assert events == [(2, 1)], events
    return "events=%s" % events


@case("有人->无人：两次变化各报一次，各滞后 20ms（2 拍）")
def _():
    #   t: 0 1 2 3 4 5
    #   P: 1 1 1 0 0 0
    # 高电平在 t=0 出现，t=2 满 20ms 认账；低电平 t=3 出现，t=5 认账
    levels = [1, 1, 1, 0, 0, 0]
    events = run(levels)
    assert events == [(2, 1), (5, 0)], events
    return "events=%s" % events


@case("上电就有人：不误报，人走才报 P=0")
def _():
    events = run([1, 1, 1, 0, 0, 0], init_level=1)
    assert events == [(5, 0)], events
    return "events=%s" % events


@case("32 位 tick 回绕：不误判、不丢事件")
def _():
    start = 0xFFFFFFFF - 3
    events = run([1, 1, 1, 1], start_tick=start)
    assert events == [(2, 1)], events
    return "start=0x%X events=%s" % (start, events)


@case("20ms 抖动窗内回到低电平：不算一次有人")
def _():
    # t=0 高一拍就回低（t=1），t=2 起才真正持续变高；如果按单次采样判，t=0 就误报了
    events = run([1, 0, 1, 1, 1])
    assert events == [(4, 1)], events
    return "events=%s" % events


def main():
    failed = 0
    for name, fn in CASES:
        try:
            detail = fn()
            print("PASS  %s    (%s)" % (name, detail))
        except AssertionError as exc:
            failed += 1
            print("FAIL  %s    -> %s" % (name, exc))
    print("")
    print("%d/%d passed" % (len(CASES) - failed, len(CASES)))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
