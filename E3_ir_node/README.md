# E3 红外事件节点（E53_IS1 人体红外）

任务卡 B：**检测有人 / 无人，带 20ms 消抖，状态一变就上报一次，不变就闭嘴。**

这块板在系统里只干一件事：把"有人 / 无人"这个状态变化，尽快告诉主控。
它不做判断、不点灯、不报警——报警是主控的事（R1：有人 → 灯亮）。

---

## 一、和主控的约定（和 `docs/分工与任务卡.md` 第二节一致）

| 项 | 值 |
|---|---|
| 上报目的地 | 主控 `8888` 端口（`GATEWAY_IP`） |
| 上报报文 | `NODE=IR,P=1`（有人）、`NODE=IR,P=0`（无人），一行 ASCII，`\n` 结尾 |
| 本机监听 | `8891`，接收命令 |
| 支持的指令 | `CMD=IR,PING=1` → 立刻回一条当前状态（不管状态变没变） |

`PING` 是给主控/答辩用的：主控想知道"**现在**到底有没有人"，不用等下一次变化。

---

## 二、代码结构

```
E3_ir_node/
├── ir_node.c              主程序：4 个任务 + 队列 + 互斥锁
├── BUILD.gn               编译脚本
├── include/
│   ├── E53_IS1.h          红外板驱动接口（GPIO_7 读电平、GPIO_8 蜂鸣器）
│   ├── ir_debounce.h      20ms 消抖（纯逻辑，可 PC 端验）
│   └── wifi_connect.h     连 Wi-Fi
├── src/
│   ├── E53_IS1.c          驱动实现
│   ├── ir_debounce.c      消抖实现
│   └── wifi_connect.c     Wi-Fi 连接（官方例程改：失败返回错误码，不死等）
├── tools/
│   ├── mock_gateway.py    PC 端假网关：没有主控板也能联调
│   └── test_debounce_logic.py  消抖逻辑自测（PC 上跑，不用板子）
└── README.md
```

四个任务，职责单一，互不阻塞：

| 任务 | 优先级 | 栈 | 干什么 |
|---|---|---|---|
| `DetectTask` | 25 | 8K | 每 10ms 读一次 GPIO_7，稳定 20ms 才认账，变化了丢进队列 |
| `StatusTask` | 23 | 8K | 每秒打一行串口状态，断网也照打 |
| `NetTask` | 22 | 10K | 连 Wi-Fi、绑 8891、把队列里的事件 UDP 发给主控 |
| `CmdTask` | 21 | 8K | `recvfrom` 阻塞等命令，收到 `PING` 让 NetTask 回一条 |

为什么这么分：**采集和上报分开，Wi-Fi 连不上时检测照跑**，事件先排队；
连上后以"当前状态"为准补发一次。这就是"断网也能用"在节点侧的最小实现。

---

## 三、硬件与引脚

E53_IS1 扩展板插在 BearPi-HM_Nano 上，接线是固定的：

| 引脚 | 用途 | 电平含义 |
|---|---|---|
| `GPIO_7` | 热释电模块数字输出（输入） | **高 = 有人**，低 = 无人 |
| `GPIO_8` | 板载蜂鸣器（复用 PWM1_OUT） | 本工程只用它短响 100ms 表示"测到了" |

蜂鸣器不是任务要求，是给现场调试/答辩用的：挥手时板子"嘀"一声，不用盯串口也知道
传感器工作正常。不想要就把 `ir_node.c` 里的 `IR_LOCAL_BEEP` 改成 `0`。

---

## 四、20ms 消抖怎么做的

采样周期 **10ms**（`SAMPLE_TICKS = 1`，1 tick = 10ms），消抖窗口 **20ms**（2 拍）：

```
t(ms)   0    10   20   30   40
电平    1    1    1    0    0
        ↑              ↑
     看到高电平     满 20ms 才认账 -> 报 P=1
                          ↑
                    电平变低，重新计时
```

规则：新电平必须**连续保持 2 拍**（20ms）才认账；中间抖回原值就作废、重新计时。
效果：

