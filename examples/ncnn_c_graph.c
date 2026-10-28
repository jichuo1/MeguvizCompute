#include "consumer_common.h"
int main(void)
{
    MgcRuntime *r;
    MgcDomain *d;
    MgcSession *s;
    MgcResource *a, *b;
    MgcLease *w, *in, *out;
    void *p;
    uint64_t n;
    MgcTicket *t;
    const float values[] = {-7, -.5f, 0, 1, 2, 18};
    const float expected[] = {0, 0, 0, 1, 2, 18};
    CHECK(start_runtime_provider(&r, &d, &s, MGC_PROVIDER_NCNN_CPU) == 0);
    CHECK(make_resource(d, 101, sizeof(values), MGC_FORMAT_FLOAT32, &a) == 0);
    CHECK(make_resource(d, 102, sizeof(values), MGC_FORMAT_FLOAT32, &b) == 0);
    CHECK(make_lease(a, 1, sizeof(values), MGC_ACCESS_WRITE, &w) == 0);
    CHECK(mgc_lease_map(w, &p, &n) == 0);
    memcpy(p, values, sizeof(values));
    CHECK(mgc_lease_unmap(w) == 0);
    mgc_lease_release(w);
    CHECK(make_lease(a, 1, sizeof(values), MGC_ACCESS_READ, &in) == 0);
    CHECK(make_lease(b, 1, sizeof(values), MGC_ACCESS_WRITE, &out) == 0);
    MgcSubmitDesc job = {0};
    job.header = (MgcHeader)MGC_HEADER_INIT(MgcSubmitDesc);
    job.operation = MGC_OPERATION_RELU_F32;
    job.byte_count = sizeof(values);
    job.input = in;
    job.output = out;
    mgc_lease_retain(in);
    mgc_lease_retain(out);
    CHECK(mgc_submit(s, &job, &t) == MGC_ACCEPTED);
    CHECK(mgc_session_drain(s, 2000000000) == 0);
    MgcCompletion result = {0};
    result.header = (MgcHeader)MGC_HEADER_INIT(MgcCompletion);
    MgcStatus queried = mgc_ticket_query(t, &result);
    fprintf(stderr, "ncnn graph completion query=%d status=%d terminal=%u\n", queried,
            result.status, result.terminal);
    CHECK(queried == 0 && result.status == 0 && result.terminal == MGC_TERMINAL_SUCCESS);
    CHECK(mgc_lease_map(out, &p, &n) == 0);
    CHECK(!memcmp(p, expected, sizeof(expected)));
    CHECK(mgc_lease_unmap(out) == 0);
    mgc_ticket_release(t);
    mgc_lease_release(in);
    mgc_lease_release(out);
    mgc_resource_release(a);
    mgc_resource_release(b);
    CHECK(mgc_session_close(s) == 0);
    mgc_domain_release(d);
    CHECK(mgc_runtime_close(r) == 0);
    mgc_runtime_release(r);
    puts("PASS pure C API resource/ticket real ncnn Input/ReLU graph vs scalar "
         "oracle");
    return 0;
}
