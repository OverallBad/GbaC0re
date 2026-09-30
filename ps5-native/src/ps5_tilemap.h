/* GbaC0re PS5 native — linear-to-tiled blitter for SceVideoOut buffers.
 *
 * The PS5 display hardware reads video-out buffers in a tiled (swizzled)
 * layout: 512x128-pixel tiles with a bit-interleaved pixel order. Writing
 * linear scanlines into a registered buffer shows garbage; pixels must be
 * scattered through this tile map.
 *
 * Algorithm adapted from PS5_Tilemap in ps5-payload-dev/SDL
 * (src/video/ps5/SDL_ps5tilemap.c), copyright 2026 John Tornblom, used
 * under that file's permissive license (retain this attribution).
 * Changes: GbaC0re naming, libc instead of SDL helpers, full-frame-only
 * blit (no damage rectangles — we redraw every frame anyway).
 */

#ifndef GBA_TILEMAP_H
#define GBA_TILEMAP_H

#include <stddef.h>
#include <stdint.h>

typedef struct GbaTilemap GbaTilemap;

/* Bytes needed for one tiled WxH buffer. */
size_t gba_tilemap_buffer_size(uint32_t width, uint32_t height);

GbaTilemap *gba_tilemap_create(uint32_t width, uint32_t height);
void gba_tilemap_destroy(GbaTilemap *tmap);

/* Scatter a full linear frame (src, pitch u32 pixels per row) into the
 * tiled buffer dst. src and dst must not overlap. */
void gba_tilemap_blit_full(GbaTilemap *tmap, const uint32_t *src,
                           int pitch, uint32_t *dst);

#endif
