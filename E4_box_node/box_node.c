#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "ohos_init.h"
#include "cmsis_os2.h"
#include "lwip/sockets.h"
#include "box_config.h"
#include "box_logic.h"
#include "box_hardware.h"

static osMutexId_t g_lock;
static BoxPosition g_position;
static uint32_t g_gps_updated;
static uint32_t g_gps_sequence;
static int g_have_rmc, g_buzzer;
static uint32_t g_remote_start;
static int g_remote;
static uint32_t Ticks(unsigned ms)
{
    return (uint32_t)(((uint64_t)ms * osKernelGetTickFreq() + 999) / 1000);
}
static void GpsTask(void *arg)
{
    BoxNmea stream = {{0},0};
    BoxPosition position = {0,0,0};
    unsigned char ch;
    (void)arg;
    while (BoxGpsInit() != 0) {
        printf("[GPS] UART init failed; retry\r\n"); osDelay(Ticks(5000));
    }
    for (;;) {
        int n=BoxGpsRead(&ch,1);
        if (n != 1) { osDelay(Ticks(20)); continue; }
        if (BoxNmeaFeed(&stream,ch,&position)) {
            osMutexAcquire(g_lock,osWaitForever);
            g_position=position; g_gps_updated=osKernelGetTickCount(); g_have_rmc=1;
            ++g_gps_sequence;
            osMutexRelease(g_lock);
        }
    }
}
typedef struct {
    BoxPosition anchor;
    uint32_t sample_tick;
    int anchored, sampled;
} MoveReference;

static int MovementSample(MoveReference *reference, const BoxPosition *position,
                          uint32_t tick, double *distance)
{
    if (reference->sampled &&
        (uint32_t)(tick-reference->sample_tick) < Ticks(BOX_MOVE_SAMPLE_MS))
        return 0;
    reference->sample_tick=tick;
    reference->sampled=1;
    if (!reference->anchored) {
        reference->anchor=*position;
        reference->anchored=1;
        return 0;
    }
    /* Keep the anchor fixed until an alarm finishes. Slow movement still
     * accumulates displacement; invalid GPS data never replaces the anchor. */
    *distance=BoxDistanceMeters(&reference->anchor,position);
    return *distance > BOX_MOVE_THRESHOLD_METERS;
}

