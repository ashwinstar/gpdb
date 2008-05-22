#include "postgres.h"
#include "fmgr.h"
#include "funcapi.h"

typedef struct {
	text *gp_map_input_key;
	text *gp_map_input_value;
	bool  gp_map_firstcall;
} gp_map_internal_fstate_t;


#define GPMAP_IS_FIRSTCALL()							                \
	gp_map_internal_fstate->gp_map_firstcall == true

#define GPMAP_INIT(state, ssize, key, keytype, value, valuetype)		\
	FuncCallContext          *gp_map_fcall_ctx;							\
	gp_map_internal_fstate_t *gp_map_internal_fstate;					\
	do {																\
		if (SRF_IS_FIRSTCALL())											\
		{							    								\
			MemoryContext gp_map_old_memctx;							\
			gp_map_fcall_ctx = SRF_FIRSTCALL_INIT();					\
			gp_map_old_memctx = MemoryContextSwitchTo(                  \
               gp_map_fcall_ctx->multi_call_memory_ctx);				\
			if (get_call_result_type(fcinfo, NULL,                      \
                &gp_map_fcall_ctx->tuple_desc) != TYPEFUNC_COMPOSITE)   \
				ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), \
				  errmsg("function returning record called in context " \
						 "that cannot accept type record")));			\
			gp_map_internal_fstate =									\
				palloc(sizeof(gp_map_internal_fstate_t) + ssize);		\
			gp_map_internal_fstate->gp_map_input_key =					\
				PG_GETARG_TEXT_P(0);									\
			gp_map_internal_fstate->gp_map_input_value =				\
				PG_GETARG_TEXT_P(1);									\
			gp_map_internal_fstate->gp_map_firstcall = true;            \
			gp_map_fcall_ctx->user_fctx = gp_map_internal_fstate;		\
			gp_map_fcall_ctx->tuple_desc = BlessTupleDesc(				\
               gp_map_fcall_ctx->tuple_desc);                           \
			MemoryContextSwitchTo(gp_map_old_memctx);					\
		}																\
		gp_map_fcall_ctx = SRF_PERCALL_SETUP();							\
		gp_map_internal_fstate = gp_map_fcall_ctx->user_fctx;			\
		state = (void *) (gp_map_internal_fstate+1);					\
		key = gp_map_internal_fstate->gp_map_input_key;					\
		value = gp_map_internal_fstate->gp_map_input_value;				\
    } while(0)

#define GPMAP_DONE() SRF_RETURN_DONE(gp_map_fcall_ctx)

#define GPMAP_RETURN_NEXT(state, key, keytype, value, valuetype)		\
    do {																\
		Datum       gp_map_output[2];									\
		bool        gp_map_isnull[2] = {false,false};					\
		Datum       gp_map_result;										\
		HeapTuple   gp_map_heaptup;										\
		gp_map_fcall_ctx->user_fctx = state;							\
		gp_map_output[0] = PointerGetDatum(key);						\
		gp_map_output[1] = PointerGetDatum(value);						\
		gp_map_heaptup =												\
			heap_form_tuple(gp_map_fcall_ctx->tuple_desc,				\
							gp_map_output, gp_map_isnull);				\
		gp_map_result = HeapTupleGetDatum(gp_map_heaptup);				\
		gp_map_internal_fstate->gp_map_firstcall = false;				\
		SRF_RETURN_NEXT(gp_map_fcall_ctx, gp_map_result);				\
	} while (0)
