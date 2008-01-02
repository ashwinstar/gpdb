#ifndef BFZ_H
#define BFZ_H

#define BFZ_MODE_CLOSED   0
#define BFZ_MODE_APPEND   1
#define BFZ_MODE_SCAN     2


typedef struct bfz_t bfz_t;
struct bfz_t {
    int fp;
    int compress;
    int mode;
    char buf[BLCKSZ];
    int bot, top;
    int eof;
    uint64 tot_bytes;
	uint64 tot_compressed;
};
    

extern bfz_t* bfz_create(bfz_t* bfz, const char* filePrefix, bool compress);
extern void bfz_close(bfz_t* bfz);
extern void bfz_append_ex(bfz_t* bfz, char* inbuf, int inbufsz);
extern void bfz_scan_begin(bfz_t* bfz);
extern int bfz_scan_next_ex(bfz_t* bfz, char* outbuf, int outbufsz);

static inline void* bfz_scan_peek(bfz_t* bfz, int size)
{
	if (bfz->mode == BFZ_MODE_SCAN &&
		bfz->top - bfz->bot >= size)
	{
		bfz->bot += size;
		return bfz->buf + bfz->bot - size;
	}
	return 0;
}


static inline void bfz_append(bfz_t* bfz, char* inbuf, int inbufsz)
{
    if (bfz->mode == BFZ_MODE_APPEND &&
		sizeof(bfz->buf) - bfz->top >= inbufsz) 
    {
		memcpy(bfz->buf + bfz->top, inbuf, inbufsz);
		bfz->top += inbufsz;
    }
	else
		bfz_append_ex(bfz, inbuf, inbufsz);
}


static inline int bfz_scan_next(bfz_t* bfz, char* outbuf, int outbufsz)
{
    if (bfz->mode == BFZ_MODE_SCAN && 
		bfz->top - bfz->bot >= outbufsz) 
    {
		memcpy(outbuf, bfz->buf + bfz->bot, outbufsz);
		bfz->bot += outbufsz;
		return outbufsz;
    }
    return bfz_scan_next_ex(bfz, outbuf, outbufsz);
}


#endif /* BFZ_H */
