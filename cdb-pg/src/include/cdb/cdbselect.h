/*-------------------------------------------------------------------------
 *
 * cdbselect.h
 *
 *	  Provides equivalents to FD_SET/FD_ZERO -- but handle more than
 *  1024 file descriptors (as far as I can tell all of our platforms
 *  support 65536).
 *
 * Copyright (c) 2006, greenplum inc
 *
 * $Id$
 *
 *-------------------------------------------------------------------------
 */
#ifndef CDBSELECT_H
#define CDBSELECT_H

/* 8-bit bytes 32 or 64-bit ints */

typedef long int mpp_fd_set[65536/(sizeof(long int) * 8)];

#define MPP_FD_ZERO(setp) (memset((char *)setp, 0, sizeof(mpp_fd_set)))

#define MPP_FD_WORD(fd) (fd / (sizeof(long int) * 8))
#define MPP_FD_BIT(fd) (1 << (fd % (sizeof(long int) * 8)))

#define MPP_FD_SET(fd, set) (set[MPP_FD_WORD(fd)] = (set[MPP_FD_WORD(fd)]) | MPP_FD_BIT(fd))
#define MPP_FD_CLR(fd, set) (set[MPP_FD_WORD(fd)] &= ~MPP_FD_BIT(fd))

#define MPP_FD_ISSET(fd, set) ((set[MPP_FD_WORD(fd)] & MPP_FD_BIT(fd)) ? 1 : 0)

#endif
