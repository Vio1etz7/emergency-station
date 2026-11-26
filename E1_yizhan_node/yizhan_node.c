/*
 * 应急物资站 · 主控 / 网关（v3）
 * ------------------------------------------------------------
 * 在 v2（单板闭环 + 电脑下发命令）的基础上，加了"当家长"的两件事：
 *   ① 收：监听 8888，接所有子节点的上报
 *   ② 发：按规则算出该谁动作，把命令发到该节点的端口
 *
 * 七个任务，一个任务只管一件事：
 *   1 CollectTask  IA1 采集温湿度/光照 → 队列
 *   2 ControlTask  取出数据，管 IA1 自己的风扇和补光灯
 *   3 ReportTask   每 2 秒打印一行总览（断网也照打）
 *   4 NetTask      连 Wi-Fi，建两个 UDP socket（8888 收节点、8889 收电脑命令）
 *   5 GwTask       收节点报文 → 解析 → 更新状态 → 立即触发规则
 *   6 RuleTask     每秒巡查：超时关灯、环境告警联动、节点掉线检测
 *   7 CmdTask      收电脑下发的命令，控制 IA1 本地的灯和风扇
 *
 * 端口约定（和 docs/分工与任务卡.md 一致）：
 *   8888 主控收节点上报      8889 主控收电脑命令
 *   8890 路灯   8891 红外   8892 物流
 *
 * 说明：节点 IP 不写死，从收到的报文里自动学（recvfrom 的源地址）。
 *      所以哪块板先上线、IP 是多少，都不用改代码。
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

#include "ohos_init.h"
#include "cmsis_os2.h"

#include "E53_IA1.h"
#include "wifi_connect.h"

#include "lwip/sockets.h"
#include "lwip/netifapi.h"

/* ======================= 配置区（改这里） ======================= */
#define WIFI_SSID       "Vio1etz"
#define WIFI_PASSWORD   "12345678"

#define GW_DATA_PORT    8888        /* 收子节点上报 */
#define GW_CMD_PORT     8889        /* 收电脑 / 手机下发的命令 */
/* ============================================================== */

/* 阈值 */
#define TEMP_HIGH       32.0f
#define HUMI_HIGH       75.0f
#define LUX_LOW         20.0f
#define MANUAL_HOLD     3000        /* 手动模式保持 3000 tick = 30 秒 */
#define LIGHT_HOLD      3000        /* 有人走后，灯再亮 30 秒 */
#define ALARM_HOLD      1000        /* 包裹告警保持 10 秒，之后自动解除 */
#define BOX_MOVE_METERS 20.0        /* 物资偏离基准点多少米算异常（与 E4 阈值保持一致） */
#define BOX_FIX_LOST    3000        /* 3000 tick = 30 秒收不到有效定位算异常 */
#define NODE_TIMEOUT    1500        /* 1500 tick = 15 秒没消息算掉线 */
#define MAX_NODES       4

#define STACK_COLLECT   8192
#define STACK_CONTROL   8192
#define STACK_REPORT    4096
#define STACK_NET       10240
#define STACK_GW        8192
#define STACK_RULE      4096
#define STACK_CMD       4096

#define PRIO_COLLECT    25
#define PRIO_CONTROL    24
#define PRIO_REPORT     23
#define PRIO_NET        22
#define PRIO_GW         21
#define PRIO_RULE       20
#define PRIO_CMD        19

/* ---------------- 数据结构 ---------------- */
typedef struct {
    float temp;
    float humi;
    float lux;
} SensorMsg_t;

/* 一块子节点的档案 */
typedef struct {
    char     name[8];       /* IR / LAMP / BOX */
    uint32_t ip;
    uint16_t port;
    uint32_t last_tick;     /* 最后一次收到消息的时间 */
    int      online;
} NodeInfo_t;

/* E53_IA1.c 里只 extern 引用这个全局变量，实体必须由使用者定义
 * （官方 C2 例程是定义在 e53_ia1_example.c 里的，新工程要自己定义） */
E53_IA1_Data_TypeDef E53_IA1_Data;

static osMessageQueueId_t g_queue = NULL;
static osMutexId_t        g_lock  = NULL;   /* 保护下面这一堆共享变量 */

