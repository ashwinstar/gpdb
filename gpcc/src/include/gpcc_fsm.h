#ifndef GPCC_FSM_H
#define GPCC_FSM_H

#include <gpcc.h>

/** action function */
typedef apr_status_t gpcc_fsm_func_t(int channel, void* ev);

/** 
 *  Create a fsm edge 
 *  @param curstate current state 
 *  @param evtype the event type 
 *  @param nxtstate next state to go to after applying funcs without error
 *  @param func first function to call to effect transit to nxtstate
 *  @param ... subsequent functions to call, NULL terminated.
 */
APR_DECLARE(apr_status_t) gpcc_fsm_put(int curstate, int evtype,
                                       int nxtstate,
                                       gpcc_fsm_func_t* func, ...);

/**
 *  Lookup a fsm transform
 *  @param curstate the current state
 *  @param evtype the event 
 *  @param pnxstate OUT: the next state to go on success
 *  @param pfunc OUT: a NULL terminated array of func pointers
 */
APR_DECLARE(apr_status_t) gpcc_fsm_get(int curstate, int evtype,
                                       int* pnxtstate,
                                       gpcc_fsm_func_t*** pfunc);



#endif /* GPCC_FSM_H */
