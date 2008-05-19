/* 
 * Fault injection utilities 
 * 
 * Copyright (c) 2008, Greenplum Inc.
 */

#ifndef GP_FAULT_INJECTION_H
#define GP_FAULT_INJECTION_H

/* Fault group 1: fault from user calling gp_fault_inject(fault_code, arg) */
#define GP_FAULT_USER 1
#define GP_FAULT_USER_SEGV 				1 	 		/* Seg fault */
#define GP_FAULT_USER_LEAK 				(1 << 1) 	/* Leaking some memory */
#define GP_FAULT_USER_LEAK_TOP  		(1 << 2) 	/* Leaking from top context */
#define GP_FAULT_USER_RAISE_ERROR 		(1 << 3)	/* elog a error */
#define GP_FAULT_USER_RAISE_FATAL 		(1 << 4) 	/* elog a fatal */
#define GP_FAULT_USER_RAISE_PANIC 		(1 << 5) 	/* elog a panic */

/* Fault group 2: Memory proection subsystem fault */
#define GP_FAULT_MP 2
#define GP_FAULT_MP_SEGV 				1 			/* Cause MP RPC Server Segv */
#define GP_FAULT_MP_ERROR 				(1 << 1)    /* Cause MP RPC Server elog error */
#define GP_FAULT_MP_FATAL				(1 << 2)    /* Cause MP RPC Server elog fatal */
#define GP_FAULT_MP_PANIC 				(1 << 3)    /* Cause MP RPC Server elog panic */
#define GP_FAULT_MP_HANG 				(1 << 4)    /* Cause MP RPC Server hang */

#ifdef USE_FAULT_INJECTION
extern int gp_fault_inject_impl(int majorcode, int minorcode, int arg, void *parg);
#define GP_FAULT_INJECT(major, minor, arg, parg) gp_fault_inject_impl(major, minor, arg, parg)
#else
#define GP_FAULT_INJECT(major, minor, arg)
#endif

#endif

