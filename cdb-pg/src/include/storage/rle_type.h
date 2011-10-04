/*
 * Copyright (c) 2011 EMC Corporation All Rights Reserved
 *
 * This software is protected, without limitation, by copyright law
 * and international treaties. Use of this software and the intellectual
 * property contained therein is expressly limited to the terms and
 * conditions of the License Agreement under which it is provided by
 * or on behalf of EMC.
 *
 * ---------------------------------------------------------------------
 */

#ifndef RLE_TYPE_H
#define RLE_TYPE_H


extern int
rle_type_comp(int typeLen_bytes, 
			  Oid colType, 
			  char align,
			  char * inbuf, 
			  int inSizeBytes, 
			  char * outbuf, 
			  int outbufSizeBytes, 
			  int * outSizeBytes);

extern int
rle_type_decomp(int typeLen_bytes, 
				Oid colType, 
				char align,
				char * inbuf, 
				int inSizeBytes, 
				char * outbuf, 
				int outbufSizeBytes, 
				int * outSizeBytes);


#endif  /* RLE_TYPE_H */
