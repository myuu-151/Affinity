// Affinity PSP runtime — texture conversion to swizzled 16-bit (5551).
// Halves texture-read bandwidth (the real-hardware fill bottleneck) and lays
// the data out swizzled so the GE texture cache doesn't thrash.
#pragma once
// RGBA8888 (0xAABBGGRR) -> swizzled 5551. 16-byte-aligned buffer, or 0 on OOM.
unsigned short* psp_make_tex16(const unsigned int* src, int w, int h);
