# RIFE抽取与reference资格

源码来自 bil 74d78e19095baa006b1315001190241952f1b84a 的 player_interpolation；原始MIT Warp许可和bil GPLv3保留。
初始抽取与变更记录见 artifacts/R0/extraction.json。模型权重不复制、不入SDK/APK/仓库。

公开接口由G1的 mgr_ C ABI定义。算法实现使用Compute private SDK和串行domain scope，取消/退役由Compute票据统一维护。
原Engine的全局create_gpu_instance被移除，VulkanDevice必须由Compute提供且保活domain；不得自行创建/销毁ncnn GPU实例。

第一条真实模型切片是CPU FP32 full-graph reference。该路径与旧GPU reduced-flow/FP16/motion-tile guard不相同，不用于声称Legacy等价或性能提升。
未取得C2/ARM GPU资格前，公开plan拒绝Legacy FP16/guard配置。原算法/shader代码保留待后续逐项验证，存在源码不等于已qualified。

模型加载只接受冻结v4.6 catalog的完整规范、精确大小和双SHA；解析采用严格JSON并拒绝重复key、非法UTF8、浮点和大小深度越界。
private SDK编译身份精确匹配，运行有效Options属于独立plan语义。后续图/精度不复用当前模型名与327/332约定。

输出由Compute资源分配，写入完成后发布read lease；最终回调只在Compute host finalize/retire后一次，consumer持有输出独立于session drain。
只支持内部输出分配，拒绝外部写target。旧C++ Engine只在私有后端使用，不暴露到公共ABI。
