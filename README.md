# MeguvizRIFE

独立 RIFE 算法库，当前可运行切片为 **真实 v4.6 CPU FP32 全图 reference**。
公开 C ABI 仅依赖 Compute；不依赖 Media、bil、Android UI，不自行创建或销毁 ncnn GPU 实例。

已经实际验证：固定权重双 SHA、有界模型读取、两个尺寸（含奇数尺寸）、alpha 0/0.5/1、独立旧 CPU 引擎逐字节对照、A/A、失败请求、输出跨 session/plan 关闭存续、纯 C 安装消费，以及 ARM64/ARMv7 静态 SDK 交叉构建。

当前没有取得 GPU reduced-flow、完整 motion/tile/static guard 迁移、AHB、native fence、真实呈现、音画或性能收益资格。公开 plan 会拒绝未支持的 FP16/guard 配置。原算法和 shader 源码保留供下一阶段验证，源码存在不代表该路径已支持。

## 构建

先按 Compute 的说明构建固定 ncnn development SDK，设置 `MEGUVIZ_COMPUTE_PREFIX`，在对应编译器环境运行：

```sh
cmake --preset host-reference
cmake --build --preset host-reference
ctest --preset host-reference
cmake --install build/host-reference --prefix out/install/reference
```

可显式指定 `-DMGR_REQUIRED_PRIVATE_SDK_FINGERPRINT=<实际Compute私有SDK指纹>`；失配会在配置阶段拒绝。
安装包提供 `MeguvizRIFE::core` 与 `MeguvizRIFE::ncnn`，静态链接闭包由 Compute 的安装配置传递。

Android 使用 API27、NDK30、`c++_static`，设置 `MEGUVIZ_NDK_ROOT`、`MEGUVIZ_COMPUTE_ANDROID_PREFIX` 后使用 `android-arm64` preset。
ARMv7 可以在独立构建目录显式传 `ANDROID_ABI=armeabi-v7a`。交叉构建不等于设备运行资格。

## 真实模型验证

模型权重外置，不随仓库、SDK、APK 分发。只有 `models/catalog/rife46.manifest.json` 对应的精确 v4.6 模型可加载。

```sh
mgr-cli inspect-model
mgr-cli --version-json
mgr-cli interpolate --run-spec pair.json --report result.json
python tests/run_reference.py --binary build/host-reference/mgr-cli --oracle build/host-reference/mgr-legacy-oracle --model-dir /approved/rife-v4.6 --out artifacts/R1/reference
```

`pair.json` 使用 `schema_version:1`、`model_directory`、P6 RGB8 `left/right` 文件、字符串 `alpha`、`output` 路径和 `precision_profile:"CPU_FP32_REFERENCE"`。
示例数据可由 `tests/run_reference.py` 确定性生成。报告将端点与成功推理分开，并明确 CPU reference 不能作为原 GPU 缩流配置的等价 oracle。

## 证据与状态

实际命令、原始输出和逐文件哈希位于 `artifacts/R0` 与 `artifacts/R1`。`ARTIFACTS.json` 指向当前构建与验证收据；`STATUS.json` 逐项列出 R0—R5 的已完成子项和阻塞。
`tests/oracle` 是原 bil 固定提交的未改算法，作为另一个进程运行；新实现不能调用它来制造自身对照。

当前交付是 development SDK。没有 R2/R3/C5 资格，不称 R5 发布候选，也不称 bil I5 完成。实验 R4 不属于默认支持范围。

本库包含 bil GPLv3 派生代码，保留根 `LICENSE`；上游 Warp 的 MIT 许可保留在私有算法目录。模型的研究用途与分发限制单独记录，不由源码许可代替。
