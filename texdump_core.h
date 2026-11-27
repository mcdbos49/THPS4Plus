/*
 * texdump_core.h - platform-independent pieces of the THPS4+ texture dumper:
 * content hashing, DXT/RGB decoding to RGBA8, and a dependency-free PNG writer.
 * Format codes are the numeric D3DFORMAT values, so this has no Windows deps.
 */
#ifndef TEXDUMP_CORE_H
#define TEXDUMP_CORE_H
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TD_A8R8G8B8 21
#define TD_X8R8G8B8 22
#define TD_R5G6B5   23
#define TD_X1R5G5B5 24
#define TD_A1R5G5B5 25
#define TD_A4R4G4B4 26
#define TD_A8       28
#define TD_L8       50
#define TD_DXT1 0x31545844u
#define TD_DXT2 0x32545844u
#define TD_DXT3 0x33545844u
#define TD_DXT4 0x34545844u
#define TD_DXT5 0x35545844u

static const char *td_fmt_name(unsigned f, char *buf, size_t n)
{
    switch (f) {
    case TD_A8R8G8B8: return "A8R8G8B8";
    case TD_X8R8G8B8: return "X8R8G8B8";
    case TD_R5G6B5:   return "R5G6B5";
    case TD_X1R5G5B5: return "X1R5G5B5";
    case TD_A1R5G5B5: return "A1R5G5B5";
    case TD_A4R4G4B4: return "A4R4G4B4";
    case TD_A8:       return "A8";
    case TD_L8:       return "L8";
    case TD_DXT1: return "DXT1";
    case TD_DXT2: return "DXT2";
    case TD_DXT3: return "DXT3";
    case TD_DXT4: return "DXT4";
    case TD_DXT5: return "DXT5";
    }
    snprintf(buf, n, "FMT%u", f);
    return buf;
}

/* bytes per pixel for uncompressed formats, 0 if unknown */
static int td_bpp(unsigned f)
{
    switch (f) {
    case TD_A8R8G8B8: case TD_X8R8G8B8: return 4;
    case TD_R5G6B5: case TD_X1R5G5B5: case TD_A1R5G5B5: case TD_A4R4G4B4: return 2;
    case TD_A8: case TD_L8: return 1;
    }
    return 0;
}

/* block bytes for DXT formats, 0 if not DXT */
static int td_block_bytes(unsigned f)
{
    if (f == TD_DXT1) return 8;
    if (f == TD_DXT2 || f == TD_DXT3 || f == TD_DXT4 || f == TD_DXT5) return 16;
    return 0;
}

/* number of rows of data and bytes per row actually used (ignores pitch padding) */
static int td_layout(unsigned f, unsigned w, unsigned h, unsigned *rows, unsigned *rowbytes)
{
    int bb = td_block_bytes(f), bpp;
    if (bb) { *rows = (h + 3) / 4; *rowbytes = ((w + 3) / 4) * bb; return 1; }
    bpp = td_bpp(f);
    if (bpp) { *rows = h; *rowbytes = w * bpp; return 1; }
    return 0;
}

/* FNV-1a 64 over (w,h,format) and the level-0 data rows. Returns 0 if format unknown. */
static int td_hash(unsigned f, const unsigned char *bits, int pitch, unsigned w, unsigned h, unsigned long long *out)
{
    unsigned rows, rb, y, i;
    unsigned long long hsh = 1469598103934665603ULL;
    unsigned hdr[3];
    const unsigned char *p;
    if (!td_layout(f, w, h, &rows, &rb)) return 0;
    hdr[0] = w; hdr[1] = h; hdr[2] = f;
    p = (const unsigned char *)hdr;
    for (i = 0; i < sizeof hdr; i++) { hsh ^= p[i]; hsh *= 1099511628211ULL; }
    for (y = 0; y < rows; y++) {
        const unsigned char *r = bits + (size_t)y * (size_t)pitch;
        for (i = 0; i < rb; i++) { hsh ^= r[i]; hsh *= 1099511628211ULL; }
    }
    *out = hsh;
    return 1;
}

/* ---- decoding ---- */

static void td_565(unsigned c, unsigned char *o)
{
    o[0] = (unsigned char)(((c >> 11) & 31) * 255 / 31);
    o[1] = (unsigned char)(((c >> 5) & 63) * 255 / 63);
    o[2] = (unsigned char)((c & 31) * 255 / 31);
}

