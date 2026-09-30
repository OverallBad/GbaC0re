/* See ps5_tilemap.h. Tiling algorithm from ps5-payload-dev/SDL's
 * PS5_Tilemap (John Tornblom, 2026); AVX2 fast path kept verbatim. */

#include "ps5_tilemap.h"

#include <stdlib.h>
#include <string.h>
#include <immintrin.h>

#define TILE_WIDTH  512
#define TILE_HEIGHT 128
#define TILE_SIZE   (TILE_WIDTH * TILE_HEIGHT)
#define TILE_BAND   8

struct GbaTilemap {
    uint32_t width;
    uint32_t height;
    uint32_t *colx;
    uint32_t yoff[TILE_HEIGHT];
};

static uint32_t tile_offset(uint32_t x, uint32_t y) {
    return (((x) & 1u) << 0 |
            ((x >>  1) & 1u) << 1 |
            ((y >>  0) & 1u) << 2 |
            ((y >>  1) & 1u) << 3 |
            ((y >>  2) & 1u) << 4 |
            ((x >>  2) & 1u) << 5 |
            (((x >> 3) ^ (y >> 3)) & 1u) << 6 |
            (((x >> 4) ^ (y >> 4)) & 1u) << 7 |
            (((x >> 6) ^ (y >> 5)) & 1u) << 8 |
            (((x >> 5) ^ (y >> 6)) & 1u) << 9 |
            ((y >>  3) & 1u) << 10 |
            ((x >>  4) & 1u) << 11 |
            ((y >>  6) & 1u) << 12 |
            ((x >>  6) & 1u) << 13 |
            ((x >>  7) & 1u) << 14 |
            ((x >>  8) & 1u) << 15);
}

static uint32_t tile_pixel(uint32_t x, uint32_t y, uint32_t width) {
    return ((x / TILE_WIDTH) * TILE_SIZE +
            (y / TILE_HEIGHT) * (TILE_HEIGHT * width) +
            ((tile_offset((x % TILE_WIDTH), 0) ^
              tile_offset(0, (y % TILE_HEIGHT)))));
}

size_t gba_tilemap_buffer_size(uint32_t width, uint32_t height) {
    uint32_t last_band = ((height - 1) / TILE_HEIGHT) * (TILE_HEIGHT * width);
    uint32_t last_tile = ((width - 1) / TILE_WIDTH) * TILE_SIZE;
    return (size_t)(last_band + last_tile + TILE_SIZE) * sizeof(uint32_t);
}

GbaTilemap *gba_tilemap_create(uint32_t width, uint32_t height) {
    GbaTilemap *tmap = (GbaTilemap *)malloc(sizeof(GbaTilemap));
    uint32_t x, y;
    if (!tmap) return NULL;

    tmap->colx = (uint32_t *)malloc(sizeof(uint32_t) * (width / 4 + 1));
    if (!tmap->colx) {
        free(tmap);
        return NULL;
    }

    for (x = 0; x < width; x += 4) {
        tmap->colx[x / 4] = (x / TILE_WIDTH) * TILE_SIZE +
            tile_offset((x % TILE_WIDTH), 0);
    }
    for (y = 0; y < TILE_HEIGHT; y++) {
        tmap->yoff[y] = tile_offset(0, y);
    }

    tmap->width = width;
    tmap->height = height;
    return tmap;
}

__attribute__((target("avx2")))
static void tile_area(const GbaTilemap *tmap,
                      const uint32_t *src, uint32_t pitch, uint32_t *dst,
                      uint32_t x0, uint32_t x1, uint32_t y0, uint32_t y1) {
    const uint32_t *colx = tmap->colx;
    const uint32_t width = tmap->width;
    const uint32_t quads = x1 / 4;
    const uint32_t *p;
    const uint32_t *s;
    uint32_t rows;
    uint32_t base;
    uint32_t yo;
    uint32_t *d;
    uint32_t *o;
    __m256i v;
    uint32_t x;
    uint32_t y;
    uint32_t k;
    uint32_t g;

    for (y = y0; y < y1; y += TILE_BAND) {
        base = (y / TILE_HEIGHT) * (TILE_HEIGHT * width);
        yo = tmap->yoff[y % TILE_HEIGHT];
        s = src + y * pitch;
        d = dst + base;
        rows = y1 - y < TILE_BAND ? y1 - y : TILE_BAND;

        for (g = x0 / 4; g < quads; g++) {
            p = s + (g << 2);
            o = d + (colx[g] ^ yo);

            for (k = 0; k + 1 < rows; k += 2) {
                v = _mm256_inserti128_si256(_mm256_castsi128_si256(_mm_loadu_si128((const __m128i *)p)),
                                            _mm_loadu_si128((const __m128i *)(p + pitch)), 1);
                _mm256_stream_si256((__m256i *)o, v);
                p += 2 * pitch;
                o += 8;
            }

            if (rows & 1) {
                _mm_stream_si128((__m128i *)o, _mm_loadu_si128((const __m128i *)p));
            }
        }

        for (k = 0; k < rows; k++) {
            p = s + k * pitch;
            for (x = quads * 4; x < x1; x++) {
                dst[tile_pixel(x, y + k, width)] = p[x];
            }
        }
    }
    _mm_sfence();
}

void gba_tilemap_blit_full(GbaTilemap *tmap, const uint32_t *src,
                           int pitch, uint32_t *dst) {
    /* Full frame: x must be a multiple of 4 (true for 1920), y a multiple
       of TILE_BAND (1080 = 135*8). */
    tile_area(tmap, src, (uint32_t)pitch, dst,
              0, tmap->width, 0, tmap->height);
}

void gba_tilemap_destroy(GbaTilemap *tmap) {
    if (!tmap) return;
    free(tmap->colx);
    free(tmap);
}
