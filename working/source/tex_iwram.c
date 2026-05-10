// Texture cache in IWRAM for fast pixel reads (0 wait states vs 2 in EWRAM)
#include <tonc.h>

#define TEX_IWRAM_SIZE 4096
IWRAM_DATA u8 tex_iwram[TEX_IWRAM_SIZE];
