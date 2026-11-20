#ifndef BOX_LOGIC_H
#define BOX_LOGIC_H
#include <stddef.h>
#include <stdint.h>
typedef struct { int fix; int32_t lat_e6, lon_e6; } BoxPosition;
typedef struct { char line[128]; unsigned used; } BoxNmea;
/* Returns 1 for a complete checked RMC (including status V), 0 otherwise. */
int BoxNmeaFeed(BoxNmea *stream, unsigned char ch, BoxPosition *position);
int BoxParseCommand(const char *text);
/* Short-range distance in metres (local projection); requires valid fixes. */
double BoxDistanceMeters(const BoxPosition *from, const BoxPosition *to);
/* Encode one URI NDEF TLV, zero padded to a 16-byte boundary; 0 on error. */
size_t BoxNdefUri(const char *url, unsigned char *out, size_t capacity);
#endif
