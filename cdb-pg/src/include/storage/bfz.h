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
};
    

extern bfz_t* bfz_create(bfz_t* bfz, const char* filePrefix, bool compress);
extern void bfz_close(bfz_t* bfz);
extern void bfz_append(bfz_t* bfz, char* inbuf, int inbufsz);
extern void bfz_scan_begin(bfz_t* bfz);
extern int bfz_scan_next(bfz_t* bfz, char* outbuf, int outbufsz);
extern void* bfz_scan_peek(bfz_t* bfz, int size);



#endif /* BFZ_H */
