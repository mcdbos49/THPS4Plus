/*
 * texload_core.h - platform-independent PNG loading for THPS4+ texture import:
 * a small inflate (zlib) decoder, a PNG decoder to RGBA8, and a box-filter mip generator.
 * Supports 8-bit and 16-bit, gray / gray+alpha / RGB / RGBA / palette (with tRNS), bit depths 1-16,
 * non-interlaced. Interlaced PNGs are refused (re-save without interlacing).
 */
#ifndef TEXLOAD_CORE_H
#define TEXLOAD_CORE_H
#include <stdlib.h>
#include <string.h>

/* ---------------- inflate ---------------- */
typedef struct { const unsigned char *src; size_t n, pos; unsigned bitbuf; int bitcnt;
                 unsigned char *dst; size_t cap, len; int err; } TlInf;
typedef struct { unsigned short count[16], symbol[320]; } TlHuff;

static unsigned tl_bits(TlInf *s, int need)
{
    unsigned v;
    while (s->bitcnt < need) {
        if (s->pos >= s->n) { s->err = 1; return 0; }
        s->bitbuf |= (unsigned)s->src[s->pos++] << s->bitcnt;
        s->bitcnt += 8;
    }
    v = s->bitbuf & ((1u << need) - 1u);
    s->bitbuf >>= need; s->bitcnt -= need;
    return v;
}

static void tl_huff_build(TlHuff *h, const unsigned char *lens, int n)
{
    int i; unsigned short offs[16];
    memset(h->count, 0, sizeof h->count);
    for (i = 0; i < n; i++) h->count[lens[i]]++;
    h->count[0] = 0;
    offs[1] = 0;
    for (i = 1; i < 15; i++) offs[i + 1] = (unsigned short)(offs[i] + h->count[i]);
    for (i = 0; i < n; i++) if (lens[i]) h->symbol[offs[lens[i]]++] = (unsigned short)i;
}

static int tl_huff_decode(TlInf *s, const TlHuff *h)
{
    int code = 0, first = 0, index = 0, len;
    for (len = 1; len <= 15; len++) {
        int count;
        code |= (int)tl_bits(s, 1);
        if (s->err) return -1;
        count = h->count[len];
        if (code - count < first) return h->symbol[index + (code - first)];
        index += count; first += count; first <<= 1; code <<= 1;
    }
    return -1;
}

static int tl_codes(TlInf *s, const TlHuff *lit, const TlHuff *dist)
{
    static const unsigned short lbase[29] = {3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258};
    static const unsigned short lext[29] = {0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0};
    static const unsigned short dbase[30] = {1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577};
    static const unsigned short dext[30] = {0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13};
    for (;;) {
        int sym = tl_huff_decode(s, lit);
        if (sym < 0) return 1;
        if (sym < 256) {
            if (s->len >= s->cap) return 1;
            s->dst[s->len++] = (unsigned char)sym;
        } else if (sym == 256) {
            return 0;
        } else {
            int len, d; unsigned dist_;
            sym -= 257;
            if (sym >= 29) return 1;
            len = lbase[sym] + (int)tl_bits(s, lext[sym]);
            d = tl_huff_decode(s, dist);
            if (d < 0 || d >= 30) return 1;
            dist_ = dbase[d] + tl_bits(s, dext[d]);
            if (s->err || dist_ > s->len || s->len + (size_t)len > s->cap) return 1;
            while (len--) { s->dst[s->len] = s->dst[s->len - dist_]; s->len++; }
        }
    }
}

static int tl_dynamic(TlInf *s)
{
    static const unsigned char order[19] = {16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15};
    unsigned char lens[320];
    TlHuff lh, dh, ch;
    int nlen = (int)tl_bits(s, 5) + 257, ndist = (int)tl_bits(s, 5) + 1, ncode = (int)tl_bits(s, 4) + 4, i;
    if (s->err || nlen > 286 || ndist > 30) return 1;
    for (i = 0; i < 19; i++) lens[i] = 0;
    for (i = 0; i < ncode; i++) lens[order[i]] = (unsigned char)tl_bits(s, 3);
    tl_huff_build(&ch, lens, 19);
    i = 0;
    while (i < nlen + ndist) {
        int sym = tl_huff_decode(s, &ch);
        if (sym < 0) return 1;
        if (sym < 16) lens[i++] = (unsigned char)sym;
        else {
            int rep, val = 0;
            if (sym == 16) { if (i == 0) return 1; val = lens[i - 1]; rep = 3 + (int)tl_bits(s, 2); }
            else if (sym == 17) rep = 3 + (int)tl_bits(s, 3);
            else rep = 11 + (int)tl_bits(s, 7);
            if (i + rep > nlen + ndist) return 1;
            while (rep--) lens[i++] = (unsigned char)val;
        }
    }
    tl_huff_build(&lh, lens, nlen);
    tl_huff_build(&dh, lens + nlen, ndist);
    return tl_codes(s, &lh, &dh);
}