- 10ms 的毛刺不会误报（`tools/test_debounce_logic.py` 里有用例）；
- 状态稳定后**绝不重复发**，上报层还会再和"上次发过的值"比一次，双重保险；
- 从人出现到报文发出，延迟约 20~30ms，比 PIR 本身 2 秒左右的保持时间小两个数量级。

**为什么不像官方例程那样用 GPIO 中断回调？**
中断上下文里不能 `osDelay`，而消抖本质就是"等 20ms 看电平还在不在"，必须能等。
改成任务里轮询后，20ms 窗口、去重、上报全在一处，逻辑清楚也好测。
官方例程的 `E53_IS1_Init / E53_IS1_Read_Data / Beep_StatusSet` 名字和接线都保留了，
对照着看不会有落差，只是 `Read_Data(回调)` 换成了 `Read_Level(&level)`。

---

## 五、编译与烧录

> 仓库是 OpenHarmony 工程的一个子目录，**单独编不了**，必须放进工程里编。

1. 把 `emergency-station` 放在 `applications/BearPi/BearPi-HM_Nano/sample/` 下
   （`docs/开发环境与Git协作.md` 第四节已经写过）。

2. 改 `applications/BearPi/BearPi-HM_Nano/sample/BUILD.gn`，**只留一行没注释的**：

   ```gn
   features = [
       #"emergency-station/E1_yizhan_node:yizhan_node",
       "emergency-station/E3_ir_node:ir_node",
   ]
   ```

   ⚠️ 这个文件属于 OpenHarmony 源码，**本地改，不要提交**。

3. 编译、烧录：

   ```bash
   cd ~/project/bearpi-hm_nano
   python build.py BearPi-HM_Nano
   ```

   HiBurn 选 `out/BearPi-HM_Nano/Hi3861_wifiiot_app_allinone.bin`，
   串口 **921600**，烧完按 RESET。

4. 上电前先改 `ir_node.c` 顶部这几个宏：

   ```c
   #define WIFI_SSID     "Vio1etz"          // 和主控同一个热点
   #define WIFI_PASSWORD "12345678"
   #define GATEWAY_IP    "192.168.43.234"   // 主控板实际分到的 IP
   ```

   `LOCAL_PORT` 固定 `8891`，不要改（四块板端口必须不同）。

---

## 六、烧完之后串口该看到什么

```
===== IR Node v1 (E53_IS1, 20ms debounce, udp 8891) =====
[IR] init done, GPIO_7=0 (NOBODY), debounce=20ms
[LOCAL] P=0 NET=DOWN DETECT=0 REPORT=0
[WIFI] scan ...
WiFi connect succeed!
<-- DHCP state:OK -->
[NET] ready, report to 192.168.43.234:8888, listen on 8891
[NET] -> 192.168.43.234:8888  NODE=IR,P=0
[LOCAL] P=0 NET=UP DETECT=0 REPORT=1
[CMD] listening on udp 8891
```

手在传感器前挥一下：

```
[IR] change -> P=1                 ← 20ms 确认后立刻出现
[NET] -> 192.168.43.234:8888  NODE=IR,P=1
[LOCAL] P=1 NET=UP DETECT=1 REPORT=2
```

手移开（PIR 自己会保持 2 秒左右，然后掉下来）：

```
[IR] change -> P=0
[NET] -> 192.168.43.234:8888  NODE=IR,P=0
```

状态不变的时候：**只会每秒一行 `[LOCAL]`，不会有 `[NET] ->`**。这就是"不重复发"。

---

## 七、联调（三种情况各怎么测）

### 情况 1：手上只有这一块板，主控还没好 —— 用 PC 假网关

电脑和开发板连同一个 Wi-Fi，电脑上：

```bash
python tools/mock_gateway.py
# 监听 udp 0.0.0.0:8888，等节点上报……
# [20:48:55] 192.168.43.50:8891  NODE=IR,P=1    <= 红外: 有人 P=1
```

Windows 第一次会弹防火墙，点**允许**。
把 `GATEWAY_IP` 改成电脑的 IP（`ipconfig` 看 IPv4 地址）。

### 情况 2：想测命令通道（8891）