static SensorMsg_t g_latest  = {0};
static int   g_motor_on  = 0;       /* IA1 自己的风扇 */
static int   g_light_on  = 0;       /* IA1 自己的补光灯 */
static int   g_net_ok    = 0;

static int   g_node_fd   = -1;      /* 绑 8888，收节点上报、发命令给节点 */
static int   g_cmd_fd    = -1;      /* 绑 8889，收电脑命令 */
static volatile uint32_t g_manual_until = 0;

/* 从子节点学到的状态 */
static NodeInfo_t g_nodes[MAX_NODES];
static float g_lamp_lux    = -1.0f;   /* 路灯报上来的光照，-1 表示还不知道 */
static int   g_ir_present = 0;      /* 红外：有人 = 1 */
static int   g_box_alarm  = 0;      /* 物流：偏离基准点 / 定位丢失 */
static int32_t  g_box_base_lat = 0;             /* 基准点纬度（E6 定点） */
static int32_t  g_box_base_lon = 0;             /* 基准点经度（E6 定点） */
static int      g_box_base_set = 0;             /* 基准点是否已记录 */
static uint32_t g_box_fix_lost_since = 0;       /* 从什么时候开始收不到有效定位 */
static double   g_box_last_dist = 0.0;          /* 最近一次算出的偏离距离，仅用于打印 */
static volatile uint32_t g_box_alarm_until = 0;   /* 告警什么时候自动解除 */
static int   g_buzz_on    = 0;      /* 当前是否已让物流节点蜂鸣 */
static int   g_light_req  = 0;      /* 是否已经让路灯开灯 */
static volatile uint32_t g_light_until = 0;
static char  g_last_cmd[64] = "none";   /* 最近发出去的一条命令，只用于打印 */

/* ---------------- 小工具 ---------------- */

/* 从 "NODE=LAMP,L=156,LED=0" 里取 L 的值 */
static int GetField(const char *buf, const char *key, char *out, int outsz)
{
    char pat[12];
    const char *p;
    int i = 0;

    snprintf(pat, sizeof(pat), "%s=", key);
    p = strstr(buf, pat);
    if (p == NULL) {
        return 0;
    }
    p += strlen(pat);
    while (*p != 0 && *p != ',' && *p != '\r' && *p != '\n' && i < outsz - 1) {
        out[i++] = *p++;
    }
    out[i] = 0;
    return 1;
}

/* 取节点名："NODE=" 之后到 ',' 之前 */
static void GetNodeName(const char *buf, char *out, int outsz)
{
    const char *p = buf;
    int i = 0;

    if (strncmp(p, "NODE=", 5) == 0) {
        p += 5;
    }
    while (*p != 0 && *p != ',' && *p != '\r' && *p != '\n' && i < outsz - 1) {
        out[i++] = *p++;
    }
    out[i] = 0;
}

/* 找节点；没有就登记一个新节点（IP、端口从报文来源学） */
static int NodeFindOrAdd(const char *name, uint32_t ip, uint16_t port)
{
    int i;
    int empty = -1;

    for (i = 0; i < MAX_NODES; i++) {
        if (g_nodes[i].name[0] != 0 && strcmp(g_nodes[i].name, name) == 0) {
            g_nodes[i].ip        = ip;
            g_nodes[i].port      = port;
            g_nodes[i].last_tick = osKernelGetTickCount();
            g_nodes[i].online    = 1;
            return i;
        }
        if (g_nodes[i].name[0] == 0 && empty < 0) {
            empty = i;
        }
    }

    if (empty < 0) {
        return -1;
    }
    strncpy(g_nodes[empty].name, name, sizeof(g_nodes[empty].name) - 1);
    g_nodes[empty].ip        = ip;
    g_nodes[empty].port      = port;
    g_nodes[empty].last_tick = osKernelGetTickCount();
    g_nodes[empty].online    = 1;
    printf("[GW] 新节点上线: %s %s:%d\r\n",
           g_nodes[empty].name, inet_ntoa(*(struct in_addr *)&ip), port);
    return empty;
}

