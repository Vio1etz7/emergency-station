/*
 * 应急物资站 · 事件节点（E53_IS1 人体红外）v2
 * ------------------------------------------------------------
 * 角色：感知节点。把"有人 / 无人"这个状态变化尽快告诉主控，
 *      并在本地自己做一个 5 秒保持的指示灯决策（W2 任务卡 B）。
 *
 * 结构：4 个常驻任务 + 1 个消息队列 + 1 把互斥锁
 *   检测任务  DetectTask  每 10ms 采一次 GPIO_7，稳定 20ms 才认账，变化了丢进队列；
 *                        有人 → 板载 LED 亮，连续 5 秒没人 → 本地灭灯（不依赖网络）
 *   上报任务  NetTask     连 Wi-Fi，把变化 UDP 发给主控 8888；每 10 秒补一条心跳
 *   命令任务  CmdTask     监听 8891，接 CMD=IR,PING=1（主控想知道"现在有人吗"）
 *   状态任务  StatusTask  每 1 秒串口打一行，断网也照打
 *
 * 设计要点：
 *   1) 采集和上报分开：Wi-Fi 连不上时检测照跑，事件先在队列里排队，
 *      连上后以"当前状态"为准补发一次，这就是"断网也能用"的最小实现；
 *   2) 状态不变绝对不重复发：消抖层只报变化，上报层再和"上次发过的值"比一次；
 *   3) 消抖放在任务里而不是 GPIO 中断回调里：中断上下文不能 osDelay，
 *      而消抖本质就是"等 20ms 看还在不在"，必须能等；
 *   4) 心跳（W2）：主控 15 秒收不到消息就判节点掉线，人来人往不频繁的时候
 *      光靠变化上报会被误判，所以每 10 秒发一条 `NODE=IR,P=x,HB=1`；
 *      心跳只是多一个字段，主控按 `P=` 解析，不用改；
 *   5) 组网发现（W2）：上电先广播 WHO=GW 找网关，回包的源地址就是网关地址，
 *      换热点、网关换 IP 都不用改代码重烧；找不到再退回 GATEWAY_IP 宏。
 */

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "ohos_init.h"
#include "cmsis_os2.h"

#include "lwip/sockets.h"
#include "lwip/netifapi.h"

#include "E53_IS1.h"
#include "ir_debounce.h"
#include "local_led.h"
#include "wifi_connect.h"

/* =======================================================
 * 现场要改的只有这两个：热点和"网关"地址
 *   WIFI_SSID / WIFI_PASSWORD：和主控连同一个热点（这里沿用 E1 的热点）
 *   GATEWAY_IP：谁收数据就填谁 ——
 *       接主控板时填主控板的 IP；
 *       还没接主控、先在电脑上用 tools/mock_gateway.py 联调时填电脑的 IP
 *   （下面默认的 192.168.43.234 是 E1 里那台电脑的热点地址，方便先跑通）
 * ======================================================= */
#define WIFI_SSID       "Vio1etz"
#define WIFI_PASSWORD   "12345678"
#define GATEWAY_IP      "192.168.43.131"
#define GATEWAY_PORT    8888            /* 主控收数据的端口，四块板统一 */
#define LOCAL_PORT      8891            /* 本机收命令的端口，红外板固定 8891 */

/* Hi3861 上 1 tick = 10ms（osDelay(100) 约 1 秒，和 E1 一致）。
 * 如果你的工程改过 OS_CFG_TICK_PER_SECOND，只改这一行。 */
#define IR_TICK_MS      10
#define SAMPLE_TICKS    1               /* 每 1 tick 采一次 = 10ms */
#define DEBOUNCE_MS     20              /* 任务卡要求：20ms 消抖 */

#define IR_LOCAL_BEEP   1               /* 检测到人时板载蜂鸣器短响 100ms，0 = 关掉 */

/* W2 任务卡 B */
#define IR_LOCAL_HOLD_MS 5000           /* 连续无人 5 秒，本地指示灯灭 */
#define IR_HB_MS         10000          /* 心跳间隔 10 秒（主控 15 秒判掉线） */

#define STACK_DETECT    8192
#define STACK_STATUS    8192
#define STACK_NET       10240
#define STACK_CMD       8192

