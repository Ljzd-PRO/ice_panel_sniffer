# 2026-07-04 最新研究结果：ESPHome 调试抓包与状态识别改进

本文记录在原始 `1 kHz` Arduino sniffer 数据之后，使用 ESPHome 调试固件继续采集和验证得到的新结论。目标机型为长虹 `CH-Z6Y3`。

## 新增数据包

```text
captures/ice_panel_esphome-debug-captures-20260704.tar.gz
```

该归档包含：

```text
20260704-213039-debug_smoke_network/
20260704-213218-debug_standby_dma/
20260704-222441-debug_small_dma/
dma_standby_small_analysis.md
old_baseline_analysis.md
```

其中 `debug_standby_dma` 和 `debug_small_dma` 是本轮最重要的两份数据，分别对应待机和小冰运行。每个目录通常包含：

```text
raw.log            ESPHome 网络日志原始文本
debug_frames.csv   从 D1-D4 结构化日志解析出的调试帧
markers.csv        采集标记
report.md          对该次抓包的自动分析报告
```

## 调试固件采样能力

ESPHome 调试固件使用 ESP32-C3 的 `ADC1_CH0-CH4`，即当前 `P1-P5 -> GPIO0-GPIO4` 映射。调试后端优先使用 ESP-IDF `adc_continuous` DMA。

实测结果：

| 数据 | 状态 | 时长 | 后端 | 完整 P1-P5 帧采样率中位数 | ADC 错误率中位数 |
| --- | --- | ---: | --- | ---: | ---: |
| `20260704-213218-debug_standby_dma` | 待机 | 约 87.6 s | ESP-IDF adc_continuous DMA | 约 8233.5 Hz | 0.000% |
| `20260704-222441-debug_small_dma` | 小冰 | 约 87.6 s | ESP-IDF adc_continuous DMA | 约 8227.0 Hz | 0.000% |

结论：高频 DMA 采样可用，但当前状态识别瓶颈不是采样频率，而是直连浮地信号本身的状态特征选择。生产 ESPHome 固件继续使用约 `1 kHz` 轮询即可。

## 待机与小冰的核心问题

旧数据和新 DMA 数据共同确认：

```text
待机 dominant signature: MHMHH
小冰 dominant signature: MHMHH
```

因此不能只用 `MHMHH` 判断小冰，也不能只用短窗口 `MHMHH` 离开待机状态。

新 DMA 数据中：

| 状态 | `MHMHH` 中位比例 | `0HHHH` 中位比例 |
| --- | ---: | ---: |
| 待机 | 约 74.6% | 约 1.0% |
| 小冰 | 约 71.8%-72.0% | 约 0.9% |

签名比例几乎重叠，必须加入二级特征。

## 最有效的二级特征

新 DMA 调试数据给出的待机/小冰分离特征如下：

| 特征 | 待机 p10/p50/p90 | 小冰 p10/p50/p90 | 小冰方向的规则 | 本轮数据准确率 |
| --- | ---: | ---: | --- | ---: |
| `P1 StdDev` | 459.8 / 531.9 / 667.1 | 761.7 / 772.8 / 796.5 | `P1 StdDev > 683.3` | 100% |
| `Delta P1 P3` | -248.5 / -216.8 / -144.9 | -95.9 / -86.9 / -72.6 | `Delta P1 P3 > -136.6` | 100% |
| `P2 StdDev` | 738.5 / 746.1 / 756.1 | 549.3 / 562.1 / 584.4 | `P2 StdDev < 724.1` | 100% |
| `Delta P2 P4` | -170.2 / -158.1 / -150.0 | -31.7 / -26.2 / -20.8 | `Delta P2 P4 > -146.5` | 100% |
| `Delta P5 P2` | 148.8 / 155.4 / 163.2 | -3.0 / 1.6 / 7.2 | `Delta P5 P2 < 143.4` | 100% |

生产固件没有直接采用所有特征，而是选择对旧 `1 kHz` 数据和新 DMA 数据都更稳的三项投票：

```text
P2 StdDev
Delta P2 P4
Delta P5 P2
```

## 当前推荐分类器

生产 ESPHome 固件的推荐逻辑是：

1. `0HHHH >= 65%`：大冰候选。
2. `MHMHH >= 55%`：进入待机/小冰二级判断。
3. 小冰投票：
   - `P2 StdDev <= 650`
   - `Delta P2 P4 >= -120`
   - `Delta P5 P2 <= 80`
4. 待机投票：
   - `P2 StdDev >= 700`
   - `Delta P2 P4 <= -145`
   - `Delta P5 P2 >= 120`
5. 得分达到 `2/3` 才生成对应候选。
6. 快速候选需要连续 2 次一致才用于更新状态。
7. 保留 16 秒电源灯慢闪窗口作为待机兜底。

## 状态机护栏

只改分类特征还不够。实机测试证明还需要状态机护栏：

- `standby -> running_small` 不允许由被动检测直接发生，因为 `CH-Z6Y3` 从待机启动必然先进入大冰。
- `standby -> running_large` 允许，可能来自原面板或远程开机。
- `running_large <-> running_small` 允许快速确认。
- 已确认待机时，短窗口偶发 `running_small` 或 `unknown` 不改变对外主状态。
- 已确认运行时，短窗口偶发 `unknown` 不立即改变对外主状态。
- 远程开机后的确认目标是 `running_large`。

## 实机稳定性结论

最新 ESPHome 固件在真实机器上完成过以下稳定性检查：

```text
130 s standby
130 s running_large
130 s running_small
130 s running_large after switching back
130 s standby after power off
```

这五个窗口内，对外 `State`、`Power`、`Large Ice` 均未出现异常跳变。

## 对后续开发的结论

- 状态识别不应只看 `ADC Signature`。
- 待机和小冰必须用短窗口二级特征区分，并用状态机约束过滤不可能的物理转移。
- DMA 调试固件适合继续做研究和采样对比，但生产固件不必切换到 DMA。
- `缺水`、`冰满` 在当前直连浮地 ADC 方案中仍不应作为可靠 Home Assistant 状态实体。
- UV 没有面板反馈，只能提供 toggle button，不能提供可靠 on/off switch。