/* zlib stream -> dst. Returns number of bytes written, or (size_t)-1 on error. */
static size_t tl_zinflate(const unsigned char *src, size_t n, unsigned char *dst, size_t cap)
{
    TlInf s; int last;
    if (n < 2) return (size_t)-1;
    memset(&s, 0, sizeof s);
    s.src = src; s.n = n; s.pos = 2; s.dst = dst; s.cap = cap;
    do {
        int type;
        last = (int)tl_bits(&s, 1); type = (int)tl_bits(&s, 2);
        if (s.err) return (size_t)-1;
        if (type == 0) {
            unsigned len, nlen;
            s.bitbuf = 0; s.bitcnt = 0;
            if (s.pos + 4 > s.n) return (size_t)-1;
            len = s.src[s.pos] | (s.src[s.pos + 1] << 8); nlen = s.src[s.pos + 2] | (s.src[s.pos + 3] << 8);
            s.pos += 4;
            if ((len ^ 0xFFFFu) != nlen || s.pos + len > s.n || s.len + len > s.cap) return (size_t)-1;
            memcpy(s.dst + s.len, s.src + s.pos, len); s.len += len; s.pos += len;
        } else if (type == 1) {
            unsigned char lens[320]; TlHuff lh, dh; int i;
            for (i = 0; i < 144; i++) lens[i] = 8;
            for (; i < 256; i++) lens[i] = 9;
            for (; i < 280; i++) lens[i] = 7;
            for (; i < 288; i++) lens[i] = 8;
            tl_huff_build(&lh, lens, 288);
            for (i = 0; i < 30; i++) lens[i] = 5;
            tl_huff_build(&dh, lens, 30);
            if (tl_codes(&s, &lh, &dh)) return (size_t)-1;
        } else if (type == 2) {
            if (tl_dynamic(&s)) return (size_t)-1;
        } else return (size_t)-1;
    } while (!last);
    return s.len;
}

/* ---------------- PNG ---------------- */
static unsigned tl_be32(const unsigned char *p) { return ((unsigned)p[0] << 24) | ((unsigned)p[1] << 16) | ((unsigned)p[2] << 8) | p[3]; }

static int tl_paeth(int a, int b, int c)
{
    int p = a + b - c, pa = p > a ? p - a : a - p, pb = p > b ? p - b : b - p, pc = p > c ? p - c : c - p;
    if (pa <= pb && pa <= pc) return a;
    return pb <= pc ? b : c;
}