#define PRIO_DETECT     25
#define PRIO_STATUS     23
#define PRIO_NET        22
#define PRIO_CMD        21

/* 一次"要上报的事件"：p = 1 有人 / 0 无人；force = 1 表示不看状态是否重复，直接发 */
typedef struct {
    uint8_t p;
    uint8_t force;
} IrEvent_t;

typedef struct {
    uint8_t  present;       /* 当前确认状态：1 有人，0 无人 */
    uint8_t  led_on;        /* 本地指示灯状态（W2：本地决策，不依赖网络） */
    uint8_t  net_up;        /* Wi-Fi + socket 是否就绪 */
    uint32_t detect_cnt;    /* 上电以来"检测到人"的次数 */
    uint32_t report_cnt;    /* 上电以来成功发出的条数 */
} IrState_t;

static osMessageQueueId_t g_evt_q = NULL;
static osMutexId_t        g_lock  = NULL;

static IrState_t g_state = {0};
static int g_fd = -1;       /* UDP socket，NetTask 建好后交给 CmdTask 收命令 */
static uint32_t g_gw_ip = 0;    /* 网关地址（网络字节序）：组网发现成功就用发现的，否则用宏 */

/* ms 换算成 tick，至少 1 个 tick */
static uint32_t MsToTicks(uint32_t ms)
{
    uint32_t ticks = (ms + IR_TICK_MS - 1) / IR_TICK_MS;
    return (ticks == 0) ? 1 : ticks;
}

/* 改本地指示灯：先动硬件，再同步状态（状态任务要打印它） */
static void SetLocalLed(uint8_t on)
{
    LocalLed_Set(on ? 1 : 0);

    osMutexAcquire(g_lock, osWaitForever);
    g_state.led_on = on ? 1 : 0;
    osMutexRelease(g_lock);
}

/* ---------------- 任务 1：检测（GPIO 采样 + 20ms 消抖 + 本地决策） ---------------- */
static void DetectTask(void *arg)
{
    (void)arg;
    IrDebounce_t db;
    IrEvent_t ev;
    uint8_t level = 0;
    uint8_t present;
    uint8_t led_on = 0;
    uint32_t now;
    uint32_t last_present_tick;
    uint32_t stable_ticks = MsToTicks(DEBOUNCE_MS);
    uint32_t hold_ticks   = MsToTicks(IR_LOCAL_HOLD_MS);

    E53_IS1_Init();     /* 里面已经 GpioInit()，并配好 GPIO_7（红外）/ GPIO_8（蜂鸣器） */
    LocalLed_Init();    /* 板载 LED（GPIO_2）只配引脚，不再调 GpioInit，避免把上面两个脚重置 */

    /* 上电先读一次，把当前电平当作初始状态（人正站着也不会漏） */
    E53_IS1_Read_Level(&level);
    IrDebounceInit(&db, level);
    present = level;

    now = osKernelGetTickCount();
    last_present_tick = now;
    if (present) {
        SetLocalLed(1);     /* 有人：本地灯亮，不看网络脸色 */
        led_on = 1;
    }

    osMutexAcquire(g_lock, osWaitForever);
    g_state.present = present;
    osMutexRelease(g_lock);

    printf("[IR] init done, GPIO_7=%d (%s), debounce=%dms, local hold=%dms, hb=%dms\r\n",
           level, level ? "HUMAN" : "NOBODY", DEBOUNCE_MS, IR_LOCAL_HOLD_MS, IR_HB_MS);

    while (1) {
        now = osKernelGetTickCount();
        E53_IS1_Read_Level(&level);

        if (IrDebounceUpdate(&db, level, now, stable_ticks)) {
            present = db.stable;

            osMutexAcquire(g_lock, osWaitForever);
            g_state.present = present;
            if (present) {
                g_state.detect_cnt++;
            }
            osMutexRelease(g_lock);

            printf("[IR] change -> P=%d\r\n", present);

            ev.p = present;
            ev.force = 0;
            if (osMessageQueuePut(g_evt_q, &ev, 0, 0) != osOK) {
                /* 队列满说明积压了 16 次变化，正常场景不会发生；丢一条，
                 * 连上后 NetTask 会补发当前状态，不会让主控一直错下去 */
                printf("[IR] event queue full, drop one\r\n");
            }

            if (present) {
                last_present_tick = now;
                if (!led_on) {
                    SetLocalLed(1);
                    led_on = 1;
                }
#if IR_LOCAL_BEEP
                E53_IS1_Beep(E53_IS1_ON);
                osDelay(10);    /* 100ms 短响：知道板上真的测到了 */
                E53_IS1_Beep(E53_IS1_OFF);
#endif
            }
        }

        /* 本地决策（W2）：连续 5 秒没人就灭灯。
         * 只看本地电平和本地时钟——Wi-Fi 断了、主控没了也照做，这就是"断网自洽"。 */
        if (led_on && (!present) && ((uint32_t)(now - last_present_tick) >= hold_ticks)) {
            SetLocalLed(0);
            led_on = 0;
            printf("[LOCAL] 无人 5 秒，本地灭灯\r\n");
        }

        osDelay(SAMPLE_TICKS);
    }
}

