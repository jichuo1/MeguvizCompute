#ifndef MEGUVIZ_COMPUTE_PRIVATE_JOB_H
#define MEGUVIZ_COMPUTE_PRIVATE_JOB_H
#include <meguviz/compute.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Private source SDK, exact build fingerprint required. Runs on the domain's
 * serial execution slot. The closure owns all other objects until release.
 * The array is copied; each entry transfers one reference only on ACCEPTED.
 * No GPU capability is implied by a host job. */
typedef MgcStatus (*MgcHostJobFn)(void *context, MgcTicket *ticket);
typedef struct MgcHostJobDesc {
    MgcHeader header;
    uint64_t request_id;
    MgcLease *const *leases;
    uint32_t lease_count;
    uint32_t required_flags;
    MgcHostJobFn execute;
    MgcCompletionFn completion;
    void *context;
    MgcContextFn retain_context;
    MgcContextFn release_context;
} MgcHostJobDesc;
MgcStatus mgc_private_submit_host_job(MgcSession *session, const MgcHostJobDesc *desc, MgcTicket **ticket);
uint32_t mgc_private_ticket_cancelled(MgcTicket *ticket);
/* Returned domain is retained by the caller, allowing immutable plans to pin
 * it. It is not a borrowed raw VkDevice. */
MgcStatus mgc_private_session_domain(MgcSession *session, MgcDomain **domain);
#ifdef __cplusplus
}
#endif
#endif