/* 按名字给节点发一条命令 */
static int SendCmdTo(const char *node_name, const char *cmd)
{
    struct sockaddr_in to;
    int i;
    int n;

    for (i = 0; i < MAX_NODES; i++) {
        if (g_nodes[i].name[0] != 0 && strcmp(g_nodes[i].name, node_name) == 0) {
            if (g_node_fd < 0) {
                return -1;
            }
            memset(&to, 0, sizeof(to));
            to.sin_family      = AF_INET;
            to.sin_port        = htons(g_nodes[i].port);
            to.sin_addr.s_addr = g_nodes[i].ip;

            n = sendto(g_node_fd, cmd, strlen(cmd), 0,
                       (struct sockaddr *)&to, sizeof(to));
            if (n < 0) {
                printf("[GW] 发命令失败 -> %s\r\n", node_name);
                return -1;
            }
            printf("[GW] -> %s:%d  %s\r\n", node_name, g_nodes[i].port, cmd);

            osMutexAcquire(g_lock, osWaitForever);
            strncpy(g_last_cmd, cmd, sizeof(g_last_cmd) - 1);
            osMutexRelease(g_lock);
            return 0;
        }
    }
    printf("[GW] 节点 %s 不在线，命令丢弃: %s\r\n", node_name, cmd);
    return -1;
}

/* ---------------- 任务 1：采集（IA1 本地） ---------------- */
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

        osMessageQueuePut(g_queue, &m, 0, 0);   /* 队列满就丢，不阻塞采集 */
        osDelay(100);                            /* 100 tick ≈ 1 秒 */
    }
}

/* ---------------- 任务 2：IA1 自己的冷暖/补光 ---------------- */
static void ControlTask(void *arg)
{
    (void)arg;
    SensorMsg_t m;

    while (1) {
        if (osMessageQueueGet(g_queue, &m, NULL, osWaitForever) != osOK) {
            continue;
        }

        /* 手动模式：人在管，自动规则让位 30 秒 */
        if (osKernelGetTickCount() < g_manual_until) {
            osMutexAcquire(g_lock, osWaitForever);
            g_latest = m;
            osMutexRelease(g_lock);
            continue;
        }

        if (m.temp > TEMP_HIGH || m.humi > HUMI_HIGH) {
            Motor_StatusSet(ON);
            g_motor_on = 1;
        } else {
            Motor_StatusSet(OFF);
            g_motor_on = 0;
        }

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

/* ---------------- 任务 3：总览打印（断网也照打） ---------------- */
static void ReportTask(void *arg)
{
    (void)arg;
    SensorMsg_t m;
    int i;

    while (1) {
        int online = 0;     /* 必须在循环里清零，否则每轮都累加（v3 的 bug） */
        osMutexAcquire(g_lock, osWaitForever);
        m = g_latest;
        for (i = 0; i < MAX_NODES; i++) {
            if (g_nodes[i].name[0] != 0 && g_nodes[i].online) {
                online++;
            }
        }
        printf("[GW] T=%.1fC H=%.1f%% L=%.0flux FAN=%d LED=%d NET=%s\r\n",
               m.temp, m.humi, m.lux, g_motor_on, g_light_on,
               g_net_ok ? "UP" : "DOWN");
        printf("[GW]    红外=%s  路灯照明=%.0flux  节点在线=%d/3  BOX=%s(%.0fm) last=%s\r\n",
               g_ir_present ? "有人" : "无人",
               g_lamp_lux,
               online,
               g_box_alarm ? "告警" : "正常",
               g_box_last_dist,
               g_last_cmd);
        osMutexRelease(g_lock);

        osDelay(200);   /* 2 秒 */
    }
}

/* ---------------- 任务 4：联网 + 建两个 socket ---------------- */
static void NetTask(void *arg)
{
    (void)arg;
    struct sockaddr_in local;

    while (WifiConnect(WIFI_SSID, WIFI_PASSWORD) != 0) {
        printf("[NET] wifi connect failed, retry in 5s\r\n");
        osDelay(500);
    }
    g_net_ok = 1;
    printf("[NET] wifi connected\r\n");

    /* socket A：收子节点上报，也用来给子节点发命令 */
    g_node_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (g_node_fd < 0) {
        printf("[NET] socket A failed\r\n");
        g_net_ok = 0;
        return;
    }
    memset(&local, 0, sizeof(local));
    local.sin_family      = AF_INET;
    local.sin_port        = htons(GW_DATA_PORT);
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(g_node_fd, (struct sockaddr *)&local, sizeof(local)) != 0) {
        printf("[NET] bind %d failed\r\n", GW_DATA_PORT);
    }

    /* socket B：收电脑下发的手动命令 */
    g_cmd_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (g_cmd_fd < 0) {
        printf("[NET] socket B failed\r\n");
        g_net_ok = 0;
        return;
    }
    memset(&local, 0, sizeof(local));
    local.sin_family      = AF_INET;
    local.sin_port        = htons(GW_CMD_PORT);
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(g_cmd_fd, (struct sockaddr *)&local, sizeof(local)) != 0) {
        printf("[NET] bind %d failed\r\n", GW_CMD_PORT);
    }

    printf("[NET] 网关就绪：收数据 %d，收命令 %d\r\n", GW_DATA_PORT, GW_CMD_PORT);

    /* 这个任务建完 socket 就没事了，留着维持状态，不退出 */
    while (1) {
        osDelay(500);
    }
}

