# 独立源码目录的检查记录

检查日期：2026-11-03。以下区分本次新目录的构建结果和原工程的历史 RTL 结果。

| 项目 | 本次结果 |
|---|---|
| 源码实际复制、无源码软链接/嵌套 Git/缺失 filelist 文件 | 通过 `make audit` |
| Armv4 完整 IREE VM Runtime | 从本目录源码编译并合并静态库成功 |
| Rocket 所需 IREE base/HAL/executable helpers | 从本目录源码编译并合并静态库成功 |
| FESVR，包括 `tsi.cc` 和 `riscv/cfg.h` | 从本目录源码编译 `libfesvr.a` 成功 |
| DRAMSim2 | 从本目录源码编译静态库成功 |
| 10 个 DPI C++ 源文件 | 使用标准 DPI/VPI 头文件编译并与上述静态库链接成功 |
| 完整 RTL 和 SRAM 模型 | 本机 Verilator 解析/lint 通过；这不是 VCS 仿真结果 |
| IREE 编译器、模型与两核固件 | 新目录配置与 host flatcc 构建通过；全量 LLVM/IREE 构建未完成，保留增量缓存，可用 make firmware 继续 |
| 可选离线 Clang/LLD 工具链入口 | 提供源码与脚本；本机使用已安装的 LLVM 19，未另做一次工具链全量构建 |
| VCS 编译与完整推理 | 本机没有 VCS 安装与许可证，尚未执行 |

DPI 链接结果没有未解析的 `tsi_t`、`htif_t` 或 DDR 模型实现。剩余的 `svSize`、`svGetBitArrElemVecVal`、`svPutBitArrElem` 和 `vpi_get_vlog_info` 属于仿真器提供的接口。

首次构建缓存全部在新目录中产生，没有复制旧编译器、旧 Runtime 库、旧 FESVR 共享库或旧仿真器。Git 忽略这些新生成的构建产物；其他人下载后会重新构建。

## 原优化版本的历史结果

`reference/` 保存 H steps 6/7/8 的完整双核 RTL 运行记录：8 个 native dispatch export、34 个 workgroup，预测/参考类别均为 7，最大误差为 0。

4 线程 Verilator、关闭波形的历史运行时间为 1014.054 秒（16.901 分钟）。本仓库的正式运行入口使用 VCS；该历史时间不能作为 VCS 的实测时间。

## 在 VCS 服务器上复现

加载本机 LLVM 19 和 VCS 环境后，执行：

```bash
make doctor
BUILD_JOBS=4 make run
```

成功日志：`build/logs/vcs-mnist-console.log`（另有 VCS 的 `vcs-mnist.log`），应包含 `MNIST PASS (Arm IREE / Rocket / Gemmini)`。LSF 用户先执行 `make firmware`，再执行 `./scripts/submit_vcs.sh short`。
