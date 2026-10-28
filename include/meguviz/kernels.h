#ifndef MEGUVIZ_KERNELS_H
#define MEGUVIZ_KERNELS_H
#include "compute.h"
#ifdef __cplusplus
extern "C" {
#endif
/* CPU reference operations use the normal mgc_submit ownership contract.
 * COPY: byte_count bytes; input READ, output WRITE.
 * NORMALIZE_F32: byte_count is input bytes, divisible by 4; finite float32
 * values become (x-min)/(max-min), or zero when min==max. Output has same size.
 * RELU_F32: finite float32 elementwise max(0,x); byte_count input bytes,
 * divisible by 4. ncnn-cpu uses a real Input/ReLU graph; CPU_REFERENCE uses
 * scalar reference. Vulkan variant remains explicitly unsupported.
 * STATISTICS_F32: same input; output is MgcStatisticsF32, including nonfinite
 * count. min/max/sum cover finite inputs only; no finite values => min=max=0.
 * RESIZE_RGB8: byte_count must be zero. Resource width/height/row_stride are
 * authoritative. Half-pixel bilinear, clamp-to-edge, round-to-nearest integer
 * (ties upward), channels=3, byte-preserving SDR values. No color conversion.
 * These operations advertise CPU_REFERENCE only, even in an ncnn domain. */
typedef struct MgcStatisticsF32 {
    MgcHeader header;
    uint64_t count;
    uint64_t finite_count;
    double sum;
    float minimum;
    float maximum;
} MgcStatisticsF32;
#ifdef __cplusplus
}
#endif
#endif