/* ---------------- 组网发现（W2 新增，见 docs/组网发现协议.md） ----------------
 * 换了热点、网关 IP 变了，不用改代码重烧：广播一句 WHO=GW，网关回 GWA=...，
 * 用回包的**源地址**当网关地址（报文里的 IP 只给人看，不解析）。
 * 3 次都失败就退回硬编码的 GATEWAY_IP，串口打一行提示。
 *
 * 和协议文档的差别：文档里先"拿旧地址发一条 HB 看有没有回包"再发现，
 * 但网关对普通上报并不回包，这一步必然白等 10 秒。这里直接发现，失败再回退，
 * 验收标准（换热点不改代码仍能上报）一样满足。
 */
#define DISC_RETRY      3       /* 广播 3 次 */
#define DISC_WAIT_SEC   2       /* 每次最多等 2 秒 */

static int DiscoverGateway(void)
{
    int fd;
    int i;
    int n;
    int ret;
    int yes = 1;
    char buf[64];
    struct sockaddr_in bcast;
    struct sockaddr_in from;
    socklen_t flen;
    fd_set rset;
    struct timeval tv;

    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        return -1;
    }

    /* 广播必须显式打开，否则 sendto 返回 EACCES */
    if (setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &yes, sizeof(yes)) != 0) {
        printf("[DISC] setsockopt SO_BROADCAST failed\r\n");
    }

    memset(&bcast, 0, sizeof(bcast));
    bcast.sin_family      = AF_INET;
    bcast.sin_port        = htons(GATEWAY_PORT);
    bcast.sin_addr.s_addr = htonl(INADDR_BROADCAST);

    for (i = 0; i < DISC_RETRY; i++) {
        sendto(fd, "WHO=GW", 6, 0, (struct sockaddr *)&bcast, sizeof(bcast));

        /* 用 select 等回包，不用 SO_RCVTIMEO：这份 lwIP 默认没开收超时 */
        FD_ZERO(&rset);
        FD_SET(fd, &rset);
        tv.tv_sec  = DISC_WAIT_SEC;
        tv.tv_usec = 0;
        ret = select(fd + 1, &rset, NULL, NULL, &tv);
        if (ret <= 0) {
            continue;
        }

        flen = sizeof(from);
        n = recvfrom(fd, buf, sizeof(buf) - 1, 0,
                     (struct sockaddr *)&from, &flen);
        if (n <= 0) {
            continue;
        }
        buf[n] = 0;

        if (strncmp(buf, "GWA=", 4) == 0) {
            /* 关键：用回包的源地址，不解析报文里的 IP */
            g_gw_ip = from.sin_addr.s_addr;
            printf("[DISC] 找到网关 %s\r\n", inet_ntoa(from.sin_addr));

            closesocket(fd);
            return 0;
        }
    }

    closesocket(fd);
    return -1;
}

