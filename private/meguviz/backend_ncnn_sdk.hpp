#ifndef MEGUVIZ_BACKEND_NCNN_SDK_HPP
#define MEGUVIZ_BACKEND_NCNN_SDK_HPP
#include <meguviz/compute_job.h>
#include <net.h>
#if NCNN_VULKAN
#include <gpu.h>
#endif
namespace meguviz {
/* Runs under the same domain serial slot used by tickets. CPU receives null
 * device. device and allocator objects may not escape this call unless their
 * owner retains the domain and never uses them outside another such scope.
 * Reentrant calls on the active worker scope are allowed. */
using NcnnDomainFn = MgcStatus (*)(void *context,
#if NCNN_VULKAN
    const ncnn::VulkanDevice *device
#else
    const void *device
#endif
);
MgcStatus with_ncnn_domain(MgcDomain *domain, NcnnDomainFn fn, void *context);
const char *private_sdk_fingerprint();
}
#endif
