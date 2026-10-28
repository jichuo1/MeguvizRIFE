#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

namespace ncnn { class VulkanDevice; }

namespace meguviz::rife::legacy {

enum class Backend : int { Cpu = 0, Vulkan = 1 };
enum class Code : int {
    Ok = 0,
    InvalidArgument = 1,
    UnsupportedBackend = 2,
    GpuUnavailable = 3,
    ModelTooLarge = 4,
    InvalidModel = 5,
    ModelLoadFailed = 6,
    NotLoaded = 7,
    Busy = 8,
    AllocationFailed = 9,
    InferenceFailed = 10,
    InvalidOutput = 11
};

struct Status {
    Code code = Code::Ok;
    bool ok() const noexcept { return code == Code::Ok; }
    int value() const noexcept { return static_cast<int>(code); }
    const char* name() const noexcept;
};

struct Options {
    Backend backend = Backend::Vulkan;
    int gpuIndex = 0;
    // Borrowed from Compute; its retained domain outlives this engine.
    const ncnn::VulkanDevice* domainDevice = nullptr;
    int threads = 1;
    std::size_t maxPixels = 1920u * 1080u;
};

// Callers verify the approved model hashes before load. Buffers are borrowed only during load.
struct ModelView {
    const std::uint8_t* param = nullptr;
    std::size_t paramSize = 0;
    const std::uint8_t* weights = nullptr;
    std::size_t weightsSize = 0;
};

// Interleaved RGB8, top-to-bottom. Capacity includes the final row's visible bytes.
struct ImageView {
    const std::uint8_t* data = nullptr;
    int width = 0;
    int height = 0;
    std::size_t strideBytes = 0;
    std::size_t byteCapacity = 0;
};

struct MutableImageView {
    std::uint8_t* data = nullptr;
    int width = 0;
    int height = 0;
    std::size_t strideBytes = 0;
    std::size_t byteCapacity = 0;
};

// One bounded worker owns an engine. Concurrent load/process returns Busy without waiting.
// Destroy only after callers have stopped; the destructor never destroys ncnn's global GPU instance.
class Engine final {
public:
    static std::unique_ptr<Engine> create(const Options& options, Status* status = nullptr) noexcept;
    ~Engine();
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    // 在 load 前调用；fp16 由 ncnn 再按设备能力裁剪。flowScale 为 1/2/4/8（光流估计分辨率的倒数；8 用于 1440p 以上），
    // flowScale > 1 与 gpuIo（RGB8 转换在 GPU 完成）仅支持 Vulkan。调用即撤销已加载模型。
    // rawGpuInput 是显式研究选项；本机有较高 CPU/PSS 代价，不默认启用。
    // adaptiveFlow（须 flowScale 4）：像素数超过 1920×1080 的帧改用 1/8 光流，使光流输入在各分辨率下都约为
    // 480×270；另载 1/6 与 1/8 的无权重缩放网络，调用方也可逐次指定 4/6/8（1080p 的速度/画质档）。
    Status configure(bool fp16, int flowScale, bool gpuIo, bool protectStaticDetails = false, bool fusedWarp = false,
                     bool rawGpuInput = false, bool adaptiveFlow = false) noexcept;
    Status load(ModelView model) noexcept;
    // 最近一次成功插帧的阶段耗时（纳秒）：0 预处理，1 推理（含上传/下载），2 后处理。
    void timings(std::int64_t out[3]) const noexcept;
    // 最近一次融合插帧中运动置信度不足的像素比例（千分比，抽样）；外层据此把转场帧换成最近原帧。
    int unreliablePermille() const noexcept;
    // 最近一次插帧中因局部转场换成最近原帧的分块数（16×9 网格）。
    int patchedTiles() const noexcept;
    // 最近一次插帧两帧颜色分布差异（千分比）；外层据此区分切镜与快速摇镜。
    int histogramPermille() const noexcept;
    // 运动置信度保护默认开启；仅供对照测试关闭。
    void setMotionGuard(bool enabled) noexcept;
    // leftId/rightId ≥ 0 时按帧 ID 复用显存张量（仅 gpuIo 路径）；同一 ID 的内容必须不变，-1 表示不缓存。
    // flowScale > 0 时本次使用该光流比例（仅 adaptiveFlow，可为 4/6/8）；0 按像素数自动选择。
    Status processRgb8(ImageView left, ImageView right, float timestep, MutableImageView output,
                       std::int64_t leftId = -1, std::int64_t rightId = -1, int flowScale = 0) noexcept;
    Backend backend() const noexcept;
    bool ready() const noexcept;

private:
    class Impl;
    explicit Engine(std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> impl_;
};

} // namespace meguviz::rife::legacy
