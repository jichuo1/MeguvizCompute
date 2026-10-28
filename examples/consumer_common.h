#ifndef MGC_CONSUMER_COMMON_H
#define MGC_CONSUMER_COMMON_H
#include <meguviz/compute.h>
#include <stdio.h>
#include <string.h>
#define CHECK(x)                                                                                   \
    do                                                                                             \
    {                                                                                              \
        if (!(x))                                                                                  \
        {                                                                                          \
            fprintf(stderr, "check failed line %d: %s\n", __LINE__, #x);                           \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)
static MgcStatus make_resource(MgcDomain *d, uint64_t id, uint64_t size, uint32_t fmt,
                               MgcResource **r)
{
    MgcResourceDesc v = {0};
    v.header = (MgcHeader)MGC_HEADER_INIT(MgcResourceDesc);
    v.allocation_id = id;
    v.allocation_generation = 1;
    v.capacity_bytes = size;
    v.format = fmt;
    return mgc_resource_create(d, &v, r);
}
static MgcStatus make_lease(MgcResource *r, uint64_t content, uint64_t size, uint32_t access,
                            MgcLease **l)
{
    MgcLeaseDesc v = {0};
    v.header = (MgcHeader)MGC_HEADER_INIT(MgcLeaseDesc);
    v.content_id = content;
    v.byte_length = size;
    v.access = access;
    return mgc_lease_create(r, &v, l);
}
static MgcStatus start_runtime_provider(MgcRuntime **r, MgcDomain **d, MgcSession **s,
                                        uint32_t provider)
{
    MgcRuntimeDesc rd = {0};
    MgcDomainDesc dd = {0};
    MgcSessionDesc sd = {0};
    rd.header = (MgcHeader)MGC_HEADER_INIT(MgcRuntimeDesc);
    rd.provider = provider;
    rd.memory_budget_bytes = 1024 * 1024;
    dd.header = (MgcHeader)MGC_HEADER_INIT(MgcDomainDesc);
    sd.header = (MgcHeader)MGC_HEADER_INIT(MgcSessionDesc);
    sd.max_in_flight = 4;
    MgcStatus x = mgc_runtime_create(&rd, r);
    if (x)
        return x;
    x = mgc_domain_acquire(*r, &dd, d);
    if (x)
        return x;
    return mgc_session_create(*d, &sd, s);
}
static MgcStatus start_runtime(MgcRuntime **r, MgcDomain **d, MgcSession **s)
{
    return start_runtime_provider(r, d, s, MGC_PROVIDER_CPU_REFERENCE);
}
#endif