/* ---------------- 发一条状态给主控 ---------------- */
static void SendState(int fd, const struct sockaddr_in *addr, uint8_t p, uint8_t hb)
{
    char buf[40];
    int n;

    if (hb) {
        /* 心跳：多一个 HB=1 字段，主控按 P= 解析，不用改 */
        n = snprintf(buf, sizeof(buf), "NODE=IR,P=%d,HB=1\n", p ? 1 : 0);
    } else {
        n = snprintf(buf, sizeof(buf), "NODE=IR,P=%d\n", p ? 1 : 0);
    }
    if (n <= 0) {
        return;
    }

    if (sendto(fd, buf, (size_t)n, 0,
               (const struct sockaddr *)addr, sizeof(*addr)) < 0) {
        printf("[NET] sendto failed\r\n");
        return;
    }

    osMutexAcquire(g_lock, osWaitForever);
    g_state.report_cnt++;
    osMutexRelease(g_lock);

    printf("[NET] -> %s:%d  NODE=IR,P=%d%s\r\n",
           inet_ntoa(addr->sin_addr), GATEWAY_PORT, p ? 1 : 0, hb ? ",HB=1" : "");
}

/* ---------------- 任务 2：联网上报 ---------------- */
static void NetTask(void *arg)
{
    (void)arg;
    int fd;
    uint8_t cur;
    uint8_t last_sent = 0xFF;       /* 0xFF = 还没发过任何一条 */
    IrEvent_t ev;
    struct sockaddr_in addr;
    struct sockaddr_in local;
    uint32_t hb_ticks = MsToTicks(IR_HB_MS);
    uint32_t last_hb;
    uint32_t now;
    uint32_t elapsed;
    uint32_t wait;

    /* 连不上就一直重试，检测任务不受影响 */
    while (WifiConnect(WIFI_SSID, WIFI_PASSWORD) != 0) {
        printf("[NET] wifi connect failed, retry in 5s\r\n");
        osDelay(500);
    }

    /* 组网发现：换热点后不用改代码重烧（见 docs/组网发现协议.md） */
    g_gw_ip = inet_addr(GATEWAY_IP);
    if (DiscoverGateway() != 0) {
        printf("[DISC] 发现失败，用硬编码地址 %s\r\n", GATEWAY_IP);
    }

    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        printf("[NET] socket create failed\r\n");
        return;
    }

    /* 绑 8891：主控/电脑才能把命令发进来 */
    memset(&local, 0, sizeof(local));
    local.sin_family      = AF_INET;
    local.sin_port        = htons(LOCAL_PORT);
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(fd, (struct sockaddr *)&local, sizeof(local)) != 0) {
        printf("[NET] bind %d failed\r\n", LOCAL_PORT);
    }

    g_fd = fd;      /* 交给 CmdTask 收命令 */

    osMutexAcquire(g_lock, osWaitForever);
    g_state.net_up = 1;
    osMutexRelease(g_lock);

    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(GATEWAY_PORT);
    addr.sin_addr.s_addr = g_gw_ip;

    printf("[NET] ready, report to %s:%d, listen on %d\r\n",
           inet_ntoa(addr.sin_addr), GATEWAY_PORT, LOCAL_PORT);

    /* 连上时先补发一次"当前状态"：主控不怕有人站着，就怕不知道现在什么情况。
     * 积压在队列里的旧事件到此为止，以当前状态为准。 */
    while (osMessageQueueGet(g_evt_q, &ev, NULL, 0) == osOK) {
        /* 清空 */
    }
    osMutexAcquire(g_lock, osWaitForever);
    cur = g_state.present;
    osMutexRelease(g_lock);
    SendState(fd, &addr, cur, 0);
    last_sent = cur;
    last_hb   = osKernelGetTickCount();

    while (1) {
        /* 距离上次心跳超过 10 秒就不再等事件，先保证心跳按时发出去 */
        now     = osKernelGetTickCount();
        elapsed = (uint32_t)(now - last_hb);
        wait    = (elapsed >= hb_ticks) ? 0 : (hb_ticks - elapsed);

        if (osMessageQueueGet(g_evt_q, &ev, NULL, wait) == osOK) {
            /* 兜底去重：消抖层只报变化，这一层再挡一次"和上次一样"的重复 */
            if ((!ev.force) && (ev.p == last_sent)) {
                continue;
            }

            SendState(fd, &addr, ev.p, 0);
            last_sent = ev.p;
            continue;
        }

        /* 等到超时还没事件：发心跳。主控 15 秒收不到任何消息就判掉线，
         * 人来人往不频繁的时候全靠这条撑着（W2 任务卡 B）。 */
        osMutexAcquire(g_lock, osWaitForever);
        cur = g_state.present;
        osMutexRelease(g_lock);
        SendState(fd, &addr, cur, 1);
        last_hb = osKernelGetTickCount();
    }
}

