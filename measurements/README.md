# 实机记录

以下为 2026-10-29 的历史实测日志，硬件为同一块 XIAO ESP32-C6 与 T94 配置的 2.9 寸屏。整理时去除了旧测量脚本 arguments 中的本机输出路径，保留统计与串口正文。文件名 v2 / v5 对应实验迭代阶段，不是本仓库的发布版本。

| 文件 | 条件与用途 |
| --- | --- |
| stable-tail-trials.json | 45FPS、FR5、11/11 下，稳定约 0.2 秒后额外补驱的基线与试验；补偿计数是像素×脉冲，不是额外扫描次数 |
| v2-normal-10.json | 10MHz、结束参数 0x22，局部行上传 |
| v2-fast-10.json | 10MHz、结束参数 0x02，局部行上传 |
| v2-fast-20.json | 20MHz、结束参数 0x02，局部行上传 |
| v2-net-fast-20.json | 相同传输设置，net 策略 |
| prepare-with-reset.json | 补回硬件复位后的快刷准备阶段 |
| v5-animation-full.json | 修复后，全屏 RAM 写入的动画测试，约 38.5Hz |
| video-playback.json | 方向修正后的完整无声播放；内容 20fps，包含 5 次定期全刷与最终关闭升压 |
| 60hz-frequency-sweep.json | 全部 296 行、全零保持掩码，频率档位 0…7 各 32 次的 BUSY 时序 |
| 60hz-20fps-comparison.json | 旧 20FPS 素材上依次加入局部 RAM 上传、下降沿唤醒、平面复用和 gate 裁剪的短测 |
| 60fps-source-processing.json | 本地 29.97FPS 原片运动补帧、黑边恢复、抽样与编码校验记录；不含媒体 |
| 60hz-60fps-short-tests.json | 60FPS 补帧素材的档位 4/5/7 短测与设备帧校验值对照 |
| 60hz-full-playback.json | 档位 7 的完整 232 秒内容；保留监控超时的原始失败标记，并单列片尾及随后停机确认 |
| 60hz-fr5-quality-trial.json | 档位 5、7/7 预算的实机观察；用户反馈残影减轻但仍存在 |
| 60hz-fr5-budget11-trial.json | 档位 5、11/11 预算；用户反馈略有改善，未确认消除残影 |
| 60hz-fr5-budget7-11-trial.json | 档位 5、7/11 预算；用户对照后偏好 11/11 |
| 60hz-fr5-budget11-7-trial.json | 档位 5、11/7 预算；用户要求恢复 11/11，未明确评价该组优劣 |
| 60hz-temporal-filter.json | 连续两帧确认过滤的全片离线统计、校验值与生成参数；光学效果单独观察 |
| 60hz-followup-trials.json | 同段过滤、流水线、历史剂量、追加保持阶段的短测与用户观察；六段活动窗口汇总 |
| 60hz-pipeline-loop-validation.json | 最终 FR5、11/11、流水线配置的 260 秒监控，整片片尾及自动重启、短循环、三帧 ROM 校验 |
| 120fps-source-processing.json | 同一原片 10…22 秒独立生成 60/120FPS，位图差异、校验和转换参数；不含媒体 |
| 120fps-trials.json | 30/45/60/120FPS 与各驱动档位的同片段对照，整轮接受/跳过计数、循环重复及解码基准 |
| 45fps-final-loop.json | 最终整片45FPS、FR5、11/11与流水线的260秒监控，累计计数闭合及片尾自动重启 |

v2 速度对照为移动色块、80px/s、7/7 预算、不限速，每组两段 2 秒日志的均值。v5 与当前默认运动速度为 24px/s，不应把这两组解释为只改变一个变量的对照。

MEASURE 的 scan_hz 包含补偿及保持扫描，排除了 CLEAN 全刷停顿。它不代表像素完成黑白转色的帧率。日志中的 fault=0、analog=0 是固件状态，不能证明光学质量、实际残余电压或面板寿命。文字保持与方向修正结论来自当时用户的实屏观察，未附仪器测量或照片。

后续 `content_hz` 统计软件接受的不同视频帧索引，即使连续索引的图像相同、无需扫描也会计数；`skipped` 为该区间跳过的索引数。它们不能当作不同画面数或光学转色率。

60FPS 整片日志中的 `completed_first_pass=false`、`loop_started=false` 是监控脚本原始结果：脚本没有及时读到完整片尾自动状态行。设备正文包含 `APPLE end` 和收尾关电事件，后续显式状态确认健康停机；补充证据放在 `observations`，没有抹去原始失败标记。频率、速度、残影与统计限制的解读见 [60Hz 实验](../docs/60hz-experiment.md)。
