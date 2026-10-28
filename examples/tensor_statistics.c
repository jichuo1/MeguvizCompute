#include "consumer_common.h"
#include <math.h>
#include <meguviz/kernels.h>
int main(int argc, char **argv)
{
    MgcRuntime *r;
    MgcDomain *d;
    MgcSession *s;
    MgcResource *a, *b, *c;
    MgcLease *w, *in, *stats, *norm;
    MgcTicket *t;
    void *p;
    uint64_t bytes;
    MgcSubmitDesc job = {0};
    float fixture[] = {-3, 1, 5, 9};
    CHECK(start_runtime(&r, &d, &s) == 0);
    (void)argc;
    (void)argv;
    CHECK(make_resource(d, 11, sizeof(fixture), MGC_FORMAT_FLOAT32, &a) == 0);
    CHECK(make_resource(d, 12, sizeof(MgcStatisticsF32), MGC_FORMAT_BYTES, &b) == 0);
    CHECK(make_resource(d, 13, sizeof(fixture), MGC_FORMAT_FLOAT32, &c) == 0);
    CHECK(make_lease(a, 1, sizeof(fixture), MGC_ACCESS_WRITE, &w) == 0);
    CHECK(mgc_lease_map(w, &p, &bytes) == 0);
    memcpy(p, fixture, sizeof(fixture));
    CHECK(mgc_lease_unmap(w) == 0);
    mgc_lease_release(w);
    CHECK(make_lease(a, 1, sizeof(fixture), MGC_ACCESS_READ, &in) == 0);
    CHECK(make_lease(b, 1, sizeof(MgcStatisticsF32), MGC_ACCESS_WRITE, &stats) == 0);
    CHECK(make_lease(c, 1, sizeof(fixture), MGC_ACCESS_WRITE, &norm) == 0);
    job.header = (MgcHeader)MGC_HEADER_INIT(MgcSubmitDesc);
    job.input = in;
    job.output = stats;
    job.operation = MGC_OPERATION_STATISTICS_F32;
    job.byte_count = sizeof(fixture);
    mgc_lease_retain(in);
    mgc_lease_retain(stats);
    CHECK(mgc_submit(s, &job, &t) == MGC_ACCEPTED);
    CHECK(mgc_session_drain(s, 1000000000) == 0);
    CHECK(mgc_lease_map(stats, &p, &bytes) == 0);
    MgcStatisticsF32 result;
    memcpy(&result, p, sizeof(result));
    CHECK(result.count == 4 && result.finite_count == 4 && result.minimum == -3 &&
          result.maximum == 9 && result.sum == 12);
    CHECK(mgc_lease_unmap(stats) == 0);
    mgc_ticket_release(t);
    job.output = norm;
    job.operation = MGC_OPERATION_NORMALIZE_F32;
    mgc_lease_retain(in);
    mgc_lease_retain(norm);
    CHECK(mgc_submit(s, &job, &t) == MGC_ACCEPTED);
    CHECK(mgc_session_drain(s, 1000000000) == 0);
    CHECK(mgc_lease_map(norm, &p, &bytes) == 0);
    float v[4];
    memcpy(v, p, sizeof(v));
    CHECK(v[0] == 0 && fabs(v[1] - 1.f / 3) < 1e-6 && fabs(v[2] - 2.f / 3) < 1e-6 && v[3] == 1);
    CHECK(mgc_lease_unmap(norm) == 0);
    mgc_ticket_release(t);
    mgc_lease_release(in);
    mgc_lease_release(stats);
    mgc_lease_release(norm);
    mgc_resource_release(a);
    mgc_resource_release(b);
    mgc_resource_release(c);
    CHECK(mgc_session_close(s) == 0);
    mgc_domain_release(d);
    CHECK(mgc_runtime_close(r) == 0);
    mgc_runtime_release(r);
    puts("PASS independent tensor statistics and normalization");
    return 0;
}