/* 两个 GPS 坐标之间的直线距离（米）。
 * 局部平面近似：纬度差直接换算成米，经度差再乘 cos(平均纬度)。
 * 不用 libm 的 cos/sqrt —— Hi3861 链接器不继承数学库（E4 也是这么绕开的）。 */
static double GwDistanceMeters(int32_t lat1, int32_t lon1, int32_t lat2, int32_t lon2)
{
    const double rad_per_e6 = 3.14159265358979323846 / 180000000.0;   /* E6 定点 -> 弧度 */
    const double earth_m = 6371000.0;
    double mean_lat = ((double)lat1 + (double)lat2) * 0.5 * rad_per_e6;
    double squared = mean_lat * mean_lat;
    double term = 1.0;
    double cosine = 1.0;
    double north;
    double east;
    double scale;
    double a;
    double b;
    double sum;
    double root;
    int i;

    /* cos 的泰勒展开，校园尺度（±90°内）够用，最多 8 项 */
    for (i = 1; i <= 8; i++) {
        term *= -squared / ((double)(2 * i - 1) * (double)(2 * i));
        cosine += term;
    }
    if (cosine < 0.0) {
        cosine = 0.0;
    }

    north = ((double)lat2 - (double)lat1) * rad_per_e6 * earth_m;
    east  = ((double)lon2 - (double)lon1) * rad_per_e6 * earth_m * cosine;
    if (north < 0.0) {
        north = -north;
    }
    if (east < 0.0) {
        east = -east;
    }

    scale = (north > east) ? north : east;
    if (scale == 0.0) {
        return 0.0;
    }
    /* 先归一化再用牛顿迭代开方，结果与位移大小无关 */
    a = north / scale;
    b = east / scale;
    sum = a * a + b * b;
    root = 1.0;
    for (i = 0; i < 8; i++) {
        root = 0.5 * (root + sum / root);
    }
    return scale * root;
}

