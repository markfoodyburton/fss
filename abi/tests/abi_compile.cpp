/* Public-header ABI compile test. */
#include <fss/artefact_abi.h>

int main()
{
    return (sizeof(fss_carrier_t) == 40 && sizeof(fss_endpoint_t) == 32) ? 0 : 1;
}
