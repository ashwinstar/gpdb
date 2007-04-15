#ifndef GPCC_MSG_H
#define GPCC_MSG_H

#include <gpcc_net.h>

typedef struct gpcc_msg_t gpcc_msg_t;
struct gpcc_msg_t {
    int pkttype;
    int cmdidx;
    int pktlen;
    char* pkt;
};





#endif /* GPCC_MSG_H */
