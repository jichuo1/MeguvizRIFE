// Adapted from nihui/rife-ncnn-vulkan a7532fc3, src/warp.cpp. MIT; see LICENSE.
#include "rife_warp.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <new>

#if NCNN_VULKAN
#include "warp.comp.hex.h"
#include "warp_pack4.comp.hex.h"
#endif

namespace meguviz::rife::legacy::detail {
Warp::Warp() {
#if NCNN_VULKAN
    support_vulkan = true;
    support_vulkan_packing = true;
#endif
}

ncnn::Layer* createWarp(void*) { return new (std::nothrow) Warp(); }

int Warp::forward(const std::vector<ncnn::Mat>& input, std::vector<ncnn::Mat>& output,
                  const ncnn::Option& options) const {
    if (input.size() != 2 || output.size() != 1) return -1;
    const ncnn::Mat& image = input[0];
    const ncnn::Mat& flow = input[1];
    if (image.empty() || flow.empty() || image.dims != 3 || flow.dims != 3 || image.w != flow.w ||
        image.h != flow.h || image.elempack != 1 || flow.elempack != 1 || flow.c != 2 ||
        image.elemsize != sizeof(float) || flow.elemsize != sizeof(float)) return -1;
    ncnn::Mat& result = output[0];
    result.create(image.w, image.h, image.c, static_cast<std::size_t>(sizeof(float)), options.blob_allocator);
    if (result.empty()) return -100;
    for (int q = 0; q < image.c; ++q) {
        const ncnn::Mat plane = image.channel(q);
        float* destination = result.channel(q);
        const float* flowX = flow.channel(0);
        const float* flowY = flow.channel(1);
        for (int y = 0; y < image.h; ++y) {
            for (int x = 0; x < image.w; ++x) {
                const float sampleX = x + *flowX++;
                const float sampleY = y + *flowY++;
                if (!std::isfinite(sampleX) || !std::isfinite(sampleY)) return -1;
                // Clamp before conversion as well, so extreme flow cannot overflow float-to-int.
                const float boundedX = std::max(0.f, std::min(sampleX, static_cast<float>(image.w - 1)));
                const float boundedY = std::max(0.f, std::min(sampleY, static_cast<float>(image.h - 1)));
                const int x0 = static_cast<int>(std::floor(boundedX));
                const int y0 = static_cast<int>(std::floor(boundedY));
                const int x1 = std::min(x0 + 1, image.w - 1);
                const int y1 = std::min(y0 + 1, image.h - 1);
                const float a = boundedX - x0, b = boundedY - y0;
                const float upper = plane.row(y0)[x0] * (1.f - a) + plane.row(y0)[x1] * a;
                const float lower = plane.row(y1)[x0] * (1.f - a) + plane.row(y1)[x1] * a;
                *destination++ = upper * (1.f - b) + lower * b;
            }
        }
    }
    return 0;
}

#if NCNN_VULKAN
int Warp::create_pipeline(const ncnn::Option& options) {
    if (!options.use_vulkan_compute || vkdev == nullptr) return 0;
    struct Source { const char* data; int size; };
    const Source sources[] = {{warp_comp_data, sizeof(warp_comp_data)-1},
        {warp_pack4_comp_data, sizeof(warp_pack4_comp_data)-1}};
    for (int i = 0; i < 2; ++i) {
        std::vector<std::uint32_t> spirv;
        if (ncnn::compile_spirv_module(sources[i].data, sources[i].size, options, spirv) != 0 || spirv.empty()) return -1;
        auto pipeline = std::unique_ptr<ncnn::Pipeline>(new (std::nothrow) ncnn::Pipeline(vkdev));
        if (!pipeline) return -100;
        pipeline->set_optimal_local_size_xyz();
        if (pipeline->create(spirv.data(), spirv.size() * sizeof(std::uint32_t), {}) != 0) return -1;
        pipelines_[i] = std::move(pipeline);
    }
    return 0;
}

int Warp::destroy_pipeline(const ncnn::Option&) {
    for (auto& pipeline : pipelines_) pipeline.reset();
    return 0;
}

int Warp::forward(const std::vector<ncnn::VkMat>& input, std::vector<ncnn::VkMat>& output,
                  ncnn::VkCompute& command, const ncnn::Option& options) const {
    if (input.size() != 2 || output.size() != 1) return -1;
    const ncnn::VkMat& image = input[0];
    const ncnn::VkMat& flow = input[1];
    const int slot = image.elempack == 1 ? 0 : image.elempack == 4 ? 1 : -1;
    if (image.empty() || flow.empty() || image.dims != 3 || flow.dims != 3 || image.w != flow.w || image.h != flow.h ||
        flow.elempack != 1 || flow.c != 2 || slot < 0 || !pipelines_[slot]) return -1;
    ncnn::VkMat& result = output[0];
    result.create(image.w, image.h, image.c, image.elemsize, image.elempack, options.blob_vkallocator);
    if (result.empty()) return -100;
    std::vector<ncnn::VkMat> bindings{image, flow, result};
    std::vector<ncnn::vk_constant_type> constants(4);
    constants[0].i = result.w;
    constants[1].i = result.h;
    constants[2].i = result.c;
    constants[3].i = static_cast<int>(result.cstep);
    command.record_pipeline(pipelines_[slot].get(), bindings, constants, result);
    return 0;
}
#endif
} // namespace meguviz::rife::legacy::detail