/* ---------------- 任务 5：收节点报文，立刻按规则反应 ---------------- */
static void GwTask(void *arg)
{
    (void)arg;
    char buf[128];
    char name[8];
    char val[16];
    struct sockaddr_in from;
    socklen_t from_len;
    int n;
    int idx;
    uint32_t now;

    while (g_node_fd < 0) {
        osDelay(50);
    }
    printf("[GW] listening on udp %d\r\n", GW_DATA_PORT);

    while (1) {
        memset(buf, 0, sizeof(buf));
        from_len = sizeof(from);
        n = recvfrom(g_node_fd, buf, sizeof(buf) - 1, 0,
                     (struct sockaddr *)&from, &from_len);
        if (n <= 0) {
            continue;
        }
        buf[n] = 0;

        /* ---------- 组网发现：节点广播 WHO=GW 来找网关 ----------
         * 子节点不知道网关 IP，就向 255.255.255.255:8888 广播一句 WHO=GW。
         * 网关收到后回一条 GWA=<本机IP>,<端口>，节点从回包里直接取源地址。
         * 这类报文不进节点表，也不参与规则判断。 */
        if (strncmp(buf, "WHO=GW", 6) == 0) {
            struct sockaddr_in me;
            socklen_t me_len = sizeof(me);
            int probe;
            char reply[48];
            char peer_str[16];
            char me_str[16];
            int rn;

            memset(&me, 0, sizeof(me));
            /* 直接 getsockname 拿到的是 0.0.0.0（bind 时用的 INADDR_ANY），
             * 所以借一个临时 socket 连一下对端，再看自己被分配到哪个地址。 */
            probe = socket(AF_INET, SOCK_DGRAM, 0);
            if (probe >= 0) {
                if (connect(probe, (struct sockaddr *)&from, from_len) == 0) {
                    me_len = sizeof(me);
                    getsockname(probe, (struct sockaddr *)&me, &me_len);
                }
                closesocket(probe);   /* lwIP 的 socket 不是 fd，没有 close() */
            }
            /* 注意：inet_ntoa 返回的是静态缓冲区，同一个 printf 里调两次，
             * 两个参数会指向同一块内存，打印出来是两个相同的地址。
             * 所以先各自拷到局部数组里再用。 */
            strncpy(peer_str, inet_ntoa(from.sin_addr), sizeof(peer_str) - 1);
            peer_str[sizeof(peer_str) - 1] = 0;
            strncpy(me_str, inet_ntoa(me.sin_addr), sizeof(me_str) - 1);
            me_str[sizeof(me_str) - 1] = 0;

            rn = snprintf(reply, sizeof(reply), "GWA=%s,%d\r\n",
                          me_str, GW_DATA_PORT);
            sendto(g_node_fd, reply, rn, 0, (struct sockaddr *)&from, from_len);
            printf("[GW] 发现请求 <- %s:%d，已回复 GWA=%s\r\n",
                   peer_str, ntohs(from.sin_port), me_str);
            continue;
        }

        GetNodeName(buf, name, sizeof(name));
        if (name[0] == 0) {
            printf("[GW] 看不懂的报文: %s\r\n", buf);
            continue;
        }

        printf("[GW] <- %s:%d  %s\r\n",
               inet_ntoa(from.sin_addr), ntohs(from.sin_port), buf);

        osMutexAcquire(g_lock, osWaitForever);
        idx = NodeFindOrAdd(name, from.sin_addr.s_addr, ntohs(from.sin_port));
        osMutexRelease(g_lock);
        if (idx < 0) {
            printf("[GW] 节点表已满，忽略 %s\r\n", name);
            continue;
        }

        now = osKernelGetTickCount();

        /* ---------- 规则 1：红外有人 + 环境暗 -> 让路灯开灯 ---------- */
        if (strcmp(name, "IR") == 0) {
            int was_present = g_ir_present;
            if (GetField(buf, "P", val, sizeof(val))) {
                g_ir_present = atoi(val);
            }
            if (g_ir_present == 1) {
                if ((g_lamp_lux < LUX_LOW) || (g_lamp_lux < 0)) {
                    if (g_light_req == 0) {
                        printf("[RULE] 有人且偏暗 -> 开路灯\r\n");
                        if (SendCmdTo("LAMP", "CMD=LIGHT,LED=1") == 0) {
                            g_light_req = 1;
                        }
                    }
                }
                g_light_until = now + LIGHT_HOLD;   /* 有人就一直续期 */
            } else if (was_present == 1) {
                /* 只在"有人 -> 无人"这一刻开始计时。
                 * 红外每 2 秒都发一次 P=0，如果每收到一条就续期，灯永远关不掉。 */
                g_light_until = now + LIGHT_HOLD;
                printf("[RULE] 人走了 -> 30 秒后关路灯\r\n");
            }
        }

        /* ---------- 路灯上报光照 ---------- */
        if (strcmp(name, "LAMP") == 0) {
            if (GetField(buf, "L", val, sizeof(val))) {
                g_lamp_lux = (float)atoi(val);
            }
        }

        /* ---------- 规则 2：物资偏离基准点 / 定位丢失 -> 现场蜂鸣 ---------- */
        if (strcmp(name, "BOX") == 0) {
            int fix = 0;
            int32_t lat = 0;
            int32_t lon = 0;
            double dist = 0.0;

            if (GetField(buf, "FIX", val, sizeof(val))) {
                fix = atoi(val);
            }
            if (GetField(buf, "LAT_E6", val, sizeof(val))) {
                lat = (int32_t)atol(val);
            }
            if (GetField(buf, "LON_E6", val, sizeof(val))) {
                lon = (int32_t)atol(val);
            }

            if ((fix == 1) && (lat != 0) && (lon != 0)) {
                g_box_fix_lost_since = 0;       /* 定位恢复了 */

                if (g_box_base_set == 0) {
                    /* 第一次拿到有效定位，就地记为基准点 */
                    g_box_base_lat = lat;
                    g_box_base_lon = lon;
                    g_box_base_set = 1;
                    printf("[RULE] 物资基准点已记录 LAT_E6=%d LON_E6=%d\r\n",
                           (int)lat, (int)lon);
                } else {
                    dist = GwDistanceMeters(g_box_base_lat, g_box_base_lon, lat, lon);
                    g_box_last_dist = dist;
                    if (dist > BOX_MOVE_METERS) {
                        if (g_box_alarm == 0) {
                            /* 只在 正常->告警 这一刻打印，之后每秒只会续期，不刷屏 */
                            printf("[RULE] 物资偏离基准点 %.0f 米（阈值 %.0f）-> 蜂鸣 + 路灯常亮\r\n",
                                   dist, BOX_MOVE_METERS);
                        }
                        g_box_alarm = 1;
                        g_box_alarm_until = now + ALARM_HOLD;   /* 每秒一条，超阈值就自动续期 */
                        g_light_until = now + LIGHT_HOLD;
                        if (g_buzz_on == 0) {
                            if (SendCmdTo("BOX", "CMD=BUZZ,ON=1") == 0) {
                                g_buzz_on = 1;
                            }
                        }
                        if (g_light_req == 0) {
                            if (SendCmdTo("LAMP", "CMD=LIGHT,LED=1") == 0) {
                                g_light_req = 1;
                            }
                        }
                    }
                }
            } else if (g_box_base_set == 1) {
                /* 拿不到有效定位（室内、冷启动、被遮挡）：连续 30 秒才算异常，
                 * 避免刚上电时的正常无信号被误判 */
                if (g_box_fix_lost_since == 0) {
                    g_box_fix_lost_since = now;
                } else if ((now - g_box_fix_lost_since) > BOX_FIX_LOST) {
                    if (g_box_alarm == 0) {
                        printf("[RULE] 物资定位丢失超过 30 秒 -> 告警\r\n");
                    }
                    g_box_alarm = 1;
                    g_box_alarm_until = now + ALARM_HOLD;
                    if (g_buzz_on == 0) {
                        if (SendCmdTo("BOX", "CMD=BUZZ,ON=1") == 0) {
                            g_buzz_on = 1;
                        }
                    }
                }
            }
        }
    }
}