/* Independent of UART and Wi-Fi: both buzzer timers expire even offline. */
static void BuzzerTask(void *arg)
{
    int previous=-1;
    int moving=0;
    uint32_t move_start=0, sequence=0;
    int tracking_fix=0, fix_reported=0, notice_active=0;
    uint32_t fix_since=0, notice_start=0;
    MoveReference reference = {0};
    (void)arg;
    for (;;) {
        int on, fresh, have_rmc, notice_on=0, fix_acquired=0, anchor_set=0;
        double distance=0.0;
        BoxPosition position;
        uint32_t updated, current_sequence;
        uint32_t now=osKernelGetTickCount();
        osMutexAcquire(g_lock,osWaitForever);
        position=g_position; updated=g_gps_updated;
        have_rmc=g_have_rmc; current_sequence=g_gps_sequence;
        /* Read 'now' after the shared GPS timestamp to avoid unsigned underflow. */
        now=osKernelGetTickCount();
        if (g_remote && (uint32_t)(now-g_remote_start) >= Ticks(BOX_REMOTE_MS))
            g_remote=0;
        fresh=have_rmc && position.fix &&
            (uint32_t)(now-updated) < Ticks(BOX_GPS_STALE_MS);
        /* Require continuous valid, regularly updated fixes. A single old
         * position must not produce a success tone after GPS output stops. */
        if (!fresh || (uint32_t)(now-updated) > Ticks(BOX_MOVE_GAP_MS)) {
            tracking_fix=0; fix_reported=0; notice_active=0;
        } else {
            if (!tracking_fix) { tracking_fix=1; fix_since=now; }
            if (!fix_reported && (uint32_t)(now-fix_since) >= Ticks(BOX_GPS_FIX_CONFIRM_MS)) {
                fix_reported=1; fix_acquired=1;
                if (BOX_GPS_FIX_NOTICE) { notice_active=1; notice_start=now; }
            }
        }
        if (moving) {
            sequence=current_sequence;
            if ((uint32_t)(now-move_start) >= Ticks(BOX_MOVE_BUZZ_MS)) {
                moving=0;
                reference.anchored=0; reference.sampled=0;
            }
        } else if (!fix_reported) {
            /* Pause comparisons during loss/reacquisition. Preserve the anchor
             * so a network or GPS outage cannot silently rearm at a new place. */
            reference.sampled=0;
            sequence=current_sequence;
        } else if (sequence != current_sequence) {
            sequence=current_sequence;
            int was_anchored=reference.anchored;
            if (MovementSample(&reference,&position,updated,&distance)) {
                moving=1;
                move_start=now;
            }
            anchor_set=!was_anchored && reference.anchored;
        }
        /* Alerts take priority and cancel a pending status sound. */
        if (g_remote || moving) notice_active=0;
        if (notice_active) {
            uint32_t elapsed=(uint32_t)(now-notice_start);
            uint32_t pulse=Ticks(BOX_GPS_FIX_PULSE_MS);
            if (elapsed >= 3*pulse) notice_active=0;
            else notice_on=elapsed < pulse || elapsed >= 2*pulse;
        }
        on=g_remote || moving || notice_on; g_buzzer=on;
        osMutexRelease(g_lock);
        if (fix_acquired)
            printf("[GPS] valid fix confirmed: LAT_E6=%ld,LON_E6=%ld; offline notice=%d\r\n",
                   (long)position.lat_e6,(long)position.lon_e6,BOX_GPS_FIX_NOTICE);
        if (anchor_set)
            printf("[MOVE] reference set: LAT_E6=%ld,LON_E6=%ld; no time limit\r\n",
                   (long)position.lat_e6,(long)position.lon_e6);
        if (distance > BOX_MOVE_THRESHOLD_METERS)
            printf("[MOVE] displacement from reference=%lu cm; buzzer %u ms\r\n",
                   (unsigned long)(distance*100.0),
                   (unsigned)BOX_MOVE_BUZZ_MS);
        if (on != previous) { BoxBuzzer(on); previous=on; }
        osDelay(Ticks(50));
    }
}
static void NfcTask(void *arg)
{
    (void)arg;
    if (!BOX_INSPECTION_URL[0]) {
        printf("[NFC] URL empty; tag unchanged. Configure inspection server URL.\r\n");
        return;
    }
    if (BoxNfcProgram(BOX_INSPECTION_URL) != 0)
        printf("[NFC] provisioning failed; move phone away, check tag, then reboot\r\n");
    else printf("[NFC] inspection URL stored (I2C verified): %s\r\n",BOX_INSPECTION_URL);
}

/* The serial monitor and UDP sender share the same snapshot and stale rules. */
static int BuildReport(char *packet, size_t capacity)
{
    BoxPosition position;
    int gps, buzzer;
    osMutexAcquire(g_lock,osWaitForever);
    position=g_position;
    buzzer=g_buzzer;
    gps=g_have_rmc &&
        (uint32_t)(osKernelGetTickCount()-g_gps_updated) < Ticks(BOX_GPS_STALE_MS);
    osMutexRelease(g_lock);
    if (!gps) position.fix=0;
    if (!position.fix) {
        position.lat_e6=0;
        position.lon_e6=0;
    }
    return snprintf(packet,capacity,
        "NODE=BOX,GPS=%d,FIX=%d,LAT_E6=%ld,LON_E6=%ld,BUZ=%d\n",
        gps,position.fix,(long)position.lat_e6,(long)position.lon_e6,buzzer);
}

static void SerialTask(void *arg)
{
    (void)arg;
    for (;;) {
        char packet[128];
        int length=BuildReport(packet,sizeof(packet));
        if (length > 0 && length < (int)sizeof(packet))
            printf("[GPS] %s",packet);
        osDelay(Ticks(BOX_REPORT_MS));
    }
}

