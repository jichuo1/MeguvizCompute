#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <limits>
#include <meguviz/compute.h>
#include <meguviz/compute_android.h>
#include <meguviz/compute_job.h>
#include <meguviz/kernels.h>
#include <memory>
#include <mutex>
#include <new>
#include <set>
#include <thread>
#include <vector>
#if MGC_HAVE_NCNN
#include <datareader.h>
#include <meguviz/backend_ncnn_sdk.hpp>
#endif
#ifndef MGC_BUILD_FP
#define MGC_BUILD_FP ""
#endif
#ifndef MGC_PRIVATE_FP
#define MGC_PRIVATE_FP ""
#endif
namespace
{
std::atomic<uint64_t> next_domain{1};
std::atomic<uint64_t> next_allocation{1};
std::recursive_mutex execution_slot;
#if MGC_HAVE_NCNN && NCNN_VULKAN
std::mutex instance_mutex;
uint32_t instance_users = 0;
#endif
bool header_ok(const MgcHeader *h, size_t minimum)
{
    return h && h->abi_major == MGC_ABI_MAJOR && h->struct_size >= minimum;
}
template <class T> MgcStatus read_desc(const T *src, T &dst, size_t minimum)
{
    if (!src)
        return MGC_INVALID_ARGUMENT;
    if (!header_ok(&src->header, minimum))
        return MGC_UNSUPPORTED_ABI;
    std::memset(&dst, 0, sizeof(dst));
    std::memcpy(&dst, src, std::min<size_t>(src->header.struct_size, sizeof(dst)));
    return MGC_SUCCESS;
}
bool zeros(const void *ptr, size_t bytes)
{
    const auto *p = static_cast<const unsigned char *>(ptr);
    for (size_t i = 0; i < bytes; ++i)
        if (p[i])
            return false;
    return true;
}
void hash_from_hex(const char *s, MgcHash &h)
{
    std::memset(&h, 0, sizeof(h));
    if (std::strlen(s) != 64)
        return;
    for (int i = 0; i < 32; ++i)
    {
        auto n = [](char c) { return c >= '0' && c <= '9' ? c - '0' : c - 'a' + 10; };
        h.bytes[i] = static_cast<uint8_t>((n(s[i * 2]) << 4) | n(s[i * 2 + 1]));
    }
}
MgcStatus flags(uint32_t required, const void *reserved, size_t bytes)
{
    return required                 ? MGC_UNSUPPORTED_ABI
           : zeros(reserved, bytes) ? MGC_SUCCESS
                                    : MGC_INVALID_ARGUMENT;
}
void invoke_context(MgcContextFn fn, void *ctx) noexcept
{
    if (fn)
    {
        try
        {
            fn(ctx);
        }
        catch (...)
        {
        }
    }
}
} // namespace
struct MgcRuntime
{
    std::atomic<uint32_t> refs{1};
    std::mutex mutex;
    bool closed = false;
    uint32_t provider = 1;
    uint64_t budget = 0, live_bytes = 0;
    uint32_t domains = 0;
};
struct MgcDomain
{
    std::atomic<uint32_t> refs{1};
    MgcRuntime *runtime = nullptr;
    uint64_t id = 0, generation = 1;
    uint32_t device_index = 0;
    std::mutex allocations_mutex;
    std::set<std::pair<uint64_t, uint64_t>> allocations;
#if MGC_HAVE_NCNN && NCNN_VULKAN
    const ncnn::VulkanDevice *device = nullptr;
    bool instance_owned = false;
#endif
};
struct MgcResource
{
    std::atomic<uint32_t> refs{1};
    MgcDomain *domain = nullptr;
    MgcResourceDesc desc{};
    std::vector<unsigned char> data;
    std::mutex mutex;
    uint32_t readers = 0, writers = 0, maps = 0, in_flight = 0;
    uint64_t current_content = 0;
};
struct MgcLease
{
    std::atomic<uint32_t> refs{1};
    MgcResource *resource = nullptr;
    MgcLeaseDesc desc{};
    uint32_t maps = 0;
};
struct MgcTicket
{
    std::atomic<uint32_t> refs{1};
    std::atomic<bool> cancelled{false};
    std::mutex mutex;
    MgcCompletion result{};
    bool done = false;
    MgcSubmitDesc desc{};
    MgcHostJobFn execute = nullptr;
    std::vector<MgcLease *> leases;
};
struct MgcSession
{
    MgcDomain *domain = nullptr;
    std::mutex mutex;
    std::condition_variable work, retired;
    std::deque<MgcTicket *> queue;
    uint32_t max_in_flight = 1, in_flight = 0;
    bool closing = false;
    std::thread worker;
};
namespace
{
thread_local MgcSession *worker_session = nullptr;
thread_local MgcDomain *active_domain = nullptr;
thread_local MgcTicket *active_ticket = nullptr;
MgcStatus output_struct(void *out, const void *src, size_t size, size_t minimum)
{
    if (!out)
        return MGC_INVALID_ARGUMENT;
    auto *h = static_cast<MgcHeader *>(out);
    if (!header_ok(h, minimum))
        return MGC_UNSUPPORTED_ABI;
    size_t n = std::min<size_t>(h->struct_size, size);
    std::memcpy(out, src, n);
    return MGC_SUCCESS;
}
MgcStatus validate_lease(MgcSession *s, MgcLease *l)
{
    if (!l)
        return MGC_INVALID_ARGUMENT;
    if (l->resource->domain != s->domain)
        return MGC_INCOMPATIBLE_DOMAIN;
    if (l->desc.dependency_kind != MGC_DEPENDENCY_READY)
        return MGC_UNSUPPORTED_CAPABILITY;
    return MGC_SUCCESS;
}
MgcStatus operation_valid(const MgcSubmitDesc &d)
{
    if (!d.input || !d.output)
        return MGC_INVALID_ARGUMENT;
    auto *a = d.input;
    auto *b = d.output;
    if (a->desc.access != MGC_ACCESS_READ || b->desc.access != MGC_ACCESS_WRITE)
        return MGC_INVALID_ARGUMENT;
    if (a->resource == b->resource)
        return MGC_INVALID_ARGUMENT;
    if (d.operation == MGC_OPERATION_COPY)
        return d.byte_count && d.byte_count <= a->desc.byte_length &&
                       d.byte_count <= b->desc.byte_length
                   ? MGC_SUCCESS
                   : MGC_INVALID_ARGUMENT;
    if (d.operation == MGC_OPERATION_NORMALIZE_F32 || d.operation == MGC_OPERATION_STATISTICS_F32 ||
        d.operation == MGC_OPERATION_RELU_F32)
    {
        if (!d.byte_count || d.byte_count % 4 || a->resource->desc.format != MGC_FORMAT_FLOAT32 ||
            d.byte_count > a->desc.byte_length)
            return MGC_INVALID_ARGUMENT;
        uint64_t needed =
            d.operation == MGC_OPERATION_STATISTICS_F32 ? sizeof(MgcStatisticsF32) : d.byte_count;
        if (needed > b->desc.byte_length)
            return MGC_INVALID_ARGUMENT;
        return MGC_SUCCESS;
    }
    if (d.operation == MGC_OPERATION_RESIZE_RGB8)
    {
        const auto &x = a->resource->desc;
        const auto &y = b->resource->desc;
        if (d.byte_count || x.format != MGC_FORMAT_RGB8 || y.format != MGC_FORMAT_RGB8 ||
            !x.width || !x.height || !y.width || !y.height)
            return MGC_INVALID_ARGUMENT;
        auto valid = [](const MgcResourceDesc &v, uint64_t n)
        {
            return v.row_stride_bytes >= uint64_t(v.width) * 3 &&
                   v.height <= UINT64_MAX / v.row_stride_bytes &&
                   uint64_t(v.height) * v.row_stride_bytes <= n;
        };
        return valid(x, a->desc.byte_length) && valid(y, b->desc.byte_length)
                   ? MGC_SUCCESS
                   : MGC_INVALID_ARGUMENT;
    }
    return MGC_UNSUPPORTED_CAPABILITY;
}
MgcStatus run_operation(MgcTicket *t)
{
    const auto &d = t->desc;
    const unsigned char *src = d.input->resource->data.data() + d.input->desc.byte_offset;
    unsigned char *dst = d.output->resource->data.data() + d.output->desc.byte_offset;
    if (d.operation == MGC_OPERATION_COPY)
    {
        std::memcpy(dst, src, static_cast<size_t>(d.byte_count));
        return MGC_SUCCESS;
    }
    if (d.operation == MGC_OPERATION_NORMALIZE_F32 || d.operation == MGC_OPERATION_STATISTICS_F32 ||
        d.operation == MGC_OPERATION_RELU_F32)
    {
        MgcStatisticsF32 st{};
        st.header = MGC_HEADER_INIT(MgcStatisticsF32);
        st.count = d.byte_count / 4;
        for (uint64_t i = 0; i < st.count; ++i)
        {
            float x;
            std::memcpy(&x, src + i * 4, 4);
            if (std::isfinite(x))
            {
                if (!st.finite_count)
                    st.minimum = st.maximum = x;
                else
                {
                    st.minimum = std::min(st.minimum, x);
                    st.maximum = std::max(st.maximum, x);
                }
                st.sum += x;
                ++st.finite_count;
            }
        }
        if (d.operation == MGC_OPERATION_STATISTICS_F32)
        {
            std::memcpy(dst, &st, sizeof(st));
            return MGC_SUCCESS;
        }
        if (d.operation == MGC_OPERATION_RELU_F32)
        {
            if (st.finite_count != st.count || st.count > INT32_MAX)
                return MGC_INVALID_ARGUMENT;
            auto provider = d.input->resource->domain->runtime->provider;
            if (provider == MGC_PROVIDER_NCNN_VULKAN)
                return MGC_UNSUPPORTED_CAPABILITY;
#if MGC_HAVE_NCNN
            if (provider == MGC_PROVIDER_NCNN_CPU)
            {
                ncnn::Net net;
                net.opt.use_vulkan_compute = false;
                net.opt.num_threads = 1;
                net.opt.use_packing_layout = false;
                net.opt.use_fp16_storage = false;
                net.opt.use_fp16_arithmetic = false;
                if (net.load_param_mem(
                        "7767517\n2 2\nInput input 0 1 in0\nReLU relu 1 1 in0 out0\n"))
                    return MGC_INTERNAL_ERROR;
                alignas(4) unsigned char empty_model[4] = {};
                const unsigned char *model_cursor = empty_model;
                ncnn::DataReaderFromMemory reader(model_cursor, sizeof(empty_model));
                if (net.load_model(reader))
                    return MGC_INTERNAL_ERROR;
                ncnn::Mat input(static_cast<int>(st.count));
                if (input.empty())
                    return MGC_OUT_OF_MEMORY;
                input.fill(0.f);
                std::memcpy(input.data, src, static_cast<size_t>(d.byte_count));
                auto ex = net.create_extractor();
                if (ex.input("in0", input))
                    return MGC_INTERNAL_ERROR;
                ncnn::Mat output;
                if (ex.extract("out0", output))
                    return MGC_INTERNAL_ERROR;
                if (output.dims != 1 || output.w != static_cast<int>(st.count) ||
                    output.elempack != 1 || output.total() < st.count || output.elemsize != 4)
                    return MGC_INTERNAL_ERROR;
                std::memcpy(dst, output.data, static_cast<size_t>(d.byte_count));
                return MGC_SUCCESS;
            }
#endif
            for (uint64_t i = 0; i < st.count; ++i)
            {
                float x;
                std::memcpy(&x, src + i * 4, 4);
                x = std::max(0.f, x);
                std::memcpy(dst + i * 4, &x, 4);
            }
            return MGC_SUCCESS;
        }
        if (st.finite_count != st.count)
            return MGC_INVALID_ARGUMENT;
        double range = double(st.maximum) - st.minimum;
        for (uint64_t i = 0; i < st.count; ++i)
        {
            float x;
            std::memcpy(&x, src + i * 4, 4);
            float y = range ? static_cast<float>((double(x) - st.minimum) / range) : 0.f;
            std::memcpy(dst + i * 4, &y, 4);
        }
        return MGC_SUCCESS;
    }
    const auto &x = d.input->resource->desc;
    const auto &y = d.output->resource->desc;
    for (uint32_t j = 0; j < y.height; ++j)
        for (uint32_t i = 0; i < y.width; ++i)
        {
            double fx = (double(i) + .5) * x.width / y.width - .5,
                   fy = (double(j) + .5) * x.height / y.height - .5;
            int64_t ix = static_cast<int64_t>(std::floor(fx)),
                    iy = static_cast<int64_t>(std::floor(fy));
            double ax = fx - ix, ay = fy - iy;
            auto clamp = [](int64_t v, uint32_t n)
            { return static_cast<uint32_t>(std::max<int64_t>(0, std::min<int64_t>(v, n - 1))); };
            for (uint32_t c = 0; c < 3; ++c)
            {
                auto at = [&](int64_t a, int64_t b)
                {
                    return double(src[uint64_t(clamp(b, x.height)) * x.row_stride_bytes +
                                      uint64_t(clamp(a, x.width)) * 3 + c]);
                };
                double z = (at(ix, iy) * (1 - ax) + at(ix + 1, iy) * ax) * (1 - ay) +
                           (at(ix, iy + 1) * (1 - ax) + at(ix + 1, iy + 1) * ax) * ay;
                dst[uint64_t(j) * y.row_stride_bytes + uint64_t(i) * 3 + c] =
                    static_cast<unsigned char>(std::floor(z + .5));
            }
        }
    return MGC_SUCCESS;
}
void release_submission(MgcTicket *t)
{
    for (auto *l : t->leases)
    {
        {
            std::lock_guard<std::mutex> lock(l->resource->mutex);
            --l->resource->in_flight;
        }
        mgc_lease_release(l);
    }
    t->leases.clear();
}
void worker(MgcSession *s)
{
    worker_session = s;
    for (;;)
    {
        MgcTicket *t;
        {
            std::unique_lock<std::mutex> lock(s->mutex);
            s->work.wait(lock, [&] { return s->closing || !s->queue.empty(); });
            if (s->queue.empty())
            {
                if (s->closing)
                    break;
                continue;
            }
            t = s->queue.front();
            s->queue.pop_front();
        }
        MgcStatus status = MGC_CANCELLED;
        if (!t->cancelled.load())
        {
            std::lock_guard<std::recursive_mutex> slot(execution_slot);
            active_domain = s->domain;
            active_ticket = t;
            try
            {
                status = t->execute ? t->execute(t->desc.context, t) : run_operation(t);
            }
            catch (const std::bad_alloc &)
            {
                status = MGC_OUT_OF_MEMORY;
            }
            catch (...)
            {
                status = MGC_INTERNAL_ERROR;
            }
            active_ticket = nullptr;
            active_domain = nullptr;
        }
        MgcCompletion result{};
        result.header = MGC_HEADER_INIT(MgcCompletion);
        result.request_id = t->desc.request_id;
        result.execution_retired = 1;
        result.host_finalized = 1;
        {
            std::lock_guard<std::mutex> lock(t->mutex);
            if (t->cancelled.load())
            {
                result.terminal = MGC_TERMINAL_CANCELLED;
                result.status = MGC_CANCELLED;
                result.result_discarded = 1;
            }
            else
            {
                result.status = status;
                result.terminal = status == MGC_SUCCESS       ? MGC_TERMINAL_SUCCESS
                                  : status == MGC_DEVICE_LOST ? MGC_TERMINAL_DEVICE_LOST
                                                              : MGC_TERMINAL_FAILED;
            }
            t->result = result;
            t->done = true;
        }
        if (t->desc.completion)
        {
            try
            {
                t->desc.completion(t->desc.context, &result);
            }
            catch (...)
            {
            }
        }
        invoke_context(t->desc.release_context, t->desc.context);
        release_submission(t);
        {
            std::lock_guard<std::mutex> lock(s->mutex);
            --s->in_flight;
        }
        s->retired.notify_all();
        mgc_ticket_release(t);
    }
    worker_session = nullptr;
}
MgcStatus accept(MgcSession *s, std::unique_ptr<MgcTicket> &t, MgcTicket **out)
{
    for (auto *l : t->leases)
    {
        auto rc = validate_lease(s, l);
        if (rc != MGC_SUCCESS)
            return rc;
    }
    std::vector<MgcResource *> resources;
    for (auto *l : t->leases)
        resources.push_back(l->resource);
    std::sort(resources.begin(), resources.end());
    resources.erase(std::unique(resources.begin(), resources.end()), resources.end());
    std::vector<std::unique_lock<std::mutex>> locks;
    for (auto *r : resources)
        locks.emplace_back(r->mutex);
    for (auto *r : resources)
        if (r->maps || r->in_flight)
            return MGC_BUSY;
    std::unique_lock<std::mutex> lock(s->mutex);
    if (s->closing)
        return MGC_BUSY;
    if (s->in_flight >= s->max_in_flight)
        return MGC_BACKPRESSURE;
    s->queue.push_back(t.get());
    ++s->in_flight;
    for (auto *l : t->leases)
        ++l->resource->in_flight;
    t->refs.store(2);
    *out = t.release();
    lock.unlock();
    locks.clear();
    s->work.notify_one();
    return MGC_ACCEPTED;
}
} // namespace
extern "C"
{
    MgcStatus mgc_query_build_info(MgcBuildInfo *info)
    {
        MgcBuildInfo v{};
        v.header = MGC_HEADER_INIT(MgcBuildInfo);
        v.provider = MGC_HAVE_NCNN ? MGC_PROVIDER_NCNN_CPU : MGC_PROVIDER_CPU_REFERENCE;
        hash_from_hex(MGC_PRIVATE_FP, v.private_sdk_fingerprint);
        hash_from_hex(MGC_BUILD_FP, v.build_fingerprint);
        return output_struct(info, &v, sizeof(v), sizeof(MgcHeader));
    }
    MgcStatus mgc_runtime_create(const MgcRuntimeDesc *input, MgcRuntime **out)
    {
        if (!out)
            return MGC_INVALID_ARGUMENT;
        *out = nullptr;
        MgcRuntimeDesc d{};
        auto rc = read_desc(input, d, offsetof(MgcRuntimeDesc, reserved));
        if (rc)
            return rc;
        rc = flags(d.required_flags, d.reserved, sizeof(d.reserved));
        if (rc)
            return rc;
        if (d.provider != MGC_PROVIDER_CPU_REFERENCE && d.provider != MGC_PROVIDER_NCNN_CPU &&
            d.provider != MGC_PROVIDER_NCNN_VULKAN)
            return MGC_UNSUPPORTED_CAPABILITY;
#if !MGC_HAVE_NCNN
        if (d.provider != MGC_PROVIDER_CPU_REFERENCE)
            return MGC_UNSUPPORTED_CAPABILITY;
#elif !NCNN_VULKAN
        if (d.provider == MGC_PROVIDER_NCNN_VULKAN)
            return MGC_UNSUPPORTED_CAPABILITY;
#endif
        try
        {
            auto *r = new MgcRuntime;
            r->provider = d.provider;
            r->budget = d.memory_budget_bytes ? d.memory_budget_bytes : 64 * 1024 * 1024;
            *out = r;
            return MGC_SUCCESS;
        }
        catch (...)
        {
            return MGC_OUT_OF_MEMORY;
        }
    }
    void mgc_runtime_retain(MgcRuntime *r)
    {
        if (r)
            ++r->refs;
    }
    void mgc_runtime_release(MgcRuntime *r)
    {
        if (r && --r->refs == 0)
            delete r;
    }
    MgcStatus mgc_runtime_close(MgcRuntime *r)
    {
        if (!r)
            return MGC_INVALID_ARGUMENT;
        std::lock_guard<std::mutex> lock(r->mutex);
        r->closed = true;
        return r->domains ? MGC_BUSY : MGC_SUCCESS;
    }
    MgcStatus mgc_domain_acquire(MgcRuntime *r, const MgcDomainDesc *input, MgcDomain **out)
    {
        if (!r || !out)
            return MGC_INVALID_ARGUMENT;
        *out = nullptr;
        MgcDomainDesc d{};
        auto rc = read_desc(input, d, offsetof(MgcDomainDesc, reserved));
        if (rc)
            return rc;
        rc = flags(d.required_flags, d.reserved, sizeof(d.reserved));
        if (rc)
            return rc;
        MgcHash fp{};
        hash_from_hex(MGC_PRIVATE_FP, fp);
        if (!zeros(&d.private_sdk_fingerprint, sizeof(fp)) &&
            std::memcmp(&fp, &d.private_sdk_fingerprint, sizeof(fp)))
            return MGC_ARTIFACT_MISMATCH;
        try
        {
            auto dom = std::make_unique<MgcDomain>();
            std::lock_guard<std::mutex> lock(r->mutex);
            if (r->closed)
                return MGC_BUSY;
#if MGC_HAVE_NCNN && NCNN_VULKAN
            if (r->provider == MGC_PROVIDER_NCNN_VULKAN)
            {
                std::lock_guard<std::mutex> gpu(instance_mutex);
                if (!instance_users && ncnn::create_gpu_instance() != 0)
                    return MGC_UNSUPPORTED_CAPABILITY;
                if (d.device_index >= static_cast<uint32_t>(ncnn::get_gpu_count()))
                {
                    if (!instance_users)
                        ncnn::destroy_gpu_instance();
                    return MGC_UNSUPPORTED_CAPABILITY;
                }
                dom->device = ncnn::get_gpu_device(d.device_index);
                if (!dom->device)
                {
                    if (!instance_users)
                        ncnn::destroy_gpu_instance();
                    return MGC_DEVICE_LOST;
                }
                ++instance_users;
                dom->instance_owned = true;
            }
#endif
            if (r->provider != MGC_PROVIDER_NCNN_VULKAN && d.device_index)
                return MGC_INVALID_ARGUMENT;
            dom->runtime = r;
            dom->id = next_domain++;
            dom->device_index = d.device_index;
            mgc_runtime_retain(r);
            ++r->domains;
            *out = dom.release();
            return MGC_SUCCESS;
        }
        catch (...)
        {
            return MGC_OUT_OF_MEMORY;
        }
    }
    MgcStatus mgc_domain_identity(MgcDomain *d, uint64_t *id, uint64_t *gen)
    {
        if (!d || !id || !gen)
            return MGC_INVALID_ARGUMENT;
        *id = d->id;
        *gen = d->generation;
        return MGC_SUCCESS;
    }
    void mgc_domain_retain(MgcDomain *d)
    {
        if (d)
            ++d->refs;
    }
    void mgc_domain_release(MgcDomain *d)
    {
        if (!d || --d->refs)
            return;
#if MGC_HAVE_NCNN && NCNN_VULKAN
        if (d->instance_owned)
        {
            std::lock_guard<std::recursive_mutex> slot(execution_slot);
            std::lock_guard<std::mutex> gpu(instance_mutex);
            if (!--instance_users)
                ncnn::destroy_gpu_instance();
        }
#endif
        auto *r = d->runtime;
        {
            std::lock_guard<std::mutex> lock(r->mutex);
            --r->domains;
        }
        delete d;
        mgc_runtime_release(r);
    }
    MgcStatus mgc_domain_capability(MgcDomain *d, uint32_t feature, MgcCapability *out)
    {
        if (!d || feature < 1 || feature > MGC_CAP_APPROVED_SHADER_PROFILE)
            return MGC_INVALID_ARGUMENT;
        MgcCapability v{};
        v.header = MGC_HEADER_INIT(MgcCapability);
        v.feature = feature;
        v.probe_result = MGC_UNSUPPORTED_CAPABILITY;
        v.reason = MGC_UNSUPPORTED_CAPABILITY;
        if (feature == MGC_CAP_CPU_REFERENCE || feature == MGC_CAP_HOST_JOB_ASYNC)
        {
            v.advertised = v.enabled = v.workload_qualified = 1;
            v.probe_result = v.reason = MGC_SUCCESS;
        }
#if MGC_HAVE_NCNN && NCNN_VULKAN
        if (feature == MGC_CAP_VULKAN_COMPUTE && d->device)
        {
            v.advertised = v.enabled = 1;
            v.probe_result = MGC_BUSY;
            v.reason = MGC_BUSY;
        }
#endif
        return output_struct(out, &v, sizeof(v), offsetof(MgcCapability, reserved));
    }
    MgcStatus mgc_resource_create(MgcDomain *d, const MgcResourceDesc *input, MgcResource **out)
    {
        if (!d || !out)
            return MGC_INVALID_ARGUMENT;
        *out = nullptr;
        MgcResourceDesc v{};
        auto rc = read_desc(input, v, offsetof(MgcResourceDesc, reserved));
        if (rc)
            return rc;
        rc = flags(v.required_flags, v.reserved, sizeof(v.reserved));
        if (rc)
            return rc;
        if (!v.capacity_bytes || v.capacity_bytes > SIZE_MAX || v.format < 1 || v.format > 4 ||
            v.usage)
            return MGC_INVALID_ARGUMENT;
        try
        {
            auto res = std::make_unique<MgcResource>();
            if (!v.allocation_generation)
                v.allocation_generation = 1;
            std::unique_lock<std::mutex> identity(d->allocations_mutex);
            if (!v.allocation_id)
            {
                do
                {
                    v.allocation_id = next_allocation++;
                    if (!v.allocation_id)
                        return MGC_OUT_OF_MEMORY;
                } while (d->allocations.count({v.allocation_id, v.allocation_generation}));
            }
            if (d->allocations.count({v.allocation_id, v.allocation_generation}))
                return MGC_INVALID_ARGUMENT;
            auto *r = d->runtime;
            {
                std::lock_guard<std::mutex> lock(r->mutex);
                if (r->closed)
                    return MGC_BUSY;
                if (v.capacity_bytes > r->budget - r->live_bytes)
                    return MGC_OUT_OF_MEMORY;
                r->live_bytes += v.capacity_bytes;
            }
            try
            {
                res->data.resize(static_cast<size_t>(v.capacity_bytes));
            }
            catch (...)
            {
                std::lock_guard<std::mutex> lock(r->mutex);
                r->live_bytes -= v.capacity_bytes;
                throw;
            }
            try
            {
                d->allocations.insert({v.allocation_id, v.allocation_generation});
            }
            catch (...)
            {
                std::lock_guard<std::mutex> lock(r->mutex);
                r->live_bytes -= v.capacity_bytes;
                throw;
            }
            res->domain = d;
            res->desc = v;
            mgc_domain_retain(d);
            *out = res.release();
            return MGC_SUCCESS;
        }
        catch (...)
        {
            return MGC_OUT_OF_MEMORY;
        }
    }
    void mgc_resource_retain(MgcResource *r)
    {
        if (r)
            ++r->refs;
    }
    void mgc_resource_release(MgcResource *r)
    {
        if (!r || --r->refs)
            return;
        auto *d = r->domain;
        {
            std::lock_guard<std::mutex> identity(d->allocations_mutex);
            d->allocations.erase({r->desc.allocation_id, r->desc.allocation_generation});
        }
        {
            std::lock_guard<std::mutex> lock(d->runtime->mutex);
            d->runtime->live_bytes -= r->desc.capacity_bytes;
        }
        delete r;
        mgc_domain_release(d);
    }
    MgcStatus mgc_lease_create(MgcResource *r, const MgcLeaseDesc *input, MgcLease **out)
    {
        if (!r || !out)
            return MGC_INVALID_ARGUMENT;
        *out = nullptr;
        MgcLeaseDesc d{};
        auto rc = read_desc(input, d, offsetof(MgcLeaseDesc, reserved));
        if (rc)
            return rc;
        rc = flags(d.required_flags, d.reserved, sizeof(d.reserved));
        if (rc)
            return rc;
        if (!d.content_id || !d.byte_length || d.byte_offset > r->desc.capacity_bytes ||
            d.byte_length > r->desc.capacity_bytes - d.byte_offset ||
            (d.access != MGC_ACCESS_READ && d.access != MGC_ACCESS_WRITE))
            return MGC_INVALID_ARGUMENT;
        if (d.dependency_kind != MGC_DEPENDENCY_READY || d.dependency_id)
            return MGC_UNSUPPORTED_CAPABILITY;
        try
        {
            auto lease = std::make_unique<MgcLease>();
            std::lock_guard<std::mutex> lock(r->mutex);
            if (r->writers || (d.access == MGC_ACCESS_WRITE && r->readers))
                return MGC_BUSY;
            if (d.access == MGC_ACCESS_READ && r->current_content &&
                d.content_id != r->current_content)
                return MGC_ARTIFACT_MISMATCH;
            if (d.access == MGC_ACCESS_WRITE)
            {
                if (d.content_id <= r->current_content)
                    return MGC_ARTIFACT_MISMATCH;
                ++r->writers;
                r->current_content = d.content_id;
            }
            else
            {
                ++r->readers;
                if (!r->current_content)
                    r->current_content = d.content_id;
            }
            lease->resource = r;
            lease->desc = d;
            mgc_resource_retain(r);
            *out = lease.release();
            return MGC_SUCCESS;
        }
        catch (...)
        {
            return MGC_OUT_OF_MEMORY;
        }
    }
    void mgc_lease_retain(MgcLease *l)
    {
        if (l)
            ++l->refs;
    }
    void mgc_lease_release(MgcLease *l)
    {
        if (!l || --l->refs)
            return;
        auto *r = l->resource;
        {
            std::lock_guard<std::mutex> lock(r->mutex);
            if (l->desc.access == MGC_ACCESS_WRITE)
                --r->writers;
            else
                --r->readers;
        }
        delete l;
        mgc_resource_release(r);
    }
    MgcStatus mgc_lease_info(MgcLease *l, MgcLeaseInfo *out)
    {
        if (!l)
            return MGC_INVALID_ARGUMENT;
        MgcLeaseInfo v{};
        v.header = MGC_HEADER_INIT(MgcLeaseInfo);
        v.domain_id = l->resource->domain->id;
        v.domain_generation = l->resource->domain->generation;
        v.allocation_id = l->resource->desc.allocation_id;
        v.allocation_generation = l->resource->desc.allocation_generation;
        v.content_id = l->desc.content_id;
        v.capacity_bytes = l->resource->desc.capacity_bytes;
        v.access = l->desc.access;
        v.dependency_kind = l->desc.dependency_kind;
        v.format = l->resource->desc.format;
        v.width = l->resource->desc.width;
        v.height = l->resource->desc.height;
        v.row_stride_bytes = l->resource->desc.row_stride_bytes;
        v.byte_offset = l->desc.byte_offset;
        v.byte_length = l->desc.byte_length;
        return output_struct(out, &v, sizeof(v), offsetof(MgcLeaseInfo, reserved));
    }
    MgcStatus mgc_lease_map(MgcLease *l, void **data, uint64_t *bytes)
    {
        if (!l || !data || !bytes)
            return MGC_INVALID_ARGUMENT;
        *data = nullptr;
        *bytes = 0;
        std::lock_guard<std::mutex> lock(l->resource->mutex);
        bool own = active_ticket &&
                   std::find(active_ticket->leases.begin(), active_ticket->leases.end(), l) !=
                       active_ticket->leases.end();
        if ((l->resource->in_flight && !own) || l->maps)
            return MGC_BUSY;
        ++l->maps;
        ++l->resource->maps;
        mgc_lease_retain(l);
        *data = l->resource->data.data() + l->desc.byte_offset;
        *bytes = l->desc.byte_length;
        return MGC_SUCCESS;
    }
    MgcStatus mgc_lease_unmap(MgcLease *l)
    {
        if (!l)
            return MGC_INVALID_ARGUMENT;
        {
            std::lock_guard<std::mutex> lock(l->resource->mutex);
            if (!l->maps)
                return MGC_INVALID_ARGUMENT;
            --l->maps;
            --l->resource->maps;
        }
        mgc_lease_release(l);
        return MGC_SUCCESS;
    }
    MgcStatus mgc_session_create(MgcDomain *d, const MgcSessionDesc *input, MgcSession **out)
    {
        if (!d || !out)
            return MGC_INVALID_ARGUMENT;
        *out = nullptr;
        MgcSessionDesc v{};
        auto rc = read_desc(input, v, offsetof(MgcSessionDesc, reserved));
        if (rc)
            return rc;
        rc = flags(v.required_flags, v.reserved, sizeof(v.reserved));
        if (rc)
            return rc;
        if (!v.max_in_flight || v.max_in_flight > 1024)
            return MGC_INVALID_ARGUMENT;
        {
            std::lock_guard<std::mutex> lock(d->runtime->mutex);
            if (d->runtime->closed)
                return MGC_BUSY;
        }
        try
        {
            auto s = std::make_unique<MgcSession>();
            s->domain = d;
            s->max_in_flight = v.max_in_flight;
            mgc_domain_retain(d);
            try
            {
                s->worker = std::thread(worker, s.get());
            }
            catch (...)
            {
                mgc_domain_release(d);
                throw;
            }
            *out = s.release();
            return MGC_SUCCESS;
        }
        catch (...)
        {
            return MGC_OUT_OF_MEMORY;
        }
    }
    MgcStatus mgc_session_close(MgcSession *s)
    {
        if (!s)
            return MGC_INVALID_ARGUMENT;
        if (worker_session == s)
            return MGC_BUSY;
        {
            std::lock_guard<std::mutex> lock(s->mutex);
            s->closing = true;
            s->work.notify_all();
            if (s->in_flight)
                return MGC_BUSY;
        }
        if (s->worker.joinable())
            s->worker.join();
        auto *d = s->domain;
        delete s;
        mgc_domain_release(d);
        return MGC_SUCCESS;
    }
    MgcStatus mgc_submit(MgcSession *s, const MgcSubmitDesc *input, MgcTicket **out)
    {
        if (!s || !out)
            return MGC_INVALID_ARGUMENT;
        *out = nullptr;
        MgcSubmitDesc d{};
        auto rc = read_desc(input, d, offsetof(MgcSubmitDesc, reserved));
        if (rc)
            return rc;
        rc = flags(d.required_flags, d.reserved, sizeof(d.reserved));
        if (rc)
            return rc;
        if (bool(d.retain_context) != bool(d.release_context))
            return MGC_INVALID_ARGUMENT;
        rc = operation_valid(d);
        if (rc)
            return rc;
        bool retained = false;
        try
        {
            auto t = std::make_unique<MgcTicket>();
            t->desc = d;
            t->result.header = MGC_HEADER_INIT(MgcCompletion);
            t->result.request_id = d.request_id;
            t->leases = {d.input, d.output};
            if (d.retain_context)
            {
                d.retain_context(d.context);
                retained = true;
            }
            rc = accept(s, t, out);
            if (rc != MGC_ACCEPTED && retained)
                invoke_context(d.release_context, d.context);
            return rc;
        }
        catch (...)
        {
            if (retained)
                invoke_context(d.release_context, d.context);
            return MGC_OUT_OF_MEMORY;
        }
    }
    void mgc_ticket_retain(MgcTicket *t)
    {
        if (t)
            ++t->refs;
    }
    void mgc_ticket_release(MgcTicket *t)
    {
        if (t && --t->refs == 0)
            delete t;
    }
    MgcStatus mgc_ticket_cancel(MgcTicket *t)
    {
        if (!t)
            return MGC_INVALID_ARGUMENT;
        std::lock_guard<std::mutex> lock(t->mutex);
        if (!t->done)
        {
            t->cancelled.store(true);
            t->result.result_discarded = 1;
        }
        return MGC_SUCCESS;
    }
    MgcStatus mgc_ticket_query(MgcTicket *t, MgcCompletion *out)
    {
        if (!t)
            return MGC_INVALID_ARGUMENT;
        std::lock_guard<std::mutex> lock(t->mutex);
        auto rc =
            output_struct(out, &t->result, sizeof(t->result), offsetof(MgcCompletion, reserved));
        return rc ? rc : t->done ? MGC_SUCCESS : MGC_BUSY;
    }
    MgcStatus mgc_session_drain(MgcSession *s, uint64_t ns)
    {
        if (!s)
            return MGC_INVALID_ARGUMENT;
        if (worker_session == s)
            return MGC_BUSY;
        std::unique_lock<std::mutex> lock(s->mutex);
        if (!s->in_flight)
            return MGC_SUCCESS;
        if (!ns)
            return MGC_TIMEOUT;
        auto limit = std::chrono::nanoseconds::max().count();
        auto bounded = static_cast<int64_t>(std::min<uint64_t>(ns, static_cast<uint64_t>(limit)));
        return s->retired.wait_for(lock, std::chrono::nanoseconds(bounded),
                                   [&] { return !s->in_flight; })
                   ? MGC_SUCCESS
                   : MGC_TIMEOUT;
    }
    MgcStatus mgc_private_submit_host_job(MgcSession *s, const MgcHostJobDesc *input,
                                          MgcTicket **out)
    {
        if (!s || !out)
            return MGC_INVALID_ARGUMENT;
        *out = nullptr;
        MgcHostJobDesc d{};
        auto rc = read_desc(input, d, sizeof(MgcHostJobDesc));
        if (rc)
            return rc;
        if (d.required_flags)
            return MGC_UNSUPPORTED_ABI;
        if (!d.execute || d.lease_count > 64 || (d.lease_count && !d.leases) ||
            bool(d.retain_context) != bool(d.release_context))
            return MGC_INVALID_ARGUMENT;
        bool retained = false;
        try
        {
            auto t = std::make_unique<MgcTicket>();
            t->desc.header = MGC_HEADER_INIT(MgcSubmitDesc);
            t->desc.request_id = d.request_id;
            t->desc.completion = d.completion;
            t->desc.context = d.context;
            t->desc.retain_context = d.retain_context;
            t->desc.release_context = d.release_context;
            t->execute = d.execute;
            t->result.header = MGC_HEADER_INIT(MgcCompletion);
            t->result.request_id = d.request_id;
            if (d.lease_count)
                t->leases.assign(d.leases, d.leases + d.lease_count);
            if (d.retain_context)
            {
                d.retain_context(d.context);
                retained = true;
            }
            rc = accept(s, t, out);
            if (rc != MGC_ACCEPTED && retained)
                invoke_context(d.release_context, d.context);
            return rc;
        }
        catch (...)
        {
            if (retained)
                invoke_context(d.release_context, d.context);
            return MGC_OUT_OF_MEMORY;
        }
    }
    uint32_t mgc_private_ticket_cancelled(MgcTicket *t)
    {
        return t && t->cancelled.load() ? 1u : 0u;
    }
    MgcStatus mgc_private_session_domain(MgcSession *s, MgcDomain **out)
    {
        if (!s || !out)
            return MGC_INVALID_ARGUMENT;
        mgc_domain_retain(s->domain);
        *out = s->domain;
        return MGC_SUCCESS;
    }
    MgcStatus mgc_android_import(MgcDomain *, const MgcAndroidImportDesc *, MgcLease **out)
    {
        if (out)
            *out = nullptr;
        return MGC_UNSUPPORTED_CAPABILITY;
    }
    MgcStatus mgc_android_export_release_fence(MgcLease *, int32_t *fd)
    {
        if (fd)
            *fd = -1;
        return MGC_UNSUPPORTED_CAPABILITY;
    }
}
#if MGC_HAVE_NCNN
namespace meguviz
{
MgcStatus with_ncnn_domain(MgcDomain *d, NcnnDomainFn fn, void *ctx)
{
    if (!d || !fn)
        return MGC_INVALID_ARGUMENT;
    if (d->runtime->provider == MGC_PROVIDER_CPU_REFERENCE)
        return MGC_UNSUPPORTED_CAPABILITY;
    std::lock_guard<std::recursive_mutex> lock(execution_slot);
    auto *previous = active_domain;
    active_domain = d;
    MgcStatus rc;
    try
    {
        rc = fn(ctx,
#if NCNN_VULKAN
                d->device
#else
                nullptr
#endif
        );
    }
    catch (const std::bad_alloc &)
    {
        rc = MGC_OUT_OF_MEMORY;
    }
    catch (...)
    {
        rc = MGC_INTERNAL_ERROR;
    }
    active_domain = previous;
    return rc;
}
const char *private_sdk_fingerprint()
{
    return MGC_PRIVATE_FP;
}
} // namespace meguviz
#endif
