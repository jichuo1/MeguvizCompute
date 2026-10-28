#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <limits>
#include <meguviz/compute.h>
#include <meguviz/compute_job.h>
#include <mutex>
#include <thread>
#define REQUIRE(x)                                                                                 \
    do                                                                                             \
    {                                                                                              \
        if (!(x))                                                                                  \
        {                                                                                          \
            std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x);                              \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)
struct Gate
{
    std::mutex mutex;
    std::condition_variable cv;
    bool started = false, finish = false;
    std::atomic<int> refs{0}, callbacks{0};
    MgcCompletion completion{};
    MgcSession *session = nullptr;
};
static MgcStatus block(void *p, MgcTicket *t)
{
    auto &g = *static_cast<Gate *>(p);
    std::unique_lock<std::mutex> lock(g.mutex);
    g.started = true;
    g.cv.notify_all();
    g.cv.wait(lock, [&] { return g.finish; });
    return mgc_private_ticket_cancelled(t) ? MGC_CANCELLED : MGC_SUCCESS;
}
static void retain(void *p)
{
    ++static_cast<Gate *>(p)->refs;
}
static void release(void *p)
{
    --static_cast<Gate *>(p)->refs;
}
static void complete(void *p, const MgcCompletion *v)
{
    auto &g = *static_cast<Gate *>(p);
    g.completion = *v;
    if (mgc_session_drain(g.session, 0) != MGC_BUSY)
        g.callbacks.store(-100);
    else
        ++g.callbacks;
}
int main()
{
    MgcRuntimeDesc rd{};
    rd.header = MGC_HEADER_INIT(MgcRuntimeDesc);
    rd.provider = MGC_PROVIDER_CPU_REFERENCE;
    rd.memory_budget_bytes = 256;
    MgcRuntime *r = nullptr;
    REQUIRE(mgc_runtime_create(&rd, &r) == 0);
    MgcDomainDesc dd{};
    dd.header = MGC_HEADER_INIT(MgcDomainDesc);
    MgcDomain *d = nullptr, *other = nullptr;
    REQUIRE(mgc_domain_acquire(r, &dd, &d) == 0);
    REQUIRE(mgc_domain_acquire(r, &dd, &other) == 0);
    dd.required_flags = 1;
    MgcDomain *bad = reinterpret_cast<MgcDomain *>(1);
    REQUIRE(mgc_domain_acquire(r, &dd, &bad) == MGC_UNSUPPORTED_ABI && bad == nullptr);
    dd.required_flags = 0;
    dd.private_sdk_fingerprint.bytes[0] = 99;
    REQUIRE(mgc_domain_acquire(r, &dd, &bad) == MGC_ARTIFACT_MISMATCH);
    MgcSessionDesc sd{};
    sd.header = MGC_HEADER_INIT(MgcSessionDesc);
    sd.max_in_flight = 1;
    MgcSession *s = nullptr, *second = nullptr;
    REQUIRE(mgc_session_create(d, &sd, &s) == 0);
    REQUIRE(mgc_session_create(d, &sd, &second) == 0);
    Gate gate;
    gate.session = s;
    MgcHostJobDesc job{};
    job.header = MGC_HEADER_INIT(MgcHostJobDesc);
    job.request_id = 91;
    job.execute = block;
    job.context = &gate;
    job.retain_context = retain;
    job.release_context = release;
    job.completion = complete;
    MgcTicket *t = nullptr;
    REQUIRE(mgc_private_submit_host_job(s, &job, &t) == MGC_ACCEPTED);
    {
        std::unique_lock<std::mutex> lock(gate.mutex);
        REQUIRE(gate.cv.wait_for(lock, std::chrono::seconds(2), [&] { return gate.started; }));
    }
    REQUIRE(gate.refs == 1 && gate.callbacks == 0);
    MgcTicket *no = nullptr;
    REQUIRE(mgc_private_submit_host_job(s, &job, &no) == MGC_BACKPRESSURE && no == nullptr);
    REQUIRE(gate.refs == 1);
    REQUIRE(mgc_session_drain(s, 1000) == MGC_TIMEOUT);
    REQUIRE(mgc_ticket_cancel(t) == 0);
    MgcCompletion current{};
    current.header = MGC_HEADER_INIT(MgcCompletion);
    REQUIRE(mgc_ticket_query(t, &current) == MGC_BUSY && current.result_discarded);
    REQUIRE(gate.callbacks == 0 && gate.refs == 1);
    REQUIRE(mgc_session_close(s) == MGC_BUSY);
    {
        std::lock_guard<std::mutex> lock(gate.mutex);
        gate.finish = true;
    }
    gate.cv.notify_all();
    REQUIRE(mgc_session_drain(s, 2000000000) == 0);
    REQUIRE(gate.callbacks == 1 && gate.refs == 0 && gate.completion.execution_retired &&
            gate.completion.host_finalized && gate.completion.terminal == MGC_TERMINAL_CANCELLED);
    REQUIRE(mgc_ticket_query(t, &current) == 0);
    mgc_ticket_release(t);
    REQUIRE(mgc_session_close(s) == 0);
    MgcResourceDesc res{};
    res.header = MGC_HEADER_INIT(MgcResourceDesc);
    res.capacity_bytes = 257;
    res.format = MGC_FORMAT_BYTES;
    MgcResource *a = nullptr;
    REQUIRE(mgc_resource_create(d, &res, &a) == MGC_OUT_OF_MEMORY && a == nullptr);
    res.capacity_bytes = 16;
    MgcResource *explicit_one = nullptr, *explicit_two = nullptr;
    res.allocation_id = 1;
    res.allocation_generation = 1;
    REQUIRE(mgc_resource_create(d, &res, &explicit_one) == 0);
    res.allocation_id = 2;
    REQUIRE(mgc_resource_create(d, &res, &explicit_two) == 0);
    res.allocation_id = 0;
    REQUIRE(mgc_resource_create(d, &res, &a) == 0);
    MgcLeaseDesc ld{};
    ld.header = MGC_HEADER_INIT(MgcLeaseDesc);
    ld.content_id = 1;
    ld.byte_length = 16;
    ld.access = MGC_ACCESS_WRITE;
    MgcLease *write = nullptr;
    REQUIRE(mgc_lease_create(a, &ld, &write) == 0);
    ld.access = MGC_ACCESS_READ;
    MgcLease *read = nullptr;
    REQUIRE(mgc_lease_create(a, &ld, &read) == MGC_BUSY);
    void *data;
    uint64_t bytes;
    REQUIRE(mgc_lease_map(write, &data, &bytes) == 0);
    mgc_lease_release(write);
    REQUIRE(mgc_lease_unmap(write) == 0);
    REQUIRE(mgc_lease_create(a, &ld, &read) == 0);
    MgcLeaseInfo info{};
    info.header = MGC_HEADER_INIT(MgcLeaseInfo);
    REQUIRE(mgc_lease_info(read, &info) == 0 && info.byte_length == 16);
    REQUIRE(info.allocation_id != 1 && info.allocation_id != 2);
    mgc_resource_release(explicit_one);
    mgc_resource_release(explicit_two);
    ld.byte_offset = std::numeric_limits<uint64_t>::max();
    MgcLease *invalid = nullptr;
    REQUIRE(mgc_lease_create(a, &ld, &invalid) == MGC_INVALID_ARGUMENT);
    MgcLease *refs[] = {read};
    job.leases = refs;
    job.lease_count = 1;
    job.execute = [](void *, MgcTicket *) -> MgcStatus { return MGC_SUCCESS; };
    job.completion = nullptr;
    job.retain_context = job.release_context = nullptr;
    job.context = nullptr;
    MgcSession *wrong = nullptr;
    REQUIRE(mgc_session_create(other, &sd, &wrong) == 0);
    REQUIRE(mgc_private_submit_host_job(wrong, &job, &t) == MGC_INCOMPATIBLE_DOMAIN);
    REQUIRE(mgc_lease_info(read, &info) == 0);
    REQUIRE(mgc_session_close(wrong) == 0);
    mgc_lease_retain(read);
    REQUIRE(mgc_private_submit_host_job(second, &job, &t) == MGC_ACCEPTED);
    REQUIRE(mgc_session_drain(second, 1000000000) == 0);
    mgc_ticket_release(t);
    REQUIRE(mgc_runtime_close(r) == MGC_BUSY);
    mgc_lease_release(read);
    mgc_resource_release(a);
    REQUIRE(mgc_session_close(second) == 0);
    mgc_domain_release(other);
    mgc_domain_release(d);
    REQUIRE(mgc_runtime_close(r) == 0);
    mgc_runtime_release(r);
    puts("PASS bounded queue, cancel, late callback, continued drain, retained "
         "map, domain mismatch, budget, second session");
    return 0;
}