static void td_color_block(const unsigned char *b, unsigned char px[16][4], int dxt1)
{
    unsigned c0 = b[0] | (b[1] << 8), c1 = b[2] | (b[3] << 8);
    unsigned char col[4][4];
    unsigned long idx;
    int k, i;
    td_565(c0, col[0]); col[0][3] = 255;
    td_565(c1, col[1]); col[1][3] = 255;
    if (!dxt1 || c0 > c1) {
        for (k = 0; k < 3; k++) {
            col[2][k] = (unsigned char)((2 * col[0][k] + col[1][k]) / 3);
            col[3][k] = (unsigned char)((col[0][k] + 2 * col[1][k]) / 3);
        }
        col[2][3] = col[3][3] = 255;
    } else {
        for (k = 0; k < 3; k++) col[2][k] = (unsigned char)((col[0][k] + col[1][k]) / 2);
        col[2][3] = 255;
        col[3][0] = col[3][1] = col[3][2] = col[3][3] = 0;   /* transparent */
    }
    idx = (unsigned long)b[4] | ((unsigned long)b[5] << 8) | ((unsigned long)b[6] << 16) | ((unsigned long)b[7] << 24);
    for (i = 0; i < 16; i++) memcpy(px[i], col[(idx >> (2 * i)) & 3], 4);
}

static void td_alpha5(const unsigned char *b, unsigned char a[16])
{
    unsigned char t[8];
    unsigned long long bits = 0;
    int i;
    t[0] = b[0]; t[1] = b[1];
    if (t[0] > t[1]) {
        for (i = 1; i <= 6; i++) t[1 + i] = (unsigned char)(((7 - i) * t[0] + i * t[1]) / 7);
    } else {
        for (i = 1; i <= 4; i++) t[1 + i] = (unsigned char)(((5 - i) * t[0] + i * t[1]) / 5);
        t[6] = 0; t[7] = 255;
    }
    for (i = 0; i < 6; i++) bits |= (unsigned long long)b[2 + i] << (8 * i);
    for (i = 0; i < 16; i++) a[i] = t[(bits >> (3 * i)) & 7];
}

/* returns malloc'd w*h*4 RGBA, or NULL if the format is unsupported */
static unsigned char *td_decode(unsigned f, const unsigned char *bits, int pitch, unsigned w, unsigned h)
{
    unsigned char *out;
    unsigned x, y;
    int bb = td_block_bytes(f);
    out = (unsigned char *)calloc((size_t)w * h, 4);
    if (!out) return NULL;

    if (bb) {
        unsigned bx, by, bw = (w + 3) / 4, bh = (h + 3) / 4;
        for (by = 0; by < bh; by++) for (bx = 0; bx < bw; bx++) {
            const unsigned char *b = bits + (size_t)by * (size_t)pitch + (size_t)bx * bb;
            unsigned char px[16][4], al[16];
            int i, has_alpha = 0;
            if (f == TD_DXT1) {
                td_color_block(b, px, 1);
            } else {
                td_color_block(b + 8, px, 0);
                has_alpha = 1;
                if (f == TD_DXT2 || f == TD_DXT3) {
                    for (i = 0; i < 16; i++) {
                        unsigned char v = b[i / 2];
                        al[i] = (unsigned char)(((i & 1) ? (v >> 4) : (v & 15)) * 17);
                    }
                } else td_alpha5(b, al);
            }
            for (i = 0; i < 16; i++) {
                unsigned px_x = bx * 4 + (i & 3), px_y = by * 4 + (i >> 2);
                unsigned char *o;
                if (px_x >= w || px_y >= h) continue;
                o = out + ((size_t)px_y * w + px_x) * 4;
                o[0] = px[i][0]; o[1] = px[i][1]; o[2] = px[i][2];
                o[3] = has_alpha ? al[i] : px[i][3];
            }
        }
        return out;
    }

    if (!td_bpp(f)) { free(out); return NULL; }
    for (y = 0; y < h; y++) for (x = 0; x < w; x++) {
        const unsigned char *s = bits + (size_t)y * (size_t)pitch + (size_t)x * td_bpp(f);
        unsigned char *o = out + ((size_t)y * w + x) * 4;
        unsigned v;
        switch (f) {
        case TD_A8R8G8B8: o[0] = s[2]; o[1] = s[1]; o[2] = s[0]; o[3] = s[3]; break;
        case TD_X8R8G8B8: o[0] = s[2]; o[1] = s[1]; o[2] = s[0]; o[3] = 255; break;
        case TD_R5G6B5:
            v = s[0] | (s[1] << 8); td_565(v, o); o[3] = 255; break;
        case TD_X1R5G5B5: case TD_A1R5G5B5:
            v = s[0] | (s[1] << 8);
            o[0] = (unsigned char)(((v >> 10) & 31) * 255 / 31);
            o[1] = (unsigned char)(((v >> 5) & 31) * 255 / 31);
            o[2] = (unsigned char)((v & 31) * 255 / 31);
            o[3] = (f == TD_A1R5G5B5) ? ((v & 0x8000) ? 255 : 0) : 255;
            break;
        case TD_A4R4G4B4:
            v = s[0] | (s[1] << 8);
            o[0] = (unsigned char)(((v >> 8) & 15) * 17);
            o[1] = (unsigned char)(((v >> 4) & 15) * 17);
            o[2] = (unsigned char)((v & 15) * 17);
            o[3] = (unsigned char)(((v >> 12) & 15) * 17);
            break;
        case TD_A8: o[0] = o[1] = o[2] = 255; o[3] = s[0]; break;
        case TD_L8: o[0] = o[1] = o[2] = s[0]; o[3] = 255; break;
        }
    }
    return out;
}

