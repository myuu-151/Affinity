// Affinity PSP runtime — texture conversion (see tex.h).
#include "tex.h"
#include <stdlib.h>
#include <malloc.h>

static void swizzle_bytes(unsigned char* out, const unsigned char* in, int bw, int h) {
    int rowblocks = bw / 16;
    for (int j = 0; j < h; j++)
        for (int i = 0; i < bw; i++) {
            int bx = i >> 4, by = j >> 3;
            int blk = bx + by * rowblocks;
            out[blk * 128 + (j & 7) * 16 + (i & 15)] = in[j * bw + i];
        }
}

unsigned short* psp_make_tex16(const unsigned int* src, int w, int h) {
    int n = w * h;
    unsigned short* lin = (unsigned short*)malloc(n * 2);
    if (!lin) return 0;
    for (int i = 0; i < n; i++) {
        unsigned int c = src[i];
        unsigned r = c & 0xFF, g = (c >> 8) & 0xFF, b = (c >> 16) & 0xFF, a = (c >> 24) & 0xFF;
        lin[i] = (unsigned short)((r >> 3) | ((g >> 3) << 5) | ((b >> 3) << 10) | ((a >= 128 ? 1 : 0) << 15));
    }
    unsigned short* sw = (unsigned short*)memalign(16, n * 2);
    if (sw) swizzle_bytes((unsigned char*)sw, (unsigned char*)lin, w * 2, h);
    free(lin);
    return sw;
}
