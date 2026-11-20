#include <string.h>
#include "box_logic.h"
/* Short-range projection for movement detection. No libm calls: the Hi3861
 * firmware linker does not reliably inherit a GN static library's libm. */
static double LatitudeCosine(double radians)
{
    double term = 1.0, result = 1.0;
    double squared = radians * radians;
    /* Mean latitude is within +/- pi/2; this series converges over that range. */
    for (unsigned i=1; i<=10; ++i) {
        term *= -squared / ((2.0*i-1.0)*(2.0*i));
        result += term;
    }
    return result > 0.0 ? result : 0.0;
}

double BoxDistanceMeters(const BoxPosition *from, const BoxPosition *to)
{
    const double radians_per_e6 = 3.14159265358979323846 / 180000000.0;
    double mean_lat = ((double)from->lat_e6 + to->lat_e6) * 0.5 * radians_per_e6;
    double delta_lon = (double)to->lon_e6 - from->lon_e6;
    if (delta_lon > 180000000.0) delta_lon -= 360000000.0;
    if (delta_lon < -180000000.0) delta_lon += 360000000.0;
    double north = ((double)to->lat_e6-from->lat_e6) * radians_per_e6 * 6371000.0;
    double east = delta_lon * radians_per_e6 * 6371000.0 * LatitudeCosine(mean_lat);
    if (north < 0.0) north = -north;
    if (east < 0.0) east = -east;
    double scale = north > east ? north : east;
    if (scale == 0.0) return 0.0;
    /* Normalize first: squared length is in [1,2], so eight Newton steps
     * suffice independently of displacement magnitude, without sqrt(). */
    north /= scale;
    east /= scale;
    double squared = north*north + east*east;
    double root = 1.0;
    for (unsigned i=0; i<8; ++i) root = 0.5*(root+squared/root);
    return scale*root;
}
static int Hex(char c)
{
    if (c >= '0' && c <= '9') return c-'0';
    if (c >= 'A' && c <= 'F') return c-'A'+10;
    if (c >= 'a' && c <= 'f') return c-'a'+10;
    return -1;
}
static int Coordinate(const char *s, char hemisphere, int degrees, int32_t *result)
{
    unsigned whole = 0, fraction = 0, scale = 1;
    size_t n = strlen(s), digits = (size_t)degrees + 2;
    if (n < digits || (n > digits && (s[digits] != '.' || n == digits+1))) return 0;
    for (size_t i=0; i<digits; ++i) {
        if (s[i] < '0' || s[i] > '9') return 0;
        whole = whole*10 + (unsigned)(s[i]-'0');
    }
    for (size_t i=digits+1; i<n; ++i) {
        if (s[i] < '0' || s[i] > '9' || scale >= 1000000) return 0;
        fraction = fraction*10 + (unsigned)(s[i]-'0'); scale *= 10;
    }
    unsigned deg = whole/100, min = whole%100, limit = degrees == 2 ? 90 : 180;
    if (min >= 60 || deg > limit || (deg == limit && (min || fraction))) return 0;
    int32_t value = (int32_t)(deg*1000000 + (min*1000000 + fraction*(1000000/scale))/60);
    if ((degrees == 2 && hemisphere != 'N' && hemisphere != 'S') ||
        (degrees == 3 && hemisphere != 'E' && hemisphere != 'W')) return 0;
    *result = (hemisphere == 'S' || hemisphere == 'W') ? -value : value;
    return 1;
}
static int Rmc(char *line, BoxPosition *out)
{
    char *star = strchr(line, '*'), *fields[16];
    unsigned checksum = 0, count = 0;
    BoxPosition p = {0,0,0};
    if (!star || strlen(star) != 3 || Hex(star[1]) < 0 || Hex(star[2]) < 0) return 0;
    for (char *c=line+1; c<star; ++c) checksum ^= (unsigned char)*c;
    if (checksum != (unsigned)(Hex(star[1])*16+Hex(star[2]))) return 0;
    *star = 0;
    fields[count++] = line+1;
    for (char *c=line+1; *c; ++c) if (*c == ',') {
        if (count >= 16) return 0;
        *c = 0; fields[count++] = c+1;
    }
    if (count < 10 || (strcmp(fields[0],"GPRMC") && strcmp(fields[0],"GNRMC"))) return 0;
    if (!strcmp(fields[2],"V")) { *out = p; return 1; }
    if (strcmp(fields[2],"A") || strlen(fields[4]) != 1 || strlen(fields[6]) != 1 ||
        !Coordinate(fields[3],fields[4][0],2,&p.lat_e6) ||
        !Coordinate(fields[5],fields[6][0],3,&p.lon_e6)) return 0;
    /* Optional RMC mode N explicitly invalidates the solution. */
    p.fix = !(count > 12 && !strcmp(fields[12],"N"));
    if (!p.fix) { p.lat_e6 = 0; p.lon_e6 = 0; }
    *out = p; return 1;
}
int BoxNmeaFeed(BoxNmea *s, unsigned char ch, BoxPosition *p)
{
    if (ch == '$') { s->used=1; s->line[0]='$'; return 0; }
    if (!s->used) return 0;
    if (ch == '\r') return 0;
    if (ch == '\n') {
        s->line[s->used]=0; s->used=0;
        return Rmc(s->line,p);
    }
    if (ch < 32 || ch > 126 || s->used >= sizeof(s->line)-1) { s->used=0; return 0; }
    s->line[s->used++]=(char)ch; return 0;
}
int BoxParseCommand(const char *text)
{
    size_t n = strlen(text);
    while (n && (text[n-1] == '\n' || text[n-1] == '\r')) --n;
    if (n != 13 || memcmp(text,"CMD=BUZZ,ON=",12)) return -1;
    return text[12]=='0' ? 0 : text[12]=='1' ? 1 : -1;
}
size_t BoxNdefUri(const char *url, unsigned char *out, size_t capacity)
{
    size_t n = strlen(url), bytes = (n+8+15)/16*16;
    if ((strncmp(url,"http://",7) && strncmp(url,"https://",8)) ||
        n < 10 || n > 220 || bytes > capacity) return 0;
    for (size_t i=0; i<n; ++i) if ((unsigned char)url[i] <= 32 || (unsigned char)url[i] > 126) return 0;
    memset(out,0,bytes);
    out[0]=0x03; out[1]=(unsigned char)(n+5); /* NDEF TLV */
    out[2]=0xd1; out[3]=1; out[4]=(unsigned char)(n+1);
    out[5]='U'; out[6]=0; /* no URI prefix compression */
    memcpy(out+7,url,n); out[n+7]=0xfe;
    return bytes;
}
