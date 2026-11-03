# MNIST：IREE → Arm + Rocket/Gemmini → 双核 RTL

这是可独立放入 Git 的完整源码快照。所需源码均为真实文件，没有源码软链接、Git 子模块或对旧 Chipyard checkout 的构建依赖。

默认采用 **H steps 6/7/8 已通过双核 RTL 验证的优化版本**：

- Armv4 兼容软核运行完整 IREE VM Runtime、VMFB 和远程 HAL。
- Rocket 处理共享队列，执行 native dispatch/Gemmini C kernel。
- 图级卷积恢复、整数后处理融合、编译期分块：本模型生成 **8 个 dispatch**，共 **34 个 workgroup**。
- **VCS** 直接编译仓库内完整 `.v/.sv` 和真实 DPI/TSI/FESVR 源码。

## 1. 所需本机工具

Linux x86-64，建议内存 16 GB 以上、可用磁盘至少 30 GB。默认同时编译 4 个任务；内存较少时设 `BUILD_JOBS=2`。

| 工具 | 用途 |
|---|---|
| Python 3.8+ | 配置、VMFB 嵌入和 ELF 加载镜像生成 |
| CMake 3.26+、Ninja、GNU Make | 源码构建 |
| Clang/LLVM/LLD 19 | 本机 IREE 编译器与 Armv4 裸机固件 |
| g++，支持 C++17 | 在当前服务器重建 FESVR/DDR/DPI |
| dtc | FESVR/Spike 上游配置检查 |
| 已安装并授权的 Synopsys VCS | RTL 编译与仿真 |

仓库附带 RISC-V GCC 13.2 裸机工具链与 Arm newlib/libgcc 的必要支持文件。GCC 运行依赖本机的 libc、libgmp、libmpfr、libmpc、zlib；libfl/libzstd 已随工具链附带。也可设置 `RISCV=/path/to/riscv RISCV_CC=/path/to/riscv/bin/riscv64-unknown-elf-gcc` 使用自己安装的工具链。

没有安装 LLVM 19 时，可以先用本机 GCC/g++（建议 11+）从仓库内源码离线构建 Clang/LLD 和 LLVM 工具：

~~~bash
BUILD_JOBS=4 make toolchain
~~~

之后的根目录脚本会自动使用 `build/host-toolchain/bin/`，无需安装到系统目录。这个额外步骤也不访问网络，但会增加首次构建时间和磁盘需求。

默认流程读取已附带常量与权重的 `cnn.mlir`、输入与参考结果 `cnn_io.h`，不需要 ONNX/PyTorch Python 包，也不访问网络。原始 ONNX 和导出脚本也已保留。

~~~bash
make doctor
make audit
~~~

`doctor` 只报告缺失工具，不自动联网安装。没有 VCS 的机器仍可构建 IREE、模型与两核固件；RTL 仿真需要本机 VCS 安装与许可证。

## 2. 从下载的源码完整构建

进入本目录，加载本机 LLVM/VCS 环境：

~~~bash
# 本机编译 IREE → 生成模型与两核固件 → 编译 RTL → 执行 MNIST
BUILD_JOBS=4 make run
~~~

首次 IREE/LLVM 编译需要较长时间，后续构建增量更新。也可分步执行：

~~~bash
make compiler   # 生成带 Gemmini 插件的 iree-compile
make model      # 生成 model.vmfb、model_lib.o 和头文件
make firmware   # 重建两核所需 IREE 库及两核固件
make dpi        # 本机重建完整 FESVR（含 tsi.cc）和 DRAMSim2
make vcs        # 直接编译本仓库的 RTL/DPI
./scripts/run_vcs.sh
~~~

`make firmware` 自动构建编译器和 Runtime，使用本目录的新构建缓存。Arm 库包含 VM 解释器与 HAL；Rocket 库包含实际使用的 IREE base/设备 executable ABI helpers。所需库均从本目录的 IREE 源码重新编译。

### LSF 集群

先完成 `make firmware`，再在已有 VCS 环境中提交：

~~~bash
./scripts/submit_vcs.sh short
~~~

队列可改为集群允许的名称；作业执行 VCS 编译和推理，日志写入 `build/logs/lsf-vcs-<jobid>.log`。

### 波形

普通运行关闭波形以减少开销。需要 VPD：

~~~bash
make wave
~~~

生成 `build/vcs/mnist.vpd`。顶层 `profileSignals` 保留 Rocket retire PC/指令、DDR AR/R、I-cache、RoCC opcode/funct/rs1/rs2 和 Gemmini DMA/busy 信号。

## 3. 目录

~~~text
third_party/
  iree/                 匹配版本的 IREE 和必要第三方源码
  llvm-project/         一份匹配且带原工程补丁的 LLVM/MLIR/Clang/LLD 源码
  riscv-isa-sim/         完整 FESVR/TSI/HTIF 上游源码，含 riscv/cfg.h
  DRAMSim2/             DDR 模型源码和配置
