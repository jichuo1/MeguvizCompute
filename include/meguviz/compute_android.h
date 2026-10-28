#ifndef MEGUVIZ_COMPUTE_ANDROID_H
#define MEGUVIZ_COMPUTE_ANDROID_H
#include "compute.h"

#ifdef __cplusplus
extern "C" {
#endif
struct AHardwareBuffer;
typedef struct MgcAndroidImportDesc {
    MgcHeader header;
    struct AHardwareBuffer *buffer;
    int32_t acquire_fence_fd;
    uint32_t required_flags;
    uint64_t allocation_id;
    uint64_t allocation_generation;
    uint64_t content_id;
    uint32_t access;
    uint32_t reserved[3];
} MgcAndroidImportDesc;
/* Synchronous call borrows caller AHB/fd. Success acquires an independent AHB
 * object reference and duplicates fd; caller retains both originals. Failure
 * unwinds only internal acquisitions. -1 means dependency already satisfied.
 * Object acquisition never grants producer content-reuse permission. */
MGC_API MgcStatus mgc_android_import(MgcDomain *domain, const MgcAndroidImportDesc *desc, MgcLease **lease);
/* Success transfers a new owned fd to caller; -1 means already satisfied.
 * Non-success returns no fd. Only actually enabled/probed paths may export. */
MGC_API MgcStatus mgc_android_export_release_fence(MgcLease *lease, int32_t *owned_fd);
#ifdef __cplusplus
}
#endif
#endif
