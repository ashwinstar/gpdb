/* chubby_string.h */

#ifndef CHUBBY_STRING_H
#define CHUBBY_STRING_H

typedef struct{
    int length;
    char*a;
}chubby_string_t;

/* chubby_udp_buffer_t extends chubby_string_t. */
typedef struct{
    int length;
    char*a;
    char aa[65536];
}chubby_udp_buffer_t;

void chubby_string_free(chubby_string_t*thiz);
chubby_string_t*chubby_string_format(chubby_string_t*thiz,const char*format,...);

chubby_udp_buffer_t*chubby_udp_buffer_init(chubby_udp_buffer_t*thiz);
int chubby_udp_buffer_isOverflowed(chubby_udp_buffer_t*thiz);
void chubby_udp_buffer_format(chubby_udp_buffer_t*thiz,char*format,...);
void chubby_udp_buffer_cat(chubby_udp_buffer_t*thiz,chubby_string_t*a);

#endif