F_gemmini_backend/
  compiler/             Gemmini target 与图级恢复/融合/分块/lowering passes
  kernels/              Gemmini C kernel
  runtime/              SDK、启动汇编、裸机 libc/allocator 和 ABI 头文件
  apps/mnist/           ONNX、MLIR、输入/参考结果和导出脚本
  toolchain/riscv/       离线裸机交叉工具链支持
G_arm_gemmini/
  firmware/host/        Arm 启动、IREE 应用、HAL device、transport
  firmware/npu/         Rocket 队列服务和 dispatch executor
  runtime/armv4/        Armv4 Runtime 兼容覆盖
  include/              共享队列和地址空间协议
  hardware/、rtl/       原硬件配置与 Arm AXI wrapper 源码
  third_party/arm9/     Arm 软核原始源码和来源信息
rtl/                    完整可直接编译的 ArmGemminiConfig .v/.sv 快照
sim/csrc/               真实 SimDRAM/SimTSI/SimUART/SimJTAG 及辅助 C++ 源码
config/、include/       Gemmini 硬件参数和编译计划 ABI
scripts/                独立构建、运行、提交、检查入口
reference/              原优化版本的数值验证与性能记录
build/                  当前机器的编译器、DPI、VCS 和日志（Git 忽略）
~~~

这里只保留一份实际使用的 LLVM。IREE/torch-mlir 内原来重复的 LLVM 副本由显式 `IREE_BUNDLED_LLVM_DIR` 配置替代；本模型只启用 StableHLO、Gemmini/LLVMCPU 与 RISC-V/X86 LLVM 目标。首次构建不需要下载 Git 子模块。

## 4. 产物与执行链

~~~text
build/compiler/third_party/iree/tools/iree-compile
G_arm_gemmini/build/model/mnist/
  model.vmfb              Arm VM 模型流程
  model_lib.o             IREE 生成的 RISC-V dispatch 机器码与导出表
  model_lib.h             库查询入口声明
  model_registry.h        Rocket 静态库登记
  model_vmfb.h            VMFB 原始字节数组
  gemmini_*.jsonl          图恢复、融合和分块报告
  model.vmfb.txt          VMFB dump
  model_lib.disasm.txt    对象反汇编
G_arm_gemmini/build/iree-firmware/mnist/
  host/host.hex           Arm boot RAM
  host/host.elf           Arm Runtime + HAL + VMFB
  npu/npu.elf             Rocket dispatch + model_lib.o + kernel
  loadmem.elf             两核共享 DDR 预加载镜像
~~~

~~~text
Arm 复位 → start.S → iree_main.c::host_main
  → IREE VM 执行 module.main
  → HAL issue_call → SRAM submission queue → CLINT doorbell
  → Rocket drain_queue → exports.ptrs[ordinal](env, dispatch, workgroup)
  → native dispatch → Gemmini C kernel → RoCC → Gemmini
  → completion(sequence/status) → Arm HAL 返回 → IREE 继续
~~~

真实 TSI 完成 Rocket BootROM 启动握手，并响应 `tohost/fromhost`。脚本保持 `+permissive` 在前、`+permissive-off npu.elf` 在最后。DPI include/静态库路径只在构建目录展开成当前仓库绝对路径，不会指向作者机器。

## 5. 成功条件与测量范围

最终日志 `build/logs/vcs-mnist-console.log`（以及 VCS 的 `vcs-mnist.log`）应包含：

~~~text
MNIST PASS (Arm IREE / Rocket / Gemmini)
~~~

预测与参考类别均为 **7**，最大误差为 **0**，有 8 个 dispatch 的设备执行记录。

`reference/` 中的 **1014.054 秒（16.901 分钟）** 是原优化版本的 **4 线程 Verilator 历史实测**，使用相同双核硬件且没有波形。这是目前项目已有记录中最快的完整推理版本；它不代表 VCS 时间，也不保证在任何服务器都是最快。VCS 用时以新服务器实测为准。

本次独立整理的构建检查结果见 `VALIDATION.md`。

## 6. 上传 Git

把本目录作为仓库根目录上传：

~~~bash
git init
git add .
git status --short
git commit -m "Add standalone optimized IREE MNIST dual-core RTL project"
git remote add origin <your-repository-url>
git push -u origin HEAD
~~~

无需 `git submodule update`。根 `.gitignore` 只忽略构建产物；已移除上游嵌套 Git 元数据、忽略与 LFS 规则，避免漏提交必要文件。`make audit` 检查源码软链接、关键文件、相对 filelist 和单个超过 100 MiB 的文件。无需另行上传旧 bundle、RTL/DPI addon 或历史构建缓存。

各上游许可证位于第三方目录与 `licenses/`，复制来源见 `SOURCE_ORIGINS.json`，各目录的文件数与字节数见 `SOURCE_INVENTORY.json`。