```bash
python tools/mock_gateway.py --node 192.168.43.50
# 回车 = 发 CMD=IR,PING=1，板子会立刻回一条 NODE=IR,P=...
```

或者只发一条就退出：

```bash
python tools/mock_gateway.py --node 192.168.43.50 --send "CMD=IR,PING=1"
```

### 情况 3：接主控板

1. 主控（E1）那边要能收 `NODE=IR,...`（任务卡 A）；
2. `GATEWAY_IP` 填主控的 IP；
3. 主控串口应能看到 `P=1` / `P=0`，主控据此点灯（R1：有人 → 灯亮）。

---

## 八、验收对照（任务卡 B）

| 任务卡要求 | 本工程怎么满足 | 怎么验 |
|---|---|---|
| 检测有人 / 无人 | `GPIO_7` 高 = 有人 | 串口 `[LOCAL] P=` 跟着挥手变 |
| 带 20ms 消抖 | 10ms 采样 + 20ms 确认（`ir_debounce.c`） | `python tools/test_debounce_logic.py` 8/8 通过；毛刺不报 |
| 状态变化才上报 | 消抖层只报变化 + 上报层比对上次值 | 人站着不动，串口没有新的 `[NET] ->` |
| `NODE=IR,P=1` / `P=0` | `SendState()` 严格按约定拼报文 | 假网关收到的原文一致 |
| 手挥一下 → 主控出现 `P=1` | 检测到后 ~20ms 入队、立刻发 | 挥手，看主控/假网关 |
| 手移开 2 秒后 `P=0` | PIR 保持结束 → 低电平稳定 20ms | 手移开，等 2~3 秒 |

---

## 九、卡住了看这里

| 现象 | 先查什么 |
|---|---|
| 串口一行都没有 | 烧进去了吗？`sample/BUILD.gn` 是不是还开着 E1？波特率 921600？ |
| 一直 `NET=DOWN` | SSID 拼错 / 密码错 / 热点是 5GHz（Hi3861 只支持 2.4GHz）/ 热点没开 |
| `ssid "xxx" not found` 反复刷 | 热点没开，或离得太远。程序会自动每 5 秒重试，不影响检测 |
| 挥手有 `[IR] change` 但主控收不到 | `GATEWAY_IP` 填的是主控的 IP 吗？两台在同一网段吗？主控 8888 端口起了吗？ |
| 挥手完全没反应 | 扩展板插紧了吗？PIR 上电要 30 秒左右预热；`[LOCAL] P=` 有没有跟着变 |
| 一直 `P=1` 不回落 | PIR 模块上的延时电位器拧太长；人走了要等 2~3 秒 |
| 编译报 `wifiiot_pwm.h` / `PwmStart` 找不到 | 这套 SDK 把 PWM 挪位置了：注释掉 `E53_IS1.c` 里 PWM 的 include 和 3 处调用，再把 `IR_LOCAL_BEEP` 改 0，红外主功能不受影响 |

---

## 十、已知限制 / 下一步（留给 W4）

1. **运行中 Wi-Fi 掉线不会自动重连**：只在启动时重试。要稳，得在 `NetTask` 里
   加"连续发失败 N 次 → 重新 `WifiConnect`"，但这会牵动 `CmdTask` 正在阻塞的
   socket，属于 W4 的活儿。
2. **没有本地缓存补传**：断网期间只保留"当前状态"，中间的变化不排队补发
   （对"现在有没有人"这个语义来说，补发旧事件反而是错的）。
3. 蜂鸣器只做 100ms 本地提示，不做 3 秒报警——报警是主控的职责，节点不抢戏。

---

## 十一、PC 端自测（不用板子）

```bash
python tools/test_debounce_logic.py     # 消抖逻辑 8 个用例
python tools/mock_gateway.py            # 假网关，测网络和报文格式
```

> `test_debounce_logic.py` 是 `src/ir_debounce.c` 的等价 Python 实现，
> 验证的是**算法设计**（20ms 才认账、毛刺滤掉、不重复报、tick 回绕不误判）；
> C 代码能不能编过、GPIO 读得对不对，还得靠编译 + 烧录。
