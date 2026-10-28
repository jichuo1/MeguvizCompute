#include <cmath>
#include <cstdio>
#include <cstring>
#include <layer.h>
#include <meguviz/backend_ncnn_sdk.hpp>
#include <modelbin.h>
class ComputeScale final : public ncnn::Layer
{
  public:
    ComputeScale()
    {
        one_blob_only = true;
        support_inplace = true;
    }
    int load_param(const ncnn::ParamDict &pd) override
    {
        scale = pd.get(0, 1.f);
        return 0;
    }
    int forward_inplace(ncnn::Mat &m, const ncnn::Option &) const override
    {
        for (int q = 0; q < m.c; ++q)
        {
            float *p = m.channel(q);
            for (int i = 0; i < m.w * m.h; ++i)
                p[i] *= scale;
        }
        return 0;
    }
    float scale = 1;
};
DEFINE_LAYER_CREATOR(ComputeScale)
static MgcStatus graph(void *,
#if NCNN_VULKAN
                       const ncnn::VulkanDevice *device
#else
                       const void *device
#endif
)
{
    if (device)
        return MGC_UNSUPPORTED_CAPABILITY;
    ncnn::Net net;
    net.opt.use_vulkan_compute = false;
    net.opt.num_threads = 1;
    net.opt.use_packing_layout = false;
    net.opt.use_fp16_storage = false;
    net.opt.use_fp16_arithmetic = false;
    net.register_custom_layer("ComputeScale", ComputeScale_layer_creator);
    const char *param = "7767517\n3 3\nInput input 0 1 in0\nConvolution conv 1 1 in0 mid 0=1 1=1 "
                        "5=1 6=1\nComputeScale scale 1 1 mid out0 0=3.0\n";
    if (net.load_param_mem(param))
        return MGC_MODEL_MISMATCH;
    alignas(4) unsigned char weights[12] = {};
    float weight = 2, bias = -1;
    std::memcpy(weights + 4, &weight, 4);
    std::memcpy(weights + 8, &bias, 4);
    if (net.load_model(weights) != sizeof(weights))
        return MGC_MODEL_MISMATCH;
    ncnn::Mat input(3, 2, 1);
    for (int i = 0; i < 6; ++i)
        ((float *)input)[i] = float(i) - 2;
    auto ex = net.create_extractor();
    if (ex.input("in0", input))
        return MGC_INTERNAL_ERROR;
    ncnn::Mat output;
    if (ex.extract("out0", output))
        return MGC_INTERNAL_ERROR;
    for (int i = 0; i < 6; ++i)
        if (std::fabs(((float *)output)[i] - (((float *)input)[i] * 2 - 1) * 3) > 1e-6)
            return MGC_INTERNAL_ERROR;
    std::printf("{\"provider\":\"ncnn-cpu\",\"effective_options\":{\"vulkan\":false,"
                "\"packing\":false,\"fp16_storage\":false,\"fp16_arithmetic\":false,"
                "\"num_threads\":1},\"synthetic_graph\":\"conv1x1_customscale\","
                "\"oracle\":\"PASS\",\"private_sdk_fingerprint\":\"%s\"}\n",
                meguviz::private_sdk_fingerprint());
    return MGC_SUCCESS;
}
int main()
{
    MgcRuntimeDesc rd{};
    rd.header = MGC_HEADER_INIT(MgcRuntimeDesc);
    rd.provider = MGC_PROVIDER_NCNN_CPU;
    MgcRuntime *r = nullptr;
    if (mgc_runtime_create(&rd, &r))
        return 1;
    MgcDomainDesc dd{};
    dd.header = MGC_HEADER_INIT(MgcDomainDesc);
    MgcDomain *d = nullptr;
    if (mgc_domain_acquire(r, &dd, &d))
        return 2;
    auto rc = meguviz::with_ncnn_domain(d, graph, nullptr);
    mgc_domain_release(d);
    mgc_runtime_close(r);
    mgc_runtime_release(r);
    return rc ? 3 : 0;
}
