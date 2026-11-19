/*
 * Copyright (c) 2020 Nanjing Xiaoxiongpai Intelligent Technology Co., Ltd.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * Wi-Fi 连接（STA 模式）
 * ------------------------------------------------------------
 * 取自小熊派官方例程，两处改动（都不改接口）：
 *   1) 官方版扫不到指定热点时会 while(1) 死等，上层永远没法重试；
 *      这里改成返回错误码，交给 NetTask 每 5 秒重连一次。
 *   2) 扫描、DHCP 都加了次数上限，绝不无限阻塞。
 * 返回码：0 成功，-1 使能失败，-2 扫描失败，-3 没扫到该 SSID，
 *        -4 连接失败，-5 DHCP 超时。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "lwip/netif.h"
#include "lwip/netifapi.h"
#include "lwip/ip4_addr.h"
#include "lwip/api_shell.h"

#include "cmsis_os2.h"
#include "hos_types.h"
#include "wifi_device.h"
#include "wifiiot_errno.h"
#include "ohos_init.h"

#define DEF_TIMEOUT         15      /* 等扫描/连接结果，最多 15 秒 */
#define SCAN_ATTEMPTS       2       /* 扫描最多试 2 轮 */
#define DHCP_TIMEOUT_TICKS  100     /* DHCP 最多等 100 * 1 秒 */

#define SELECT_WIFI_SECURITYTYPE WIFI_SEC_TYPE_PSK
#define SELECT_WLAN_PORT         "wlan0"

static void WiFiInit(void);
static void WaitSacnResult(void);
static int WaitConnectResult(void);
static void OnWifiScanStateChangedHandler(int state, int size);
static void OnWifiConnectionChangedHandler(int state, WifiLinkedInfo *info);
static void OnHotspotStaJoinHandler(StationInfo *info);
static void OnHotspotStateChangedHandler(int state);
static void OnHotspotStaLeaveHandler(StationInfo *info);

static int g_staScanSuccess = 0;
static int g_connectSuccess = 0;
static int g_ssidCount = 0;
static WifiEvent g_wifiEventHandler = {0};
static WifiErrorCode g_error = WIFI_SUCCESS;

int WifiConnect(const char *ssid, const char *psk)
{
    WifiScanInfo *info = NULL;
    unsigned int size = WIFI_SCAN_HOTSPOT_LIMIT;
    struct netif *netif = NULL;
    WifiDeviceConfig config = {0};
    int netId = 0;
    int found = -1;
    int i;
    int wait;

    if ((ssid == NULL) || (psk == NULL)) {
        return -1;
    }

    printf("<--System Init-->\r\n");
    WiFiInit();

    if (EnableWifi() != WIFI_SUCCESS) {
        printf("EnableWifi failed, error = %d\r\n", g_error);
        return -1;
    }

    if (IsWifiActive() == 0) {
        printf("Wifi station is not actived.\r\n");
        return -1;
    }

    info = malloc(sizeof(WifiScanInfo) * WIFI_SCAN_HOTSPOT_LIMIT);
    if (info == NULL) {
        printf("[WIFI] malloc failed\r\n");
        return -1;
    }

    /* 扫描（最多 2 轮，每轮最多 15 秒） */
    for (i = 0; (i < SCAN_ATTEMPTS) && (g_staScanSuccess != 1); i++) {
        g_staScanSuccess = 0;
        g_ssidCount = 0;
        Scan();
        WaitSacnResult();
    }
    if (g_staScanSuccess != 1) {
        printf("[WIFI] scan failed, retry later\r\n");
        free(info);
        return -2;
    }

    g_error = GetScanInfoList(info, &size);
    if (g_error != WIFI_SUCCESS) {
        printf("[WIFI] get scan list failed, error = %d\r\n", g_error);
        free(info);
        return -2;
    }

    printf("********************\r\n");
    for (i = 0; i < g_ssidCount; i++) {
        printf("no:%03d, ssid:%-30s, rssi:%5d\r\n", i + 1, info[i].ssid, info[i].rssi / 100);
    }
    printf("********************\r\n");

    /* 找目标热点 */
    for (i = 0; i < g_ssidCount; i++) {
        if (strcmp(ssid, info[i].ssid) == 0) {
            found = i;
            break;
        }
    }
    if (found < 0) {
        printf("[WIFI] ssid \"%s\" not found, retry later\r\n", ssid);
        free(info);
        return -3;
    }

    printf("Select:%3d wireless, Waiting...\r\n", found + 1);

    strncpy(config.ssid, info[found].ssid, sizeof(config.ssid) - 1);
    strncpy(config.preSharedKey, psk, sizeof(config.preSharedKey) - 1);
    config.securityType = SELECT_WIFI_SECURITYTYPE;

    if (AddDeviceConfig(&config, &netId) != WIFI_SUCCESS) {
        printf("[WIFI] add device config failed\r\n");
        free(info);
        return -4;
    }

    if ((ConnectTo(netId) != WIFI_SUCCESS) || (WaitConnectResult() != 1)) {
        printf("[WIFI] connect failed, retry later\r\n");
        free(info);
        return -4;
    }
    printf("WiFi connect succeed!\r\n");

    netif = netifapi_netif_find(SELECT_WLAN_PORT);
    if (netif == NULL) {
        printf("[WIFI] find netif %s failed\r\n", SELECT_WLAN_PORT);
        free(info);
        return -5;
    }

    /* 启动 DHCP，最多等 10 秒 */
    dhcp_start(netif);
    printf("begain to dhcp\r\n");
    for (wait = 0; wait < DHCP_TIMEOUT_TICKS; wait++) {
        if (dhcp_is_bound(netif) == ERR_OK) {
            break;
        }
        osDelay(100);
    }
    if (wait >= DHCP_TIMEOUT_TICKS) {
        printf("[WIFI] dhcp timeout, retry later\r\n");
        free(info);
        return -5;
    }

    printf("<-- DHCP state:OK -->\r\n");
    netifapi_netif_common(netif, dhcp_clients_info_show, NULL);

    free(info);
    osDelay(10);
    return 0;
}

