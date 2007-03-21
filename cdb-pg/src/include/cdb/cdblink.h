/*
 * cdblink.h
 *
 * Functions returning results from a remote database
 *
 * Joe Conway <mail@joeconway.com>
 * And contributors:
 * Darko Prenosil <Darko.Prenosil@finteh.hr>
 * Shridhar Daithankar <shridhar_daithankar@persistent.co.in>
 *
 * Copyright (c) 2001-2003, PostgreSQL Global Development Group
 * ALL RIGHTS RESERVED;
 *
 * Permission to use, copy, modify, and distribute this software and its
 * documentation for any purpose, without fee, and without a written agreement
 * is hereby granted, provided that the above copyright notice and this
 * paragraph and the following two paragraphs appear in all copies.
 *
 * IN NO EVENT SHALL THE AUTHOR OR DISTRIBUTORS BE LIABLE TO ANY PARTY FOR
 * DIRECT, INDIRECT, SPECIAL, INCIDENTAL, OR CONSEQUENTIAL DAMAGES, INCLUDING
 * LOST PROFITS, ARISING OUT OF THE USE OF THIS SOFTWARE AND ITS
 * DOCUMENTATION, EVEN IF THE AUTHOR OR DISTRIBUTORS HAVE BEEN ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 * THE AUTHOR AND DISTRIBUTORS SPECIFICALLY DISCLAIMS ANY WARRANTIES,
 * INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY
 * AND FITNESS FOR A PARTICULAR PURPOSE.  THE SOFTWARE PROVIDED HEREUNDER IS
 * ON AN "AS IS" BASIS, AND THE AUTHOR AND DISTRIBUTORS HAS NO OBLIGATIONS TO
 * PROVIDE MAINTENANCE, SUPPORT, UPDATES, ENHANCEMENTS, OR MODIFICATIONS.
 *
 */

#ifndef CDBLINK_H
#define CDBLINK_H


/*
 * Called by backend/utils/init/postinit.c to setup the connections to the
 * tails and store them for the duration of the session.  Currently, since each
 * user session has their own process, all of the information is stored in
 * global or static variables.
 */
extern void cdblink_setup(void);


/*
 * Returns the number of segments
 *
 * N.B.  Mpp_role must be either dispatch or execute, since
 * when utiliy	no mpp catalog tables are read.  An Assert is
 * thrown if Mpp_role = utility.
 */
extern int	getgpsegmentCount(void);

extern void buildMirrorQDDefinition(void);

extern void *getEntrySegDB(int *);

extern bool isQDMirroringEnabled(void);

extern void write_qd_sync(char *, void *, int);

extern void disconnectMirrorQD(void);

#endif   /* CDBLINK_H */
