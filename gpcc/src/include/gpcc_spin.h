#ifndef GPCC_SPIN_H
#define GPCC_SPIN_H

#include <gpcc.h>
#include <apr_atomic.h>


/**
 *  Secure a spin lock. Note: x is a pointer to an apr_uint32_t.
 */
APR_DECLARE(apr_uint32_t*) gpcc_spin_secure(apr_uint32_t* spin) ;


/**
 *  Release a spin lock.
 */
#define gpcc_spin_release(x)  ((void*) ((*(x) = 0), NULL))



#endif /* GPCC_SPIN_H */
