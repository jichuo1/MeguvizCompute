/* Public-only conformance entry: link against real MeguvizCompute::core.
 * No G1 fake symbols, no private-provider state or fabricated GPU results. */
#include <meguviz/compute.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do{if(!(x)){fprintf(stderr,"conformance FAIL line %d: %s\n",__LINE__,#x);exit(1);}}while(0)
#define INIT(v,t) do{memset(&(v),0,sizeof(v));(v).header=(MgcHeader)MGC_HEADER_INIT(t);}while(0)
typedef struct Context {int refs,calls;uint64_t id;MgcSession *session;} Context;
static void retain(void *p){++((Context*)p)->refs;}
static void release(void *p){--((Context*)p)->refs;}
static void completed(void *p,const MgcCompletion *c){Context *x=(Context*)p;CHECK(c->execution_retired&&c->host_finalized&&c->terminal==MGC_TERMINAL_SUCCESS);CHECK(x->refs==1);CHECK(mgc_session_drain(x->session,0)==MGC_BUSY);++x->calls;x->id=c->request_id;}
int main(void){
    struct {MgcHeader header;} old={{sizeof(MgcHeader),MGC_ABI_MAJOR,0}};
    MgcRuntimeDesc rd;MgcRuntime *runtime=0;MgcDomainDesc dd;MgcDomain *domain=0,*other=0;MgcResourceDesc bd;MgcResource *src=0,*dst=0;MgcLeaseDesc ld;MgcLease *input=0,*output=0,*write=0;MgcSessionDesc sd;MgcSession *session=0;MgcSubmitDesc submit;MgcTicket *ticket=0;Context context={0};void *p=0;uint64_t n=0;
    CHECK(mgc_query_build_info((MgcBuildInfo*)&old)==MGC_SUCCESS);INIT(rd,MgcRuntimeDesc);rd.provider=MGC_PROVIDER_CPU_REFERENCE;rd.memory_budget_bytes=4096;rd.required_flags=1;CHECK(mgc_runtime_create(&rd,&runtime)==MGC_UNSUPPORTED_ABI&&runtime==0);rd.required_flags=0;CHECK(mgc_runtime_create(&rd,&runtime)==MGC_SUCCESS);INIT(dd,MgcDomainDesc);CHECK(mgc_domain_acquire(runtime,&dd,&domain)==MGC_SUCCESS);CHECK(mgc_domain_acquire(runtime,&dd,&other)==MGC_SUCCESS);
    INIT(bd,MgcResourceDesc);bd.capacity_bytes=32;bd.format=MGC_FORMAT_BYTES;bd.allocation_id=1;bd.allocation_generation=1;CHECK(mgc_resource_create(domain,&bd,&src)==MGC_SUCCESS);bd.allocation_id=2;CHECK(mgc_resource_create(domain,&bd,&dst)==MGC_SUCCESS);
    INIT(ld,MgcLeaseDesc);ld.content_id=3;ld.access=MGC_ACCESS_WRITE;ld.byte_length=32;CHECK(mgc_lease_create(src,&ld,&write)==MGC_SUCCESS);CHECK(mgc_lease_map(write,&p,&n)==MGC_SUCCESS&&n==32);memset(p,0x6d,32);CHECK(mgc_lease_unmap(write)==MGC_SUCCESS);mgc_lease_release(write);ld.access=MGC_ACCESS_READ;CHECK(mgc_lease_create(src,&ld,&input)==MGC_SUCCESS);ld.access=MGC_ACCESS_WRITE;CHECK(mgc_lease_create(dst,&ld,&output)==MGC_SUCCESS);
    INIT(sd,MgcSessionDesc);sd.max_in_flight=1;CHECK(mgc_session_create(other,&sd,&session)==MGC_SUCCESS);INIT(submit,MgcSubmitDesc);submit.request_id=91;submit.operation=MGC_OPERATION_COPY;submit.input=input;submit.output=output;submit.byte_count=32;submit.completion=completed;submit.context=&context;submit.retain_context=retain;submit.release_context=release;
    CHECK(mgc_submit(session,&submit,&ticket)==MGC_INCOMPATIBLE_DOMAIN&&!ticket);CHECK(mgc_lease_map(input,&p,&n)==MGC_SUCCESS&&((unsigned char*)p)[0]==0x6d);CHECK(mgc_lease_unmap(input)==MGC_SUCCESS);CHECK(mgc_session_close(session)==MGC_SUCCESS);CHECK(mgc_session_create(domain,&sd,&session)==MGC_SUCCESS);context.session=session;
    mgc_lease_retain(input);mgc_lease_retain(output);CHECK(mgc_submit(session,&submit,&ticket)==MGC_ACCEPTED);submit.request_id=999;CHECK(mgc_session_drain(session,5000000000)==MGC_SUCCESS);CHECK(context.calls==1&&context.refs==0&&context.id==91);CHECK(mgc_lease_map(output,&p,&n)==MGC_SUCCESS&&((unsigned char*)p)[0]==0x6d&&((unsigned char*)p)[31]==0x6d);CHECK(mgc_lease_unmap(output)==MGC_SUCCESS);mgc_ticket_release(ticket);CHECK(mgc_session_close(session)==MGC_SUCCESS);mgc_lease_release(input);mgc_resource_release(src);mgc_resource_release(dst);mgc_domain_release(domain);mgc_domain_release(other);CHECK(mgc_runtime_close(runtime)==MGC_BUSY);CHECK(mgc_lease_map(output,&p,&n)==MGC_SUCCESS&&((unsigned char*)p)[31]==0x6d);CHECK(mgc_lease_unmap(output)==MGC_SUCCESS);mgc_lease_release(output);CHECK(mgc_runtime_close(runtime)==MGC_SUCCESS);mgc_runtime_release(runtime);puts("{\"scope\":\"production_cpu_public_abi\",\"result\":\"PASS\",\"old_size\":true,\"reject_preserves_input\":true,\"copy\":true,\"callback_once\":true,\"owned_output_survives\":true}");return 0;
}
