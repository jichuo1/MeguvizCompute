# MeguvizCompute

当前是可运行的 **0.1.0 开发 SDK**。Windows 与 Android 模拟器上的 CPU 公共契约、真实 ncnn CPU 小图已验证；ARM64/ARMv7 的 ncnn Vulkan 静态依赖闭包已交叉构建并从安装目录独立链接。`bil-rife-ncnn-android` 的完整 C1–C6 资格尚未完成，不能称为 GPU/AHB 插帧引擎发布候选。逐 gate 状态与实际证据见 [STATUS.json](STATUS.json)。

## 已实现

- 版本化纯 C ABI；runtime/domain、资源身份与代际、内容 lease、显式 map/unmap。
- 有界任务 worker、提交引用转移、拒绝保留、取消与物理退役分离、最终 callback 一次、超时后继续 drain、消费者输出跨 session 保活。
- CPU copy、半像素 bilinear RGB8 resize、有限浮点 min-max normalization、statistics，以及通过真实 ncnn Input/ReLU 图执行的 `RELU_F32`。
- 私有 `backend_ncnn_sdk`：固定 ncnn 版本、serial execution slot、受 domain 保活的 ncnn device scope、可供其他算法提交的 host job。CPU domain 的 device 是 null；Vulkan domain 由 ncnn 实际创建。
- STATIC/PIC development packages、安装后的公共 C 与私有 custom-op consumers、配置期 fingerprint 与重复 ncnn owner 拒绝。
- 通用 affine shader 的离线编译与独立 host 静态校验。它尚未接入运行时 pipeline。

## 未实现与未取得的证据

`mgc_android_import` 与 native release-fence export 目前明确返回 `UNSUPPORTED_CAPABILITY`。AHB 资源适配、跨 API layout/ownership barrier、GPU 通用核同图链路、sync_fd 创建补丁、异步 ncnn 提交拆分、运行时 shaderpack/pipeline/cache 接入尚未完成。真实 GPU、hidden-submit trace、多设备、长期热负载、能量、音画与呈现证据均未取得。模拟器 CPU 结果不替代这些门槛。runtime glslang 在 Vulkan 组合中保留。

## 构建与验证

依赖源码位于授权的 `../dependencies/ncnn`，固定 commit `7e36d25cb153092ba87a60530455c7376fedf452`；其 glslang 子模块固定 `780b6a209132dc74e99458f62ebe5fa7e57c41cf`。`tools/verify_upstream_patches.py` 拒绝版本漂移或未登记 patch。SDK 不含 RIFE 模型、第三方转换权重、下载器或测试 DSO。

当前机器的可重复命令（Python 3.12.14；MSVC 19.51；NDK r30）：

```powershell
& 'C:\Users\Administrator\.cache\codex-runtimes\codex-primary-runtime\dependencies\python\python.exe' tools/verify_development_sdk.py
& 'C:\Users\Administrator\.cache\codex-runtimes\codex-primary-runtime\dependencies\python\python.exe' tools/verify_android_sdk.py --abi arm64
& 'C:\Users\Administrator\.cache\codex-runtimes\codex-primary-runtime\dependencies\python\python.exe' tools/verify_android_sdk.py --abi v7
& 'C:\Users\Administrator\.cache\codex-runtimes\codex-primary-runtime\dependencies\python\python.exe' tools/run_android_cpu.py --serial emulator-5732
```

最后一条只适用于已核实、无并行 instrumentation 冲突的当前模拟器；仅写 `/data/local/tmp/meguviz-01a1814c` 下自有测试。设备已变更时先重新核实，不启动或连接未知设备。

安装目录为 `build/install/windows-cpu`、`build/install/android-arm64-api27` 与 `build/install/android-v7-api27`。Android 外部消费者同时设置 `CMAKE_FIND_ROOT_PATH` 与 `CMAKE_PREFIX_PATH` 指向目标 SDK；`MGC_REQUIRED_PRIVATE_SDK_FINGERPRINT` 必须匹配对应 ABI 的 `build-info.json`。Windows/Linux/各 Android ABI 的 private fingerprint 彼此不同，运行时 ncnn Options 不属于编译身份。

```cmake
find_package(MeguvizCompute 0.1 CONFIG REQUIRED)
target_link_libraries(application PRIVATE MeguvizCompute::core)
# 只有内部算法适配器才使用：
target_link_libraries(algorithm PRIVATE MeguvizCompute::backend_ncnn_sdk)
```

先由 Compute 提供唯一 ncnn owner；提前自行创建另一 ncnn target 会在配置期失败。官方最终单 DSO 的组合、符号白名单和 JNI owner 归 Media distribution。

## 证据与回退

`artifacts/` 保存源码清单、已安装 SDK 的逐文件身份与真实测试摘要；完整 commands、stdout/stderr、CTest XML 和测试二进制保留在摘要指向的 `build/reports/`。开发 DSO 的 ELF 对齐通过只说明静态格式，尚无 16KB 环境运行证据。

基线保守配置是 `MGC_WITH_NCNN=OFF` 的 CPU reference；`MGC_VULKAN=OFF` 的 ncnn CPU 可独立消费。未 qualified 的 Android 加速路径 fail-closed，宿主应保留其原 legacy/原画回退。不要卸载在途代码或覆盖已装载 DSO。
