# H_arm_gemmini_compiler_opt：步骤 6、7、8 的实现

本文保留原 H 优化版本的设计说明。独立工程的构建和 VCS 运行方式见根目录 [README](../README.md)；源码现已实际拷贝到本仓库。

## 6：在 dispatch 划分之前优化生产者与消费者

新增 `GemminiGraphOptimization.cpp` 中的两个 module pass，并从 `GemminiTarget.cpp` 的 IREE 插件钩子接入：

- **GemminiRestoreConvolution**：在输入转换前识别 StableHLO 的 `slice → reshape → concatenate → dot_general`。只有完整、按 KH/KW 顺序排列的静态 NHWC 窗口，batch=1，步长 1/2，以及普通 i8×i8→i32 矩阵收缩才会改写。验证源张量、所有 slice 范围、步长、通道、权重形状和收缩维度后，生成带显式滑窗索引的 linalg 卷积。
- **GemminiGraphFusion**：在图级 preprocessing 阶段，使用 MLIR 标准融合模式合并单消费者、纯 tensor、全 parallel 的整数逐元素操作。保留 reduction/convolution 根，供后面的 Gemmini 选择 pass 使用。

这会让原本逐个产生 dispatch 的窗口整理操作成为一个卷积的内部工作，实际 dispatch 划分仍由 IREE 完成。它不实现任意模型的通用四维 packed layout 传播，也不把多个已形成的队列请求在驱动中合并。

设备 lowering 的融合匹配也已收紧：检查完整 SSA 运算链、bias 广播映射、整数类型、移位范围和饱和值；支持透明 collapse/expand/cast 视图，以及 int8 截断后的 ReLU；从底层 allocation 追踪别名，检查其他消费者。只有私有临时累加器且没有额外消费者才融合；HAL 输出绑定及有其他消费者的结果保留原有写回。保留后续输出 copy，避免错误绕过仍被使用的临时输出。

## 7：分块卷积与融合后处理

新增 `gemmini_conv2d_i8_requant_planned`，它执行：

```text
收集 block_m 个输出位置的输入窗口
    → 连续 [block_m,K] im2col tile
    → Gemmini 矩阵乘法
    → bias + 精确固定点 requantize + clamp
    → 写出这一块 int8 结果
    → 复用工作区，继续下一块
```

普通 int32 卷积和融合 int8 卷积共用 `h_im2col_chunk`，块可以跨输出图像的行边界；尾块按有效输出位置数截短。编译器传入静态分块、布局和软件行块大小。卷积匹配同时支持普通六循环和 IREE 折叠 C=1 后的五循环形式，并验证 signed i8 扩展、乘法和累加的实际运算体。共享队列 ABI 保持不变，新参数仍在 native dispatch 的调用中。

- 最大 im2col 工作区：128×1152 字节，即 144 KiB。
- 编译器每块有效 im2col 数据目标：64 KiB。
- 融合卷积入口不需要完整 `[OH*OW,OC]` 的 int32 中间结果。
- 一般固定点量化仍需要将当前 int32 块写入 `g_cp`，由 Rocket 处理后写出 int8。
- 静态工作区继续依赖当前串行设备服务；不支持多个并发 kernel 共用它。

## 8：区分片上后处理与精确软件后处理

实际 Gemmini 配置的 accumulator scaling 使用 FP32 转换与 round-near-even。模型的量化公式是：

```text
sum32 = acc32 + bias32                 （32 位模加法）
v64   = sign_extend(sum32) * mult + round  （64 位模运算）
v64   = arithmetic_shift_right(v64, shift)
v32   = truncate_to_i32(v64)
out8  = truncate_to_i8(clamp(v32, min, max))
```

这两种舍入通常不等价。因此实现了两个明确分支：

| 条件 | 实际实现 | int32 结果是否写 DDR |
|---|---|---|
| mult=1、round=0、shift=0、max=127，min=-128 或 0 | bias 作为 repeating i32 D 输入；恒等缩放，必要时 ReLU；直接输出 int8 | 这一分支不写 `g_cp` |
| 其他受支持的固定点量化 | 写当前 int32 tile，再由 Rocket 执行精确整数公式 | 是，仅当前 tile |

片上分支仅覆盖恒等缩放与普通 int8 饱和/ReLU，不将近似 FP32 缩放用于一般 MNIST 量化。`gemmini_epilogue.h` 实现共用的条件判断和精确整数运算；使用无符号中间值明确表达 MLIR 的模运算，避免 C 有符号溢出造成语义差异。

用于能力解释的实际 Scala 源码副本在 `hardware-reference/gemmini/`。没有修改原硬件，也没有在 H 中添加通用 64 位整数 requantize 电路。

## 构建与记录