/* ---------------- 任务 6：每秒巡查（定时类规则都在这里） ---------------- */
static void RuleTask(void *arg)
{
    (void)arg;
    SensorMsg_t m;
    uint32_t now;
    int i;
    int env_alarm;

    while (1) {
        osDelay(100);    /* 1 秒走一圈 */
        now = osKernelGetTickCount();

        /* 规则 1 的收尾：人走了 30 秒，关灯 */
        if ((g_light_req == 1) && (now >= g_light_until)) {
            printf("[RULE] 无人 30 秒 -> 关路灯\r\n");
            SendCmdTo("LAMP", "CMD=LIGHT,LED=0");
            g_light_req = 0;
        }

        /* 规则 2 的收尾：物资告警满 10 秒自动解除，状态回到正常 */
        if ((g_box_alarm == 1) && (now >= g_box_alarm_until)) {
            g_box_alarm = 0;
            printf("[RULE] 物资告警解除（已回到基准点附近或定位恢复），状态恢复正常\r\n");
            if (g_buzz_on == 1) {
                SendCmdTo("BOX", "CMD=BUZZ,ON=0");
                g_buzz_on = 0;
            }
        }

        /* 规则 3：柜内温湿度超限 -> 远程让物流节点蜂鸣提醒（本地风扇在 ControlTask） */
        osMutexAcquire(g_lock, osWaitForever);
        m = g_latest;
        osMutexRelease(g_lock);

        env_alarm = (m.temp > TEMP_HIGH || m.humi > HUMI_HIGH) ? 1 : 0;
        if (env_alarm && g_buzz_on == 0 && g_box_alarm == 0) {
            printf("[RULE] 柜内 T=%.1f H=%.1f 超限 -> 远程蜂鸣\r\n", m.temp, m.humi);
            if (SendCmdTo("BOX", "CMD=BUZZ,ON=1") == 0) {
                g_buzz_on = 1;
            }
        } else if (!env_alarm && g_buzz_on == 1 && g_box_alarm == 0) {
            SendCmdTo("BOX", "CMD=BUZZ,ON=0");
            g_buzz_on = 0;
        }

        /* 节点掉线检测：15 秒没消息 */
        osMutexAcquire(g_lock, osWaitForever);
        for (i = 0; i < MAX_NODES; i++) {
            if (g_nodes[i].name[0] != 0 && g_nodes[i].online &&
                (now - g_nodes[i].last_tick) > NODE_TIMEOUT) {
                g_nodes[i].online = 0;
                printf("[GW] 节点 %s 掉线（15 秒无消息）\r\n", g_nodes[i].name);
            }
        }
        osMutexRelease(g_lock);
    }
}