/* Returns malloc'd w*h*4 RGBA8 (R,G,B,A byte order) or NULL. err (optional) gets a short reason. */
static unsigned char *tl_png_decode(const unsigned char *f, size_t n, unsigned *ow, unsigned *oh, const char **err)
{
    static const unsigned char sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    size_t pos = 8, idatlen = 0, rawcap, rawlen, rowbytes, y;
    unsigned w = 0, h = 0, bd = 0, ct = 0, il = 0, x;
    unsigned char *idat = NULL, *raw = NULL, *out = NULL, pal[256][4];
    int npal = 0, have_ihdr = 0, bpp, chans;
    const char *e = "bad PNG";
    memset(pal, 255, sizeof pal);
    if (n < 8 || memcmp(f, sig, 8)) { e = "not a PNG"; goto fail; }
    idat = (unsigned char *)malloc(n);
    if (!idat) goto fail;
    while (pos + 8 <= n) {
        unsigned len = tl_be32(f + pos); const unsigned char *t = f + pos + 4, *d = f + pos + 8;
        if (pos + 12 + (size_t)len > n) break;
        if (!memcmp(t, "IHDR", 4) && len >= 13) {
            w = tl_be32(d); h = tl_be32(d + 4); bd = d[8]; ct = d[9]; il = d[12]; have_ihdr = 1;
        } else if (!memcmp(t, "PLTE", 4)) {
            unsigned i; npal = (int)(len / 3); if (npal > 256) npal = 256;
            for (i = 0; i < (unsigned)npal; i++) { pal[i][0] = d[i*3]; pal[i][1] = d[i*3+1]; pal[i][2] = d[i*3+2]; pal[i][3] = 255; }
        } else if (!memcmp(t, "tRNS", 4) && ct == 3) {
            unsigned i; for (i = 0; i < len && i < 256; i++) pal[i][3] = d[i];
        } else if (!memcmp(t, "IDAT", 4)) {
            memcpy(idat + idatlen, d, len); idatlen += len;
        } else if (!memcmp(t, "IEND", 4)) break;
        pos += 12 + (size_t)len;
    }
    if (!have_ihdr || !w || !h || w > 16384 || h > 16384) { e = "bad size"; goto fail; }
    if (il) { e = "interlaced PNG not supported (re-save it without interlacing)"; goto fail; }
    switch (ct) { case 0: chans = 1; break; case 2: chans = 3; break; case 3: chans = 1; break; case 4: chans = 2; break; case 6: chans = 4; break; default: e = "bad colour type"; goto fail; }
    if (bd != 1 && bd != 2 && bd != 4 && bd != 8 && bd != 16) { e = "bad bit depth"; goto fail; }
    rowbytes = ((size_t)w * chans * bd + 7) / 8;
    bpp = (int)(((unsigned)chans * bd + 7) / 8);
    rawcap = (rowbytes + 1) * h;
    raw = (unsigned char *)malloc(rawcap);
    if (!raw) goto fail;
    rawlen = tl_zinflate(idat, idatlen, raw, rawcap);
    if (rawlen != rawcap) { e = "damaged image data"; goto fail; }
    for (y = 0; y < h; y++) {                                   /* undo the row filters */
        unsigned char *row = raw + y * (rowbytes + 1) + 1, *prev = y ? raw + (y - 1) * (rowbytes + 1) + 1 : NULL;
        int ft = raw[y * (rowbytes + 1)];
        size_t i;
        for (i = 0; i < rowbytes; i++) {
            int a = i >= (size_t)bpp ? row[i - bpp] : 0, b = prev ? prev[i] : 0, c = (prev && i >= (size_t)bpp) ? prev[i - bpp] : 0, v = row[i];
            switch (ft) { case 0: break; case 1: v += a; break; case 2: v += b; break; case 3: v += (a + b) / 2; break; case 4: v += tl_paeth(a, b, c); break; default: e = "bad filter"; goto fail; }
            row[i] = (unsigned char)v;
        }
    }
    out = (unsigned char *)malloc((size_t)w * h * 4);
    if (!out) goto fail;
    for (y = 0; y < h; y++) {
        const unsigned char *row = raw + y * (rowbytes + 1) + 1;
        for (x = 0; x < w; x++) {
            unsigned char *o = out + ((size_t)y * w + x) * 4;
            unsigned c[4] = {0, 0, 0, 255}; int k;
            if (bd >= 8) {
                int step = (int)(bd / 8);
                for (k = 0; k < chans; k++) c[k] = row[((size_t)x * chans + k) * step];   /* 16-bit: keep the high byte */
                if (ct == 0) { c[1] = c[2] = c[0]; }
                else if (ct == 4) { c[3] = c[1]; c[1] = c[2] = c[0]; }
                else if (ct == 3) { unsigned i = c[0]; c[0] = pal[i][0]; c[1] = pal[i][1]; c[2] = pal[i][2]; c[3] = pal[i][3]; }
            } else {
                size_t bit = (size_t)x * bd; unsigned v = (row[bit / 8] >> (8 - bd - (bit % 8))) & ((1u << bd) - 1u);
                if (ct == 3) { c[0] = pal[v][0]; c[1] = pal[v][1]; c[2] = pal[v][2]; c[3] = pal[v][3]; }
                else { c[0] = c[1] = c[2] = v * 255u / ((1u << bd) - 1u); }
            }
            o[0] = (unsigned char)c[0]; o[1] = (unsigned char)c[1]; o[2] = (unsigned char)c[2]; o[3] = (unsigned char)c[3];
        }
    }
    free(idat); free(raw);
    *ow = w; *oh = h;
    return out;
fail:
    free(idat); free(raw); free(out);
    if (err) *err = e;
    return NULL;
}

/* ---------------- mip chain ---------------- */
/* dst gets (max(1,w/2) x max(1,h/2)) RGBA, 2x2 box filter. */
static void tl_box_down(const unsigned char *src, unsigned w, unsigned h, unsigned char *dst)
{
    unsigned nw = w > 1 ? w / 2 : 1, nh = h > 1 ? h / 2 : 1, x, y; int k;
    for (y = 0; y < nh; y++) for (x = 0; x < nw; x++) {
        unsigned x0 = x * 2, y0 = y * 2, x1 = x0 + 1 < w ? x0 + 1 : x0, y1 = y0 + 1 < h ? y0 + 1 : y0;
        for (k = 0; k < 4; k++) {
            unsigned s = src[((size_t)y0 * w + x0) * 4 + k] + src[((size_t)y0 * w + x1) * 4 + k]
                       + src[((size_t)y1 * w + x0) * 4 + k] + src[((size_t)y1 * w + x1) * 4 + k];
            dst[((size_t)y * nw + x) * 4 + k] = (unsigned char)((s + 2) / 4);
        }
    }
}
#endif