```bash
cd /home/aiden/iree_rocket_gemmini/H_arm_gemmini_compiler_opt
./scripts/build_mnist.sh
```

模型输出目录中的新增记录：

- `gemmini_graph.jsonl`：恢复了哪些显式 im2col 链。
- `gemmini_plans.jsonl`：卷积/矩阵计算的分块与布局。
- `gemmini_epilogues.jsonl`：实际被设备 lowering 融合的后处理及路径条件。

设置 `H_GEMMINI_NO_GRAPH_OPT=1` 可关闭两个新图级 pass；设置 `GEMMINI_NO_FUSE_EPILOGUE=1` 可关闭设备后处理融合。用于后续对照时，应单独保存输出目录，避免覆盖前一份构建产物。

本次任务只构建代码和固件；不运行数值测试或 RTL 仿真，不能从编译结果推断实测加速。

## RTL 仿真结果

已在相同的非 debug、4 线程 Verilator 可执行文件上运行。与步骤 6/7/8 前的 1276.561 秒相比，本次总仿真 1014.054 秒，约快 1.259×，减少 20.56%。Context 初始化与 QUERY 从 608.508 秒降至 259.075 秒；模型 dispatch 基本持平（582.349 秒对 581.591 秒）。VMFB 加载和输出校验本次较慢。

[阶段、队列计数和时钟域详细对比](../G_arm_gemmini/artifacts/steps678_performance_comparison.md)。本次未开启波形；静态链接的 Gemmini kernel 未被现有 C wrapper 计数器统计，所以没有 Gemmini busy 周期的直接测量。

## 本次实际构建结果

H 编译器、MNIST VMFB、RISC-V 设备对象、Arm/Rocket 固件和 DDR 加载文件均已构建成功。

| 项目 | 构建中观察到的结果 |
|---|---|
| 图级恢复 | 两条卷积输入链，每条原有 25 个窗口 |
| native dispatch 入口 | 基线 108 个，本次 8 个 |
| 进入 Gemmini 的计算入口 | 4 个：两条卷积、两条全连接 |
| 融合整数后处理 | 3 个：两条卷积、第一条全连接（包括末尾 ReLU） |
| 本模型片上恒等缩放分支 | 0 个；三个融合入口均选择精确整数 DDR tile 路径 |
| 原 F/G 文件 | 快照中的 798 个文件全部保持原内容 |

### 编译器实际传给 kernel 的计划

| 入口 | 矩阵 M×N×K | tile_i / tile_j / tile_k | block_m 输出位置数 |
|---|---|---|---|
| `main_dispatch_0_conv_24x24x10x5x5_i8xi8xi32` | 576×10×25 | 8 / 1 / 2 | 128 |
| `main_dispatch_2_conv_8x8x20x5x5x10_i8xi8xi32` | 64×20×250 | 4 / 2 / 16 | 64 |
| `main_dispatch_4_vecmat_50x320_i8xi8xi32` | 1×50×320 | 1 / 4 / 20 | 1 |
| `main_dispatch_5_vecmat_10x50_i8xi8xi32` | 1×10×50 | 1 / 1 / 4 | 1 |

`tile_i/j/k` 是 16×16 硬件 tile 的数量，`block_m` 是软件工作区本次收集的输出位置数；这些参数不是 IREE workgroup 数量。

### 生成文件与记录

- `G_arm_gemmini/build/model/mnist/model.vmfb`：Arm 运行的模型流程。
- `G_arm_gemmini/build/model/mnist/model_lib.o`：8 个 RISC-V native dispatch 入口，引用 planned kernel 符号。
- `G_arm_gemmini/build/iree-firmware/mnist/host/host.elf`：Arm 固件。
- `G_arm_gemmini/build/iree-firmware/mnist/npu/npu.elf`：链接设备入口、C kernel 和队列服务后的 Rocket 固件。
- `G_arm_gemmini/build/iree-firmware/mnist/loadmem.elf`：双核 DDR 加载段。
- [完整构建状态与产物 SHA256](../artifacts/steps678_status.json)。
- [编译器构建日志](../artifacts/steps678_compiler_build.log)、[模型及固件构建日志](../artifacts/steps678_mnist_build.log)、[RTL 阶段日志](../G_arm_gemmini/artifacts/iree_mnist_soc.log)、[计时数据](../G_arm_gemmini/artifacts/iree_mnist_timing.json)。

108→8 是编译生成入口数量的变化；实际 RTL 观察到 dispatch 从 108 条、134 个 workgroup 降为 8 条、34 个 workgroup，QUERY 返回的 export 从 108 个降为 8 个。MNIST RTL 输出预测 7，参考 7，误差 0。步骤 8 的首版是受条件限制的片上路径与精确软件路径；一般整数 requantize 的完整硬件电路仍未实现。