/* ---------------- 任务 7：收电脑命令（手动干预 IA1） ---------------- */
static void CmdTask(void *arg)
{
    (void)arg;
    char buf[64];
    struct sockaddr_in from;
    socklen_t from_len;
    int n;

    while (g_cmd_fd < 0) {
        osDelay(50);
    }
    printf("[CMD] listening on udp %d\r\n", GW_CMD_PORT);

    while (1) {
        memset(buf, 0, sizeof(buf));
        from_len = sizeof(from);
        n = recvfrom(g_cmd_fd, buf, sizeof(buf) - 1, 0,
                     (struct sockaddr *)&from, &from_len);
        if (n <= 0) {
            continue;
        }
        buf[n] = 0;
        printf("[CMD] %s:%d => %s\r\n",
               inet_ntoa(from.sin_addr), ntohs(from.sin_port), buf);

        if (strstr(buf, "BASE") != NULL) {
            /* 换了场地 / 重启过物资板之后，用 BASE 清掉旧基准点 */
            g_box_base_set = 0;
            g_box_alarm = 0;
            g_box_fix_lost_since = 0;
            g_box_last_dist = 0.0;
            printf("[CMD] 基准点已清除，等下一个有效定位重新记录\r\n");
        } else if (strstr(buf, "LED=1") != NULL) {
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
            printf("[CMD] 不认识的命令: %s\r\n", buf);
            continue;
        }

        g_manual_until = osKernelGetTickCount() + MANUAL_HOLD;
        printf("[CMD] 手动模式 %d 秒\r\n", MANUAL_HOLD / 100);
    }
}

/* ---------------- 入口 ---------------- */
static void YizhanNodeEntry(void)
{
    osThreadAttr_t attr;

    printf("\r\n===== 应急物资站 主控/网关 v3（7 任务）=====\r\n");

    memset(g_nodes, 0, sizeof(g_nodes));

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

    attr.name       = "GwTask";
    attr.stack_size = STACK_GW;
    attr.priority   = PRIO_GW;
    osThreadNew((osThreadFunc_t)GwTask, NULL, &attr);

    attr.name       = "RuleTask";
    attr.stack_size = STACK_RULE;
    attr.priority   = PRIO_RULE;
    osThreadNew((osThreadFunc_t)RuleTask, NULL, &attr);

    attr.name       = "CmdTask";
    attr.stack_size = STACK_CMD;
    attr.priority   = PRIO_CMD;
    osThreadNew((osThreadFunc_t)CmdTask, NULL, &attr);
}

APP_FEATURE_INIT(YizhanNodeEntry);
