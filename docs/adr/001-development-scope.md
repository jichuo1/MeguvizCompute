# ADR-001：保守 development SDK 与 selected profile 资格分离

采用 Compute 唯一固定 ncnn provider、独立 STATIC/PIC SDK 与 Media 最终组合。公共 C ABI 不暴露 ncnn；私有 SDK 用源码、无 patch ledger、实际编译选项、compiler bytes、平台配置与工具链生成 fingerprint。运行时各图 Options 独立记录，不强制全组合 FP16 一致。

第一实现以真正的 CPU/reference 工作与 ncnn 小图验证资源和 ticket；不生成假 GPU handle、假完成 fence 或假性能数字。Vulkan domain 可以由真实 ncnn 创建，但 AHB/同域通用 GPU 数据链路与异步补丁未实现，相关 gate BLOCKED。

跨 ABI 安装 SDK 由外部消费工程重链；版本失配和重复 ncnn owner 配置期拒绝。sourceKey/build fingerprint 覆盖公共头、私有头及库 source inputs。runtime glslang 不删除。消费者持有的输出继续保活资源/domain；session drain 不回收这些输出。

独立联调曾发现显式 allocation 1/2 与自动 ID 冲突，已在域锁内跳过占用身份，原失败证据保留且添加同触发条件回归。ShaderTools reflection hash 的 newline 身份差异由独立文件 bytes 校验发现，修复后的固定工具重新消费通过；不放宽 validator。

本阶段产物是 development SDK，不满足 C2–C6 的 bil-qualified 候选定义；后续先补真实 AHB/barrier slice 与设备，再推进 GPU sync/优化。回退使用 CPU/reference 或宿主 legacy/原画。禁止把 ELF 16KB 对齐当成 16KB 真运行，禁止把 CPU 模拟器当 Adreno/Mali。
