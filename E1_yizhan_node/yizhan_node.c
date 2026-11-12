/*
 * 应急物资站 · 农业节点（单板原型 v1）
 * ------------------------------------------------------------
 * 结构：4 个任务 + 1 个消息队列 + 1 个互斥锁
 *   采集任务  CollectTask  每 1 秒读一次温湿度/光照，丢进队列
 *   控制任务  ControlTask  从队列取数据，判断阈值，驱动风扇/补光灯
 *   上报任务  ReportTask   每 2 秒打印一次（串口，供现场演示）
 *   网络任务  NetTask      连 Wi-Fi 后每 2 秒把数据 UDP 发给电脑
 *
 * 设计要点：网络是独立任务，连不上也不影响采集、控制、打印，
 *          这就是"断网也能用"的雏形。
 */

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "ohos_init.h"
#include "cmsis_os2.h"

#include "E53_IA1.h"
#include "wifi_connect.h"

#include "lwip/sockets.h"
#include "lwip/netifapi.h"
#include <string.h>

/* ======================================================= */
#define WIFI_SSID       "Vio1etz"
#define WIFI_PASSWORD   "12345678"
#define SERVER_IP       "192.168.43.234"   /* 电脑在热点网段的 IP */
#define SERVER_PORT     8888
#define LOCAL_PORT      8889          /* 本机监听端口，电脑往这里发命令 */
#define MANUAL_HOLD     3000          /* 手动模式保持 3000 tick = 30 秒 */
/* ======================================================= */

/* 阈值：超过就动作 */
#define TEMP_HIGH       32.0f
#define HUMI_HIGH       75.0f
#define LUX_LOW         20.0f

#define STACK_COLLECT   8192
#define STACK_CONTROL   8192
#define STACK_REPORT    8192
#define STACK_NET       10240
#define STACK_CMD       8192

#define PRIO_COLLECT    25
#define PRIO_CONTROL    24
#define PRIO_REPORT     23
#define PRIO_NET        22
#define PRIO_CMD        21

typedef struct {
    float temp;
    float humi;
    float lux;
} SensorMsg_t;

/* E53_IA1.c 里只 extern 引用这个全局变量，实体必须由使用者定义
 * （官方 C2 例程是定义在 e53_ia1_example.c 里的，新工程要自己定义） */
E53_IA1_Data_TypeDef E53_IA1_Data;

static osMessageQueueId_t g_queue = NULL;
static osMutexId_t        g_lock  = NULL;   /* 保护 g_latest 与串口打印 */

static SensorMsg_t g_latest = {0};
static int g_motor_on = 0;
static int g_light_on = 0;
static int g_net_ok   = 0;
static int g_fd       = -1;             /* UDP socket，收发共用 */
static volatile uint32_t g_manual_until = 0;   /* 手动模式截止的 tick */

/* ---------------- 任务 1：采集 ---------------- */
static void CollectTask(void *arg)
{
    (void)arg;
    SensorMsg_t m;

    E53_IA1_Init();
    printf("[BOOT] E53_IA1 init done\r\n");

    while (1) {
        E53_IA1_Read_Data();

        m.temp = E53_IA1_Data.Temperature;
        m.humi = E53_IA1_Data.Humidity;
        m.lux  = E53_IA1_Data.Lux;

        /* 队列满就丢掉这一条，不阻塞采集 */
        osMessageQueuePut(g_queue, &m, 0, 0);

        osDelay(100);   /* 100 tick ≈ 1 秒 */
    }
}

/* ---------------- 任务 2：判断与执行 ---------------- */
static void ControlTask(void *arg)
{
    (void)arg;
    SensorMsg_t m;

    while (1) {
        if (osMessageQueueGet(g_queue, &m, NULL, osWaitForever) != osOK) {
            continue;
        }

        /* 手动模式：人在管，自动规则暂时让位（否则刚开的风扇会被下一轮判据关掉） */
        if (osKernelGetTickCount() < g_manual_until) {
            osMutexAcquire(g_lock, osWaitForever);
            g_latest = m;
            osMutexRelease(g_lock);
            continue;
        }

        /* 温度或湿度超限 -> 开风扇 */
        if (m.temp > TEMP_HIGH || m.humi > HUMI_HIGH) {
            Motor_StatusSet(ON);
            g_motor_on = 1;
        } else {
            Motor_StatusSet(OFF);
            g_motor_on = 0;
        }

        /* 光照不足 -> 开补光灯 */
        if (m.lux < LUX_LOW) {
            Light_StatusSet(ON);
            g_light_on = 1;
        } else {
            Light_StatusSet(OFF);
            g_light_on = 0;
        }

        osMutexAcquire(g_lock, osWaitForever);
        g_latest = m;
        osMutexRelease(g_lock);
    }
}

/* ---------------- 任务 3：本地打印（断网也照打） ---------------- */
static void ReportTask(void *arg)
{
    (void)arg;
    SensorMsg_t m;

    while (1) {
        osMutexAcquire(g_lock, osWaitForever);
        m = g_latest;
        printf("[LOCAL] T=%.1fC H=%.1f%% L=%.0flux  FAN=%s LED=%s NET=%s\r\n",
               m.temp, m.humi, m.lux,
               g_motor_on ? "ON" : "OFF",
               g_light_on ? "ON" : "OFF",
               g_net_ok ? "UP" : "DOWN");
        osMutexRelease(g_lock);

        osDelay(200);   /* 2 秒 */
    }
}

