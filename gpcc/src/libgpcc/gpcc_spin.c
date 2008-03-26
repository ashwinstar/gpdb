#include <gpcc_spin.h>


APR_DECLARE(apr_uint32_t*) gpcc_spin_secure(apr_uint32_t* spin) 
{
    do ; while (0 != apr_atomic_xchg32(spin, 1));
    return spin;
}
    