static void WiFiInit(void)
{
    g_wifiEventHandler.OnWifiScanStateChanged = OnWifiScanStateChangedHandler;
    g_wifiEventHandler.OnWifiConnectionChanged = OnWifiConnectionChangedHandler;
    g_wifiEventHandler.OnHotspotStaJoin = OnHotspotStaJoinHandler;
    g_wifiEventHandler.OnHotspotStaLeave = OnHotspotStaLeaveHandler;
    g_wifiEventHandler.OnHotspotStateChanged = OnHotspotStateChangedHandler;

    g_error = RegisterWifiEvent(&g_wifiEventHandler);
    if (g_error != WIFI_SUCCESS) {
        printf("register wifi event fail!\r\n");
    }
}

static void OnWifiScanStateChangedHandler(int state, int size)
{
    if (size > 0) {
        g_ssidCount = size;
        g_staScanSuccess = 1;
    }
    printf("callback function for wifi scan:%d, %d\r\n", state, size);
}

static void OnWifiConnectionChangedHandler(int state, WifiLinkedInfo *info)
{
    if (info == NULL) {
        printf("WifiConnectionChanged:info is null, stat is %d.\r\n", state);
        return;
    }

    if (state == WIFI_STATE_AVALIABLE) {
        g_connectSuccess = 1;
    } else {
        g_connectSuccess = 0;
    }
}

static void OnHotspotStaJoinHandler(StationInfo *info)
{
    (void)info;
    printf("STA join AP\r\n");
}

static void OnHotspotStaLeaveHandler(StationInfo *info)
{
    (void)info;
    printf("HotspotStaLeave\r\n");
}

static void OnHotspotStateChangedHandler(int state)
{
    printf("HotspotStateChanged:state is %d.\r\n", state);
}

/* 等扫描结果：最多 DEF_TIMEOUT 秒，不再无限等 */
static void WaitSacnResult(void)
{
    int scanTimeout = DEF_TIMEOUT;

    while (scanTimeout > 0) {
        osDelay(100);   /* 1 秒 */
        scanTimeout--;
        if (g_staScanSuccess == 1) {
            printf("WaitSacnResult:wait success[%d]s\r\n", DEF_TIMEOUT - scanTimeout);
            return;
        }
    }

    printf("WaitSacnResult:timeout!\r\n");
}

/* 等连接结果：最多 DEF_TIMEOUT 秒 */
static int WaitConnectResult(void)
{
    int connectTimeout = DEF_TIMEOUT;

    while (connectTimeout > 0) {
        osDelay(100);   /* 1 秒 */
        connectTimeout--;
        if (g_connectSuccess == 1) {
            printf("WaitConnectResult:wait success[%d]s\r\n", DEF_TIMEOUT - connectTimeout);
            return 1;
        }
    }

    printf("WaitConnectResult:timeout!\r\n");
    return 0;
}
