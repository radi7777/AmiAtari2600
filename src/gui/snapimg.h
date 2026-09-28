/*
 * snapimg.h - screenshots prepared once for quick display.
 *
 * A downloaded snap (PNG, up to 512 x 384, uneven borders) is converted a
 * single time: the border is cut off, the rest is reduced to fit
 * SNAPIMG_MAXW x SNAPIMG_MAXH and stored as a palette plus PackBits-packed
 * colour indices (snaps/<CRC>.a26i, usually a few KB). Showing it then
 * only means reading that file and scaling 8-bit indices; no PNG decoding
 * and no datatypes while browsing the list.
 *
 * Portable C; the decoding of the PNG is done by the caller (getrow).
 */
#ifndef A26_SNAPIMG_H
#define A26_SNAPIMG_H

#define SNAPIMG_MAXW 320
#define SNAPIMG_MAXH 240

typedef struct {
    int w, h;                   /* square pixels */
    int ncol;                   /* palette entries, 1..256 */
    unsigned char pal[256 * 3]; /* r, g, b */
    unsigned char *pix;         /* w * h indices */
} SnapImg;

/* delivers row y of the source picture as w r,g,b triples */
typedef int (*SnapGetRow)(void *ctx, int y, unsigned char *rgb);

/* border cut, reduction and palette; returns 0 on success */
int  snapimg_from_rgb(SnapImg *im, int w, int h, SnapGetRow getrow, void *ctx);
int  snapimg_save(const char *path, const SnapImg *im);
int  snapimg_load(const char *path, SnapImg *im);
void snapimg_free(SnapImg *im);

#endif
