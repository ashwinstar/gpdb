/* 
 * memprot_imph.h
 * 		Memory Protect Server Implementation.
 *
 * Copyright (c) 2008, Greenplum Inc.
 */

#ifndef GP_MEMPROT_IMPL_H
#define GP_MEMPROT_IMPL_H

typedef struct MemProtServer_ImplConfig
{
	int64 vmem_max;
	int64 vmem_alloc;
} MemProtServer_ImplConfig;

extern void memprot_init_impl_config(void);
extern int memprot_service_request(MemProtectRequest *req, MemProtectResponse *response);
#endif
