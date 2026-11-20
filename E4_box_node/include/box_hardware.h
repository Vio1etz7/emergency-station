#ifndef BOX_HARDWARE_H
#define BOX_HARDWARE_H
int BoxHardwareInit(void);
int BoxGpsInit(void);
int BoxGpsRead(unsigned char *buffer, unsigned length);
void BoxBuzzer(int on);
int BoxNfcProgram(const char *url);
int BoxWifiConnect(void);
int BoxWifiReady(void);
#endif
