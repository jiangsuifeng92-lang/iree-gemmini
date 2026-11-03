# H 步骤 6、7、8：MNIST RTL 仿真对比

MNIST 双核 RTL 推理通过：预测 **7**，参考 **7**，最大误差为 0。
本次非 debug 仿真没有开启波形，使用 4 线程 Verilator；当前与基线模拟器可执行文件 SHA-256 相同（`71fae2a8415ebf0969d2029ff6c9805a9bc680acb2fcb469fe63cb5e5484da15`）。

| 指标 | 优化前 | 步骤 6/7/8 后 |
|---|---:|---:|
| RTL 仿真总墙钟时间 | 1276.561 秒（21.28 分钟） | 1014.054 秒（16.90 分钟） |
| 节省时间 | — | 262.507 秒（4.38 分钟） |
| 总体提速 | — | **1.259×**（减少 20.56%） |
| QUERY | 1 次请求、108 exports | 1 次请求、8 exports |
| Dispatch / workgroups | 108 / 134 | 8 / 34 |
| Rocket cycles since queue ready | 5,526,936 | 2,141,439 |

## 阶段墙钟时间

| 阶段 | 优化前 | 步骤 6/7/8 后 | 阶段提速 |
|---|---:|---:|---:|
| VMFB 加载 | 32.408 秒 | 62.776 秒 | 0.52× |
| Context 初始化与 QUERY | 608.508 秒 | 259.075 秒 | 2.35× |
| QUERY 完成到 module.main | 10.840 秒 | 21.300 秒 | 0.51× |
| 模型 dispatch | 582.349 秒 | 581.591 秒 | 1.00× |
| 读取输出与校验 | 27.158 秒 | 59.087 秒 | 0.46× |

主要收益来自 context/QUERY：耗时从 608.508 秒降到 259.075 秒，约快 2.35×。模型 dispatch 基本持平（582.349 秒对 581.591 秒）。VMFB 加载和输出校验在本次反而更慢，所以总体仿真减少约五分之一。

旧版 108 个 dispatch 请求覆盖 134 个 workgroup；新版为 8 个请求覆盖 34 个 workgroup。一次 QUERY 从 108 个 export 缩为 8 个。入口数量和工作项都下降，但模型 dispatch 墙钟没显著变化，不能把减少请求数直接当作 Gemmini 计算提速。

核对上一个版本的 Gemmini C wrapper 计数和当前一样为 0；该计数无法看到静态链接 kernel 内部直接发出的 Gemmini 指令。此次没有波形，故不能给出 Gemmini busy 周期的直接前后对比。Rocket 队列周期和仿真总墙钟均有记录。

优化前计时文件：`steps678_baseline_mnist_timing.json`；本次计时：`iree_mnist_timing.json`；完整字段：`steps678_performance_comparison.json`。
