#ifndef BOX_CONFIG_H
#define BOX_CONFIG_H
#define BOX_WIFI_SSID "reno12"
/* WPA2 hotspot passphrase must be 8-63 bytes; match the phone's setting. */
#define BOX_WIFI_PASSWORD "12345678"
/* Receiver computer/controller IPv4 address, not the phone hotspot gateway. */
#define BOX_GATEWAY_IP "10.222.234.111"
#define BOX_REPORT_PORT 8888
#define BOX_COMMAND_PORT 8892
#define BOX_REPORT_MS 1000
#define BOX_REMOTE_MS 3000
#define BOX_GPS_STALE_MS 5000
/* Offline indication: two short beeps after a continuous valid GPS fix. */
#define BOX_GPS_FIX_NOTICE 1
#define BOX_GPS_FIX_CONFIRM_MS 3000
#define BOX_GPS_FIX_PULSE_MS 150
#define BOX_MOVE_THRESHOLD_METERS 5.0
#define BOX_MOVE_BUZZ_MS 3000
/* Movement samples are taken at most once per second. */
#define BOX_MOVE_SAMPLE_MS 1000
#define BOX_MOVE_GAP_MS 2000
#if BOX_MOVE_SAMPLE_MS <= 0
#error "Movement sample interval must be positive"
#endif
/* Full URL, including device query. Empty: leave the NFC EEPROM unchanged.
 * Example: http://192.168.43.100:8080/inspect?device=HDPAP00008-325JPC4X0I
 * Setting a URL replaces the tag's existing NDEF user message on next boot. */
/* An IP address alone is invalid: supply the complete URL shown above,
 * replacing its host with the computer's actual Wi-Fi IPv4 address. */
#define BOX_INSPECTION_URL "http://10.222.234.111:8080/inspect?device=HDPAP00008-325JPC4X0I"
#endif