static void NetworkTask(void *arg)
{
    (void)arg;
    for (;;) {
        struct sockaddr_in gateway = {0}, local = {0};
        int fd;
        if (BoxWifiConnect() != 0) {
            printf("[NET] connect failed; configure box_config.h/check AP\r\n");
            osDelay(Ticks(5000));
            continue;
        }
        gateway.sin_family = AF_INET;
        gateway.sin_port = htons(BOX_REPORT_PORT);
        gateway.sin_addr.s_addr = inet_addr(BOX_GATEWAY_IP);
        fd = socket(AF_INET, SOCK_DGRAM, 0);
        if (fd < 0) { osDelay(Ticks(1000)); continue; }
        local.sin_family = AF_INET;
        local.sin_port = htons(BOX_COMMAND_PORT);
        local.sin_addr.s_addr = htonl(INADDR_ANY);
        if (bind(fd, (struct sockaddr *)&local, sizeof(local)) != 0) {
            closesocket(fd); osDelay(Ticks(1000)); continue;
        }
        printf("[CMD] listening on udp %d\r\n", BOX_COMMAND_PORT);
        uint32_t last = osKernelGetTickCount() - Ticks(BOX_REPORT_MS);
        while (BoxWifiReady()) {
            uint32_t now = osKernelGetTickCount();
            if ((uint32_t)(now-last) >= Ticks(BOX_REPORT_MS)) {
                char packet[128];
                int n=BuildReport(packet,sizeof(packet));
                if (n < 0 || n >= (int)sizeof(packet) ||
                    sendto(fd,packet,n,0,(struct sockaddr *)&gateway,sizeof(gateway)) != n) break;
                last = now;
            }
            fd_set readable;
            struct timeval timeout = {0, 100000};
            FD_ZERO(&readable); FD_SET(fd, &readable);
            int ready = select(fd+1, &readable, NULL, NULL, &timeout);
            if (ready < 0) break;
            if (ready > 0) {
                char packet[64];
                struct sockaddr_in from;
                socklen_t size = sizeof(from);
                int n = recvfrom(fd, packet, sizeof(packet)-1, 0, (struct sockaddr *)&from, &size);
                if (n <= 0) break;
                /* Accept commands only from configured gateway. LAN is still trusted:
                   UDP source filtering is not authentication. */
                if (from.sin_addr.s_addr != gateway.sin_addr.s_addr ||
                    n >= (int)sizeof(packet)-1 || memchr(packet, 0, n)) continue;
                packet[n] = 0;
                int command = BoxParseCommand(packet);
                if (command < 0) continue;
                osMutexAcquire(g_lock, osWaitForever);
                g_remote = command;
                g_remote_start = osKernelGetTickCount();
                osMutexRelease(g_lock);
                printf("[CMD] BUZZ=%d (ON lease %d ms)\r\n", command, BOX_REMOTE_MS);
            }
        }
        closesocket(fd);
        osDelay(Ticks(1000));
    }
}
static void BoxEntry(void)
{
    osThreadAttr_t attr = {0};
    if (BoxHardwareInit() != 0) {
        printf("[BOOT] hardware initialization failed\r\n");
        return;
    }
    g_lock = osMutexNew(NULL);
    if (!g_lock) return;
    attr.name = "BoxBuzzer";
    attr.stack_size = 2048;
    attr.priority = osPriorityAboveNormal;
    if (!osThreadNew(BuzzerTask,NULL,&attr)) {
        printf("[BOOT] buzzer task failed\r\n"); return;
    }
    attr.name = "BoxGps";
    attr.stack_size = 4096;
    attr.priority = osPriorityNormal;
    if (!osThreadNew(GpsTask,NULL,&attr)) printf("[BOOT] GPS task failed\r\n");
    attr.name = "BoxSerial";
    attr.stack_size = 2048;
    if (!osThreadNew(SerialTask,NULL,&attr)) printf("[BOOT] serial report task failed\r\n");
    attr.name = "BoxNfc";
    attr.stack_size = 4096;
    if (!osThreadNew(NfcTask,NULL,&attr)) printf("[BOOT] NFC task failed\r\n");
    attr.name = "BoxNetwork";
    attr.stack_size = 8192;
    attr.priority = osPriorityNormal;
    if (!osThreadNew(NetworkTask, NULL, &attr)) printf("[BOOT] network task failed\r\n");
}
APP_FEATURE_INIT(BoxEntry);