/* ---------------- 任务 4：联网上报 ---------------- */
static void NetTask(void *arg)
{
    (void)arg;
    int fd;
    int n;
    char buf[128];
    SensorMsg_t m;
    struct sockaddr_in addr;
    struct sockaddr_in local;

    /* 连不上就一直重试，其他任务不受影响 */
    while (WifiConnect(WIFI_SSID, WIFI_PASSWORD) != 0) {
        printf("[NET] wifi connect failed, retry in 5s\r\n");
        osDelay(500);
    }
    g_net_ok = 1;
    printf("[NET] wifi connected\r\n");

    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        printf("[NET] socket create failed\r\n");
        g_net_ok = 0;
        return;
    }

    /* 绑定本地端口，电脑才能把命令发进来 */
    memset(&local, 0, sizeof(local));
    local.sin_family      = AF_INET;
    local.sin_port        = htons(LOCAL_PORT);
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(fd, (struct sockaddr *)&local, sizeof(local)) != 0) {
        printf("[NET] bind %d failed\r\n", LOCAL_PORT);
    }
    g_fd = fd;      /* 交给 CmdTask 去收命令 */

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(SERVER_PORT);
    addr.sin_addr.s_addr = inet_addr(SERVER_IP);

    while (1) {
        osMutexAcquire(g_lock, osWaitForever);
        m = g_latest;
        n = snprintf(buf, sizeof(buf),
                     "T=%.1f,H=%.1f,L=%.0f,FAN=%d,LED=%d",
                     m.temp, m.humi, m.lux, g_motor_on, g_light_on);
        osMutexRelease(g_lock);

        sendto(fd, buf, n, 0, (struct sockaddr *)&addr, sizeof(addr));
        printf("[NET] -> %s:%d  %s\r\n", SERVER_IP, SERVER_PORT, buf);

        osDelay(200);   /* 2 秒 */
    }
}

/* ---------------- 任务 5：接收命令（电脑/主控板下发） ---------------- */
static void CmdTask(void *arg)
{
    (void)arg;
    char buf[64];
    struct sockaddr_in from;
    socklen_t from_len;
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

        if (strstr(buf, "LED=1") != NULL) {
            Light_StatusSet(ON);
            g_light_on = 1;
        } else if (strstr(buf, "LED=0") != NULL) {
            Light_StatusSet(OFF);
            g_light_on = 0;
        } else if (strstr(buf, "FAN=1") != NULL) {
            Motor_StatusSet(ON);
            g_motor_on = 1;
        } else if (strstr(buf, "FAN=0") != NULL) {
            Motor_StatusSet(OFF);
            g_motor_on = 0;
        } else {
            printf("[CMD] unknown: %s\r\n", buf);
            continue;
        }

        /* 手动动过之后，30 秒内自动规则不抢控制权 */
        g_manual_until = osKernelGetTickCount() + MANUAL_HOLD;
        printf("[CMD] manual mode for %d s\r\n", MANUAL_HOLD / 100);
    }
}

/* ---------------- 入口 ---------------- */
static void YizhanNodeEntry(void)
{
    osThreadAttr_t attr;

    printf("\r\n===== Yizhan Node v2 (5 tasks, cmd ready) =====\r\n");

    g_queue = osMessageQueueNew(8, sizeof(SensorMsg_t), NULL);
    g_lock  = osMutexNew(NULL);
    if (g_queue == NULL || g_lock == NULL) {
        printf("[ERR] queue/mutex create failed\r\n");
        return;
    }

    memset(&attr, 0, sizeof(attr));
    attr.attr_bits  = 0U;
    attr.cb_mem     = NULL;
    attr.cb_size    = 0U;
    attr.stack_mem  = NULL;

    attr.name       = "CollectTask";
    attr.stack_size = STACK_COLLECT;
    attr.priority   = PRIO_COLLECT;
    osThreadNew((osThreadFunc_t)CollectTask, NULL, &attr);

    attr.name       = "ControlTask";
    attr.stack_size = STACK_CONTROL;
    attr.priority   = PRIO_CONTROL;
    osThreadNew((osThreadFunc_t)ControlTask, NULL, &attr);

    attr.name       = "ReportTask";
    attr.stack_size = STACK_REPORT;
    attr.priority   = PRIO_REPORT;
    osThreadNew((osThreadFunc_t)ReportTask, NULL, &attr);

    attr.name       = "NetTask";
    attr.stack_size = STACK_NET;
    attr.priority   = PRIO_NET;
    osThreadNew((osThreadFunc_t)NetTask, NULL, &attr);

    attr.name       = "CmdTask";
    attr.stack_size = STACK_CMD;
    attr.priority   = PRIO_CMD;
    osThreadNew((osThreadFunc_t)CmdTask, NULL, &attr);
}

APP_FEATURE_INIT(YizhanNodeEntry);
