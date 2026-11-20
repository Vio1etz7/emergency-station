#include <string.h>
#include <stdio.h>
#include "cmsis_os2.h"
#include "wifi_device.h"
#include "lwip/netifapi.h"
#include "box_config.h"
#include "box_hardware.h"

static struct netif *g_interface;
static int g_network = -1;
static void DelaySecond(void) { osDelay(osKernelGetTickFreq()); }
int BoxWifiReady(void)
{
    WifiLinkedInfo info = {0};
    return GetLinkedInfo(&info) == WIFI_SUCCESS &&
        info.connState == WIFI_CONNECTED && g_interface &&
        netifapi_dhcp_is_bound(g_interface) == ERR_OK;
}
int BoxWifiConnect(void)
{
    WifiDeviceConfig config = {0};
    WifiLinkedInfo info = {0};
    int result;
    size_t password_length = strlen(BOX_WIFI_PASSWORD);
    if (!BOX_WIFI_SSID[0] || !strcmp(BOX_WIFI_SSID, "CHANGE_ME")) {
        printf("[WIFI] configure BOX_WIFI_SSID first\r\n"); return -1;
    }
    if (strlen(BOX_WIFI_SSID) >= sizeof(config.ssid) ||
        password_length >= sizeof(config.preSharedKey)) {
        printf("[WIFI] SSID or password exceeds SDK buffer capacity\r\n"); return -1;
    }
    /* This application uses WPA/WPA2 PSK with a text passphrase. */
    if (password_length < 8 || password_length > 63) {
        printf("[WIFI] PSK password must be 8-63 bytes; change hotspot and BOX_WIFI_PASSWORD\r\n");
        return -1;
    }
    if (IsWifiActive() != 1) {
        result = EnableWifi();
        if (result != WIFI_SUCCESS) {
            printf("[WIFI] EnableWifi failed: %d\r\n",result); return -1;
        }
    }
    if (g_interface) netifapi_dhcp_stop(g_interface);
    Disconnect();
    if (g_network >= 0) { RemoveDevice(g_network); g_network = -1; }
    strcpy(config.ssid, BOX_WIFI_SSID);
    strcpy(config.preSharedKey, BOX_WIFI_PASSWORD);
    config.securityType = WIFI_SEC_TYPE_PSK;
    result = AddDeviceConfig(&config, &g_network);
    if (result != WIFI_SUCCESS) {
        printf("[WIFI] AddDeviceConfig failed: %d\r\n",result); return -1;
    }
    result = ConnectTo(g_network);
    if (result != WIFI_SUCCESS) {
        printf("[WIFI] ConnectTo failed: %d\r\n",result); return -1;
    }
    printf("[WIFI] connecting; use 2.4 GHz WPA2 hotspot\r\n");
    int connected = 0;
    for (int i = 0; i < 20; ++i) {
        DelaySecond();
        result = GetLinkedInfo(&info);
        if (result == WIFI_SUCCESS && info.connState == WIFI_CONNECTED) {
            connected = 1; break;
        }
    }
    if (!connected) {
        printf("[WIFI] connection timeout: query=%d state=%d; check SSID/password/band/security\r\n",
               result,(int)info.connState); return -1;
    }
    printf("[WIFI] associated; requesting DHCP address\r\n");
    g_interface = netifapi_netif_find("wlan0");
    if (!g_interface) {
        printf("[WIFI] wlan0 interface not found\r\n"); return -1;
    }
    result = netifapi_dhcp_start(g_interface);
    if (result != ERR_OK) {
        printf("[WIFI] DHCP start failed: %d\r\n",result); return -1;
    }
    for (int i = 0; i < 20; ++i) {
        if (BoxWifiReady()) {
            printf("[WIFI] DHCP ready\r\n"); return 0;
        }
        DelaySecond();
    }
    printf("[WIFI] DHCP timeout or Wi-Fi disconnected\r\n");
    return -1;
}
