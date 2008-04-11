#ifndef LZJB_H
#define LZJB_H

extern size_t lzjb_compress(void *s_start, void *d_start, size_t s_len);
extern size_t lzjb_decompress(void *s_start, void *d_start, size_t s_len, size_t d_len);

#endif /* LZJB_H */
