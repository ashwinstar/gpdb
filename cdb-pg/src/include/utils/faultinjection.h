/* 
 * Fault injection utilities 
 * 
 * Copyright (c) 2008, Greenplum Inc.
 */

#ifndef GP_FAULT_INJECTION_H
#define GP_FAULT_INJECTION_H

/* Fault group 1: fault from user calling gp_fault_inject(fault_code, arg) */
#define GP_FAULT_USER 1
#define GP_FAULT_USER_SEGV 				1 	/* Seg fault */
#define GP_FAULT_USER_LEAK 				2 	/* Leaking some memory */
#define GP_FAULT_USER_LEAK_TOP  		3 	/* Leaking from top context */
#define GP_FAULT_USER_RAISE_ERROR 		4	/* elog a error */
#define GP_FAULT_USER_RAISE_FATAL 		5 	/* elog a fatal */
#define GP_FAULT_USER_RAISE_PANIC 		6 	/* elog a panic */
#define GP_FAULT_USER_PROCEXIT          7   /* Simply call proc_exit  */
#define GP_FAULT_USER_ABORT             8   /* Simply call abort */

/* Fault group 2: From User, cause some fault in MP */
#define GP_FAULT_USER_MP_CONFIG 		100	/* Show vmem_max guc */
#define GP_FAULT_USER_MP_ALLOC 	 		101 /* Show allocation */
#define GP_FAULT_USER_MP_HIGHWM 		102 /* Show high water mark */
#define GP_FAULT_SEG_AVAILABLE 			103 /* Show segment available */
#define GP_FAULT_SEG_SET_VMEMMAX 		104 /* Set new vmem max */
#define GP_FAULT_SEG_GET_VMEMMAX 		105 /* Get current vmem max */

#ifdef USE_FAULT_INJECTION
extern int64 gp_fault_inject_impl(int32 reason, int64 arg); 
extern int64 gp_mp_fault(int32 reason, int64 arg);
#endif

#endif

