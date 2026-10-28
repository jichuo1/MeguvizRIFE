#pragma once

#include <layer.h>
#include <memory>

namespace innocent::interpolation::detail {
// Adapted from nihui/rife-ncnn-vulkan's Warp, under upstream/LICENSE.
class Warp final : public ncnn::Layer {
public:
    Warp();
    int forward(const std::vector<ncnn::Mat>& input, std::vector<ncnn::Mat>& output,
                const ncnn::Option& options) const override;
#if NCNN_VULKAN
    int create_pipeline(const ncnn::Option& options) override;
    int destroy_pipeline(const ncnn::Option& options) override;
    int forward(const std::vector<ncnn::VkMat>& input, std::vector<ncnn::VkMat>& output,
                ncnn::VkCompute& command, const ncnn::Option& options) const override;
private:
    std::unique_ptr<ncnn::Pipeline> pipelines_[2];
#endif
};
ncnn::Layer* createWarp(void* userdata);
} // namespace innocent::interpolation::detail
