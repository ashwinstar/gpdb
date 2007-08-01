/*
 * tuplestorenew.h
 * 	A better tuple store
 */

#ifndef TUPSTORE_NEW_H
#define TUPSTORE_NEW_H

/* Opaque data types */
typedef struct NTupleStorePos NTupleStorePos;
typedef struct NTupStoreAccessor NTupleStoreAccessor;
typedef struct NTupleStore NTupleStore;

/* Instrument tuple store 
 * Caller must ensure ins ptr remain valid during the lifetype of the tuple store 
 */
void ntuplestore_setinstrument(NTupleStore* ts, struct Instrumentation *ins);

/* Tuple store method */
extern NTupleStore *ntuplestore_create(bool interXact, int maxBytes);
extern NTupleStore *ntuplestore_create_readerwriter(const char* filename, bool isWriter);
extern void ntuplestore_reset(NTupleStore *ts);
extern void ntuplestore_flush(NTupleStore *ts);
extern void ntuplestore_destroy(NTupleStore *ts);

/* Tuple store accessor method 
 * Create Accessor: current we support 1 writer, many reader per store 
 */
extern NTupleStoreAccessor *ntuplestore_create_accessor(NTupleStore *ts, bool isWriter);

/* Put slot automatically postion the accessor to the last entry */
extern void ntuplestore_acc_put_tupleslot(NTupleStoreAccessor *tsa, TupleTableSlot *slot);
extern void ntuplestore_acc_put_data(NTupleStoreAccessor *tsa, void *data, int len);

/* Trim will trim tuples upto but not include the pos.  After trim, the accessor is postioned
 * at pos
 */
extern void ntuplestore_acc_trim(NTupleStoreAccessor *tsa, NTupleStorePos *pos);

/* return true if sucess, false if beyond last (first) valid position */
extern bool ntuplestore_acc_next(NTupleStoreAccessor *tsa);
extern bool ntuplestore_acc_prev(NTupleStoreAccessor *tsa);

/* Get data.  The slot/pointer returned is guaranteed to be valid till the accessor
 * call next/prev or put.
 * NOTE: trim may make current postion of an accessor invalid.  It is caller's reponsibilty
 * to make sure trim does not trim too far ahead
 */
extern void ntuplestore_acc_current_tupleslot(NTupleStoreAccessor *tsa, TupleTableSlot *slot);
extern void ntuplestore_acc_current_data(NTupleStoreAccessor *tsa, void **data, int *len);

/* Tell/seek postion of accessor.  Tell may return NULL if next/prev goes beyond last entry */
extern NTupleStorePos *ntuplestore_acc_tell(NTupleStoreAccessor *tsa);
extern bool ntuplestore_acc_seek(NTupleStoreAccessor *tsa, NTupleStorePos *pos);

#endif /* TUPSTORE_NEW_H */