/* ---------------- 任务 3：本地状态行（断网也照打） ---------------- */
static void StatusTask(void *arg)
{
    (void)arg;

    while (1) {
        osMutexAcquire(g_lock, osWaitForever);
        printf("[LOCAL] P=%d LED=%d NET=%s DETECT=%lu REPORT=%lu\r\n",
               g_state.present,
               g_state.led_on,
               g_state.net_up ? "UP" : "DOWN",
               (unsigned long)g_state.detect_cnt,
               (unsigned long)g_state.report_cnt);
        osMutexRelease(g_lock);

        osDelay(100);   /* 1 秒 */
    }
}

/* ---------------- 任务 4：收命令（主控 / 电脑下发） ---------------- */
static void CmdTask(void *arg)
{
    (void)arg;
    char buf[64];
    struct sockaddr_in from;
    socklen_t from_len;
    IrEvent_t ev;
    int n;

    /* 等 NetTask 把 socket 建好并绑定端口 */
    while (g_fd < 0) {
        osDelay(50);
    }
    printf("[CMD] listening on udp %d\r\n", LOCAL_PORT);

    while (1) {
        memset(buf, 0, sizeof(buf));
        from_len = sizeof(from);

        /* 没有命令就睡在这里，不占 CPU */
        n = recvfrom(g_fd, buf, sizeof(buf) - 1, 0,
                     (struct sockaddr *)&from, &from_len);
        if (n <= 0) {
            continue;
        }
        buf[n] = 0;

        printf("[CMD] %s:%d => %s\r\n",
               inet_ntoa(from.sin_addr), ntohs(from.sin_port), buf);

        /* 主控想知道"现在到底有没有人"：不管状态变没变，立刻回一条 */
        if ((strstr(buf, "PING") != NULL) || (strstr(buf, "REPORT=1") != NULL)) {
            ev.force = 1;
            osMutexAcquire(g_lock, osWaitForever);
            ev.p = g_state.present;
            osMutexRelease(g_lock);

            if (osMessageQueuePut(g_evt_q, &ev, 0, 0) != osOK) {
                printf("[CMD] queue full, ignore\r\n");
            } else {
                printf("[CMD] report current state on request\r\n");
            }
        } else {
            printf("[CMD] unknown: %s\r\n", buf);
        }
    }
}

/* ---------------- 入口 ---------------- */
static void IrNodeEntry(void)
{
    osThreadAttr_t attr;

    printf("\r\n===== IR Node v2 (E53_IS1, 20ms debounce, hold 5s, hb 10s, udp %d) =====\r\n", LOCAL_PORT);

    g_evt_q = osMessageQueueNew(16, sizeof(IrEvent_t), NULL);
    g_lock  = osMutexNew(NULL);
    if ((g_evt_q == NULL) || (g_lock == NULL)) {
        printf("[ERR] queue/mutex create failed\r\n");
        return;
    }

    memset(&attr, 0, sizeof(attr));
    attr.attr_bits  = 0U;
    attr.cb_mem     = NULL;
    attr.cb_size    = 0U;
    attr.stack_mem  = NULL;

    attr.name       = "DetectTask";
    attr.stack_size = STACK_DETECT;
    attr.priority   = PRIO_DETECT;
    osThreadNew((osThreadFunc_t)DetectTask, NULL, &attr);

    attr.name       = "StatusTask";
    attr.stack_size = STACK_STATUS;
    attr.priority   = PRIO_STATUS;
    osThreadNew((osThreadFunc_t)StatusTask, NULL, &attr);

    attr.name       = "NetTask";
    attr.stack_size = STACK_NET;
    attr.priority   = PRIO_NET;
    osThreadNew((osThreadFunc_t)NetTask, NULL, &attr);

    attr.name       = "CmdTask";
    attr.stack_size = STACK_CMD;
    attr.priority   = PRIO_CMD;
    osThreadNew((osThreadFunc_t)CmdTask, NULL, &attr);
}

APP_FEATURE_INIT(IrNodeEntry);
