/*
 * 应急物资站 · 事件节点（E53_IS1 人体红外）v1
 * ------------------------------------------------------------
 * 角色：感知节点。只干一件事——把"有人 / 无人"这个状态变化，尽快告诉主控。
 *
 * 结构：4 个常驻任务 + 1 个消息队列 + 1 把互斥锁
 *   检测任务  DetectTask  每 10ms 采一次 GPIO_7，稳定 20ms 才认账，变化了丢进队列
 *   上报任务  NetTask     连 Wi-Fi，把变化 UDP 发给主控 8888 端口
 *   命令任务  CmdTask     监听 8891，接 CMD=IR,PING=1（主控想知道"现在有人吗"）
 *   状态任务  StatusTask  每 1 秒串口打一行，断网也照打
 *
 * 设计要点：
 *   1) 采集和上报分开：Wi-Fi 连不上时检测照跑，事件先在队列里排队，
 *      连上后以"当前状态"为准补发一次，这就是"断网也能用"的最小实现；
 *   2) 状态不变绝对不重复发：消抖层只报变化，上报层再和"上次发过的值"比一次；
 *   3) 消抖放在任务里而不是 GPIO 中断回调里：中断上下文不能 osDelay，
 *      而消抖本质就是"等 20ms 看还在不在"，必须能等。
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
#define GATEWAY_IP      "192.168.43.234"
#define GATEWAY_PORT    8888            /* 主控收数据的端口，四块板统一 */
#define LOCAL_PORT      8891            /* 本机收命令的端口，红外板固定 8891 */

/* Hi3861 上 1 tick = 10ms（osDelay(100) 约 1 秒，和 E1 一致）。
 * 如果你的工程改过 OS_CFG_TICK_PER_SECOND，只改这一行。 */
#define IR_TICK_MS      10
#define SAMPLE_TICKS    1               /* 每 1 tick 采一次 = 10ms */
#define DEBOUNCE_MS     20              /* 任务卡要求：20ms 消抖 */

#define IR_LOCAL_BEEP   1               /* 检测到人时板载蜂鸣器短响 100ms，0 = 关掉 */

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
    uint8_t  net_up;        /* Wi-Fi + socket 是否就绪 */
    uint32_t detect_cnt;    /* 上电以来"检测到人"的次数 */
    uint32_t report_cnt;    /* 上电以来成功发出的条数 */
} IrState_t;

static osMessageQueueId_t g_evt_q = NULL;
static osMutexId_t        g_lock  = NULL;

static IrState_t g_state = {0};
static int g_fd = -1;       /* UDP socket，NetTask 建好后交给 CmdTask 收命令 */

/* ms 换算成 tick，至少 1 个 tick */
static uint32_t MsToTicks(uint32_t ms)
{
    uint32_t ticks = (ms + IR_TICK_MS - 1) / IR_TICK_MS;
    return (ticks == 0) ? 1 : ticks;
}

/* ---------------- 任务 1：检测（GPIO 采样 + 20ms 消抖） ---------------- */
static void DetectTask(void *arg)
{
    (void)arg;
    IrDebounce_t db;
    IrEvent_t ev;
    uint8_t level = 0;
    uint8_t p;
    uint32_t stable_ticks = MsToTicks(DEBOUNCE_MS);

    E53_IS1_Init();

    /* 上电先读一次，把当前电平当作初始状态（人正站着也不会漏） */
    E53_IS1_Read_Level(&level);
    IrDebounceInit(&db, level);

    osMutexAcquire(g_lock, osWaitForever);
    g_state.present = level;
    osMutexRelease(g_lock);

    printf("[IR] init done, GPIO_7=%d (%s), debounce=%dms\r\n",
           level, level ? "HUMAN" : "NOBODY", DEBOUNCE_MS);

    while (1) {
        E53_IS1_Read_Level(&level);

        if (IrDebounceUpdate(&db, level, osKernelGetTickCount(), stable_ticks)) {
            p = db.stable;

            osMutexAcquire(g_lock, osWaitForever);
            g_state.present = p;
            if (p) {
                g_state.detect_cnt++;
            }
            osMutexRelease(g_lock);

            printf("[IR] change -> P=%d\r\n", p);

            ev.p = p;
            ev.force = 0;
            if (osMessageQueuePut(g_evt_q, &ev, 0, 0) != osOK) {
                /* 队列满说明积压了 16 次变化，正常场景不会发生；丢一条，
                 * 连上后 NetTask 会补发当前状态，不会让主控一直错下去 */
                printf("[IR] event queue full, drop one\r\n");
            }

#if IR_LOCAL_BEEP
            if (p) {
                E53_IS1_Beep(E53_IS1_ON);
                osDelay(10);    /* 100ms 短响：知道板上真的测到了 */
                E53_IS1_Beep(E53_IS1_OFF);
            }
#endif
        }

        osDelay(SAMPLE_TICKS);
    }
}

/* ---------------- 发一条状态给主控 ---------------- */
static void SendState(int fd, const struct sockaddr_in *addr, uint8_t p)
{
    char buf[32];
    int n;

    n = snprintf(buf, sizeof(buf), "NODE=IR,P=%d\n", p ? 1 : 0);
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

    printf("[NET] -> %s:%d  NODE=IR,P=%d\r\n", GATEWAY_IP, GATEWAY_PORT, p ? 1 : 0);
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

    /* 连不上就一直重试，检测任务不受影响 */
    while (WifiConnect(WIFI_SSID, WIFI_PASSWORD) != 0) {
        printf("[NET] wifi connect failed, retry in 5s\r\n");
        osDelay(500);
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
    printf("[NET] ready, report to %s:%d, listen on %d\r\n",
           GATEWAY_IP, GATEWAY_PORT, LOCAL_PORT);

    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(GATEWAY_PORT);
    addr.sin_addr.s_addr = inet_addr(GATEWAY_IP);

    /* 连上时先补发一次"当前状态"：主控不怕有人站着，就怕不知道现在什么情况。
     * 积压在队列里的旧事件到此为止，以当前状态为准。 */
    while (osMessageQueueGet(g_evt_q, &ev, NULL, 0) == osOK) {
        /* 清空 */
    }
    osMutexAcquire(g_lock, osWaitForever);
    cur = g_state.present;
    osMutexRelease(g_lock);
    SendState(fd, &addr, cur);
    last_sent = cur;

    while (1) {
        if (osMessageQueueGet(g_evt_q, &ev, NULL, osWaitForever) != osOK) {
            continue;
        }

        /* 兜底去重：消抖层只报变化，这一层再挡一次"和上次一样"的重复 */
        if ((!ev.force) && (ev.p == last_sent)) {
            continue;
        }

        SendState(fd, &addr, ev.p);
        last_sent = ev.p;
    }
}

/* ---------------- 任务 3：本地状态行（断网也照打） ---------------- */
static void StatusTask(void *arg)
{
    (void)arg;

    while (1) {
        osMutexAcquire(g_lock, osWaitForever);
        printf("[LOCAL] P=%d NET=%s DETECT=%lu REPORT=%lu\r\n",
               g_state.present,
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

    printf("\r\n===== IR Node v1 (E53_IS1, 20ms debounce, udp %d) =====\r\n", LOCAL_PORT);

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
