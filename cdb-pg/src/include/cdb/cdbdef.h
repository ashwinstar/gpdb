/*-------------------------------------------------------------------------
 *
 * cdbdef.h
 *	Definitions for use anywhere
 *
 * Copyright (c) 2005, greenplum inc
 *
 * $Id$
 *
 *-------------------------------------------------------------------------
 */
#ifndef CDBDEF_H
#define CDBDEF_H


/*
 * CdbSwap
 *		Exchange the contents of two variables.
 */
#define CdbSwap(_type, _x, _y)	\
	do							\
	{							\
		_type _t = (_x);		\
		_x = (_y);				\
		_y = (_t);				\
	} while (0)


#endif   /* CDBDEF_H */
