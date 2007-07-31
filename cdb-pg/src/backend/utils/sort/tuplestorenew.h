/*
 * tuplestorenew.h
 * 	A better tuple store
 */

#ifndef TUPSTORE_NEW_H
#define TUPSTORE_NEW_H

/* Opaque data types */
typedef struct NTupleStorePos NTupleStorePos;
typedef struct NTupleStoreAccessor NTupleStoreAccessor;
typedef struct NTupleStore NTupleStore;

/* Instrument tuple store 
 * Caller must ensure ins ptr remain valid during the lifetype of the tuple store 
 */
void ntuplestore_setinstrument(NTupleStore* ts, struct Instrumentation *ins);

/* Tuple store method */
extern NTupleStore *ntuplestore_create(int maxBytes);
extern NTupleStore *ntuplestore_create_readerwriter(const char* filename, int maxBytes, bool isWriter);
extern void ntuplestore_reset(NTupleStore *ts);
extern void ntuplestore_flush(NTupleStore *ts);
extern void ntuplestore_destroy(NTupleStore *ts);

/* Tuple store accessor method 
 * Create Accessor: current we support 1 writer, many reader per store.  After created, the accessor
 * is positioned at the first tuple or eof (if there is no tuple).
 */
extern NTupleStoreAccessor *ntuplestore_create_accessor(NTupleStore *ts, bool init, bool isWriter);
extern void ntuplestore_destroy_accessor(NTupleStoreAccessor *acc);

/* Put slot/data automatically postion the accessor to the last entry */
extern void ntuplestore_acc_put_tupleslot(NTupleStoreAccessor *tsa, TupleTableSlot *slot);
extern void ntuplestore_acc_put_data(NTupleStoreAccessor *tsa, void *data, int len);

/* Trim will trim tuples upto but not include the pos.  After trim, the accessor is postioned
 * at pos.  Trim changes tuplestore state so if there are other accessors opend, it is caller's
 * reponsibility to make sure trim does not invalid other accessors.
 */
extern void ntuplestore_acc_trim(NTupleStoreAccessor *tsa, NTupleStorePos *pos);

/* return true if sucess, false if beyond last (first) valid position */
extern bool ntuplestore_acc_advance(NTupleStoreAccessor *tsa, int n);

/* Get data.  The slot/pointer returned is guaranteed to be valid till the accessor
 * call advance.
 * NOTE: trim may make current postion of an accessor invalid.  It is caller's reponsibilty
 * to make sure trim does not trim too far ahead
 */
extern void ntuplestore_acc_current_tupleslot(NTupleStoreAccessor *tsa, TupleTableSlot *slot);
extern void ntuplestore_acc_current_data(NTupleStoreAccessor *tsa, void **data, int *len);

/* Tell/seek postion of accessor.  Tell may return NULL if next/prev goes beyond last entry */
extern NTupleStorePos *ntuplestore_acc_tell(NTupleStoreAccessor *tsa);

/* put the current row at a certain pos (or first/last).  Return false pos is invalid */
extern bool ntuplestore_acc_seek(NTupleStoreAccessor *tsa, NTupleStorePos *pos);
extern bool ntuplestore_acc_seek_first(NTupleStoreAccessor *tsa);
extern bool ntuplestore_acc_seek_last(NTupleStoreAccessor *tsa);

#endif /* TUPSTORE_NEW_H */
