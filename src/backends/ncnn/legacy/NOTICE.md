# Native interpolation source lock

Only the RIFE v4 forward path and its custom Warp are adapted here. This directory contains no
model weights, CLI, image codec, filesystem model loader or third-party downloader.

- RIFE upstream: https://github.com/nihui/rife-ncnn-vulkan
- Source/tag: `a7532fc3f9f8f008cd6eecd6f2ffe2a9698e0cf7` / `20221029`, MIT, retained in `upstream/LICENSE`.
- `rife_warp.cpp` derives from `src/warp.cpp`; the two `.comp` files are unchanged upstream shaders.
- `frame_engine.cpp` follows the v4 no-TTA flow in `src/rife.cpp`: RGB / 255, zero padding to 32,
  inputs `in0/in1/in2`, output `out0`, crop, round and clamp to RGB8.
- ncnn is supplied by the parent target: Tencent/ncnn `7e36d25cb153092ba87a60530455c7376fedf452`
  / `20261020`, BSD-3-Clause and its bundled notices.
- Vulkan shader compiler closure: nihui/glslang `780b6a209132dc74e99458f62ebe5fa7e57c41cf`.

Local adaptations use a private namespace, check shapes/statuses and finite output, bound memory
reads and image capacities, support CPU-only builds, and avoid global GPU destruction. CPU Warp
clamps coordinates before float-to-int conversion. TTA, UHD and older model variants are omitted.
The generic ncnn Extractor handles transfers for Vulkan; this is not a zero-copy Surface renderer.
Vulkan defaults to float32 pack1/pack4. fp16 storage/arithmetic, reduced-resolution flow (1/2, 1/4 or 1/8, or adaptive: 1/4 up to 1920×1080 pixels and 1/8 above, with a per-call 1/4, 1/6 or 1/8 choice)
and GPU RGB8 conversion are explicit options set through `configure` before `load` (the host player enables all three); ncnn still
clips fp16 to device support. Pixel positions in the fused warp and the RGB8 reduction, and the downscale Interp, use fp32 arithmetic because fp16 spacing is 2 px above 2048. Reduced-resolution flow runs the unchanged v4.6 graph on downscaled inputs,
reads its flow (blob 327) and mask (blob 332), then upsamples them (flow values scaled by the same factor)
and repeats the graph's final Warp/blend at full resolution. GPU conversion uploads raw RGB8 bytes and
unpacks, normalizes and pads them in `frame_preproc.comp`; `frame_postproc.comp` scales, rounds, clamps and
packs the output back to RGB8 and flags any non-finite value, so only raw bytes cross the CPU/GPU boundary. The obsolete upstream `use_shader_pack8` option is absent from
the pinned ncnn API, and pack8 stays disabled.

Callers authenticate the approved model bytes before `load`. Model buffers are borrowed only for
that call and are not retained. Failed model replacement disables the previous model. A single
worker owns an engine; concurrent work returns `busy`. Destruction requires callers to have stopped.
Only complete successful RGB output is committed, including when input/output buffers alias.

Converted v4.6 weights remain research-only. Their presence in the upstream repository is not
treated here as proof of a separate model redistribution grant. They must not enter APK assets.
