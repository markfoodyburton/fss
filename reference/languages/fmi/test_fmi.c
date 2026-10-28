/* SPDX-License-Identifier: BSD-3-Clause */

#include "fmi.h"

int main(void)
{
    uint32_t u32 = 1;
    uint64_t u64 = 1;
    int active = 1;
    const void *bytes = &u32;
    size_t len = 1;

    if (fss_fmi_open(NULL) != NULL) return 1;
    if (fss_fmi_get_u32_status(NULL, 0, &u32) != FSS_FMI_STATUS_ERROR || u32) return 1;
    if (fss_fmi_get_u64_status(NULL, 0, &u64) != FSS_FMI_STATUS_ERROR || u64) return 1;
    if (fss_fmi_get_clock_status(NULL, 0, &active) != FSS_FMI_STATUS_ERROR || active) return 1;
    if (fss_fmi_get_binary_status(NULL, 0, &bytes, &len) != FSS_FMI_STATUS_ERROR ||
        bytes || len) return 1;
    return 0;
}