/* ---- PNG writer (stored deflate: big but needs no zlib) ---- */

static unsigned long td_crc_table[256];
static int td_crc_ready;

static void td_crc_init(void)
{
    unsigned long c; int n, k;
    for (n = 0; n < 256; n++) {
        c = (unsigned long)n;
        for (k = 0; k < 8; k++) c = (c & 1) ? (0xEDB88320UL ^ (c >> 1)) : (c >> 1);
        td_crc_table[n] = c;
    }
    td_crc_ready = 1;
}

static unsigned long td_crc(unsigned long c, const unsigned char *b, size_t n)
{
    size_t i;
    if (!td_crc_ready) td_crc_init();
    for (i = 0; i < n; i++) c = td_crc_table[(c ^ b[i]) & 0xFF] ^ (c >> 8);
    return c;
}

static void td_be32(unsigned char *p, unsigned long v)
{
    p[0] = (unsigned char)(v >> 24); p[1] = (unsigned char)(v >> 16);
    p[2] = (unsigned char)(v >> 8);  p[3] = (unsigned char)v;
}

static void td_chunk(FILE *f, const char *type, const unsigned char *data, unsigned long len)
{
    unsigned char hdr[8], tail[4];
    unsigned long c;
    td_be32(hdr, len); memcpy(hdr + 4, type, 4);
    fwrite(hdr, 1, 8, f);
    c = td_crc(0xFFFFFFFFUL, hdr + 4, 4);
    if (len) { fwrite(data, 1, len, f); c = td_crc(c, data, len); }
    td_be32(tail, c ^ 0xFFFFFFFFUL);
    fwrite(tail, 1, 4, f);
}

static int td_write_png(const char *path, const unsigned char *rgba, unsigned w, unsigned h)
{
    static const unsigned char sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    unsigned char ihdr[13];
    size_t rawlen = (size_t)h * (1 + (size_t)w * 4), nblocks, zlen, pos = 0, off = 0, y;
    unsigned char *raw, *z;
    unsigned long a = 1, b = 0;
    FILE *f;
    size_t i;

    raw = (unsigned char *)malloc(rawlen);
    if (!raw) return 0;
    for (y = 0; y < h; y++) {
        raw[y * (1 + (size_t)w * 4)] = 0;
        memcpy(raw + y * (1 + (size_t)w * 4) + 1, rgba + y * (size_t)w * 4, (size_t)w * 4);
    }
    nblocks = (rawlen + 65534) / 65535;
    zlen = 2 + nblocks * 5 + rawlen + 4;
    z = (unsigned char *)malloc(zlen);
    if (!z) { free(raw); return 0; }
    z[pos++] = 0x78; z[pos++] = 0x01;
    while (off < rawlen) {
        size_t n = rawlen - off; if (n > 65535) n = 65535;
        z[pos++] = (off + n >= rawlen) ? 1 : 0;
        z[pos++] = (unsigned char)(n & 255); z[pos++] = (unsigned char)(n >> 8);
        z[pos++] = (unsigned char)(~n & 255); z[pos++] = (unsigned char)((~n >> 8) & 255);
        memcpy(z + pos, raw + off, n); pos += n; off += n;
    }
    for (i = 0; i < rawlen; i++) { a = (a + raw[i]) % 65521UL; b = (b + a) % 65521UL; }
    td_be32(z + pos, (b << 16) | a); pos += 4;

    td_be32(ihdr, w); td_be32(ihdr + 4, h);
    ihdr[8] = 8; ihdr[9] = 6; ihdr[10] = 0; ihdr[11] = 0; ihdr[12] = 0;

    f = fopen(path, "wb");
    if (!f) { free(raw); free(z); return 0; }
    fwrite(sig, 1, 8, f);
    td_chunk(f, "IHDR", ihdr, 13);
    td_chunk(f, "IDAT", z, (unsigned long)pos);
    td_chunk(f, "IEND", NULL, 0);
    fclose(f);
    free(raw); free(z);
    return 1;
}
#endif
