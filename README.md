# EPD Fast

在 **Seeed XIAO ESP32-C6 + 2.9 寸 T94 黑白墨水屏**上验证逐像素短脉冲驱动的实验。包含驱动固件、两种像素规划策略、测试画面、串口测量工具和实机日志。

2026-10-29，同一块实物最初测得约 **38.5 次扫描/秒**；后续调整波形扫描频率、局部上传和重复掩码复用后，20FPS 视频短测的活动扫描率提高到约 **119.7Hz**。这里的扫描率是控制器完成短扫描的次数，与视频内容帧率、像素完成黑白转色的速度不同。

60FPS 运动补帧内容已在设备上播到片尾；统计区间的内容索引接受率均值约 **59.94Hz**，排除定期全刷停顿。最快档位下用户观察到更流畅，同时残影明显加重；降到档位 5 后残影减轻，增加到 11/11 预算后略有改善。随后计算流水线使同段活动扫描率从 **83.13Hz 提高到 97.56Hz**，用户确认残影或对比有改善。三档历史反向补偿、追加保持阶段及最轻一档弱收尾均未胜过固定预算，恢复关闭；残影仍未消除。结果、统计限制和实屏反馈见 [60Hz 实验](docs/60hz-experiment.md)。尚未做光学反射率和面板寿命测量，不能把内容推进率当作光学 60FPS。

默认参数仍为目标 30Hz、波形频率档位 2、全屏 RAM 上传、不复用掩码、轮询等待，并保留 3 秒诊断等待；高扫描率配置通过串口显式开启。上电不会自动播放视频或进入循环。

思路参考 [SmoothEPD](https://github.com/PinkiePie1/SmoothEPD) 和 [InkBoy](https://github.com/kuslota/InkBoy)，这里的规划器和 ESP32-C6 传输实现独立编写。波形表改自 Waveshare，并保留其授权声明。代码来源见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。

## 硬件与接线

已测主控为 XIAO ESP32-C6。屏幕依据原有工程配置判断为 GDEM029T94 / SSD1680，原生 128×296，横向 296×128；没有通过读取芯片 ID 确认型号。只有这组实物经过验证，同接口或同尺寸的其他面板需要另行适配。

| 屏幕信号 | XIAO 引脚 | GPIO |
| --- | --- | ---: |
| SCLK | D0 | 0 |
| MOSI | D1 | 1 |
| RST | D2 | 2 |
| DC | D3 | 21 |
| CS | D4 | 22 |
| BUSY（高电平忙） | D5 | 23 |

还需共地，并按所用屏幕转接板的规格供电。本接线针对带驱动电路的 SPI 屏幕模块；裸屏排线不能直接接 MCU。

## 编译与运行

安装 [PlatformIO Core](https://docs.platformio.org/en/latest/core/installation/index.html)，在仓库根目录运行。默认版本自带方块、圆点、细线和计时画面，无需准备视频素材：

```sh
pio run
pio run -t upload --upload-port COM7
pio device monitor --port COM7
```

将 COM7 换成自己的串口；Linux 常见为 /dev/ttyACM0。上传会覆盖板上现有固件。串口为 115200。上电先显示全刷参考图，发送命令后才开始高刷。

| 命令 | 用途 |
| --- | --- |
| `budget` | 默认策略：目标改变时分配新的黑白脉冲预算 |
| `net` | 限制累计黑白脉冲差，比较运动拖影和对比度 |
| `blocks` / `line` / `reverse` / `clock` | 切换测试画面 |
| `fps 15` / `fps 30` / `fps 0` | 设置扫描目标；0 为不限速 |
| `apple` / `apple loop` | 视频版播放一遍 / 在设备上循环播放 |
| `fr 0` … `fr 7` | 设置波形扫描频率档位；先全刷停机，再重新开始 |
| `window full` / `window partial` | 两平面完整上传 / 上传当前及上次活动行的并集 |
| `reuse on` / `reuse off` | 跳过内容相同的控制器 RAM 平面 / 每次上传 |
| `wait irq` / `wait poll` | BUSY 下降沿唤醒 / 轮询等待 |
| `pipeline on` / `pipeline off` | BUSY 期间预计算下一轮，成功后接纳影子状态；默认关闭 |
| `paired on` / `paired off` | 每次驱动后追加一阶段保持，增加扫描忙时；默认关闭 |
| `history 100` / `history 150` / `history 200` / `history off` | 按估计位置调整未完成反向的剂量，默认关闭 |
| `weakprobe N [weak\|sameA\|sameB]` | 用 1…11 次脉冲比较两种底色上的源状态，sameA/sameB 为映射对照，结束关升压 |
| `weak 1` … `weak 4` / `weak off` | 黑色尾部改用弱态；实验模型按两次弱态算一次强态，真实比例尚未校准；默认关闭 |
| `scan full` / `scan 63 233` | 全部 gate 行 / 指定实际 gate 扫描范围；与上传窗口不同 |
| `guard 0` / `guard 3000` | 全刷后额外诊断等待，单位 ms；不会省略硬件复位 |
| `verify N` | 仅在停机时解码视频第 N 帧，输出校验值与解码耗时 |
| `settle` | 完成当前目标的剩余预算，关闭升压并保留画面 |
| `stop` | 标准全刷并关闭升压 |
| `status` / `help` | 状态与完整命令表 |

可先发送 `budget`，观察方块和背景文字，再切换 `net` 对比。实验结束发送 `stop`。高刷期间模拟升压持续开启；当前参数是实验设置，尚未验证长期使用寿命。

固件默认每 45 秒或 2000 次扫描执行标准全刷，以先到者为准。高扫描率下会更早达到 2000 次上限。默认全刷切换快刷保留 3 秒诊断等待，整次停顿约 4.75 秒；可用 `guard 0` 取消额外等待，硬件复位与重新配置仍会执行。全刷停顿不计入短时扫描率。

## 测量与桌面验证

可选 Python 工具依赖：

```sh
python -m pip install -r tools/requirements.txt
python tools/measure.py --port COM7 --mode budget --seconds 7 --output .scratch/measurement.json
```

脚本会启动高刷、读取统计，结束时全刷停机并确认错误状态与升压状态。它会改变屏幕内容。原始历史记录与测量条件见 [measurements/README.md](measurements/README.md)。

桌面测试只需支持 C++17 的 g++，不接触设备：

```sh
g++ -std=c++17 -O2 -Wall -Wextra -Werror -Isrc tests/planner_test.cpp -o planner_test
./planner_test
g++ -std=c++17 -O2 -Wall -Wextra -Werror -Isrc tests/planner_equivalence.cpp -o planner_equivalence
./planner_equivalence
```

Windows 可将输出名改为 planner_test.exe / planner_equivalence.exe。测试覆盖预算、反向、累计脉冲差、提交时机、随机参考实现对照和 RAM 行范围；软件测试不能替代实屏观察。

## 可选视频

保留了板载无声视频播放器与转换工具；仓库不附 Bad Apple 视频或转换数据。使用自己有权使用的视频生成数据后，单独编译视频环境，步骤见 [assets/README.md](assets/README.md)。默认版本收到 `apple` 会明确提示视频不可用。

## 实验中发现的问题

最初在标准全刷后直接装载快刷波形，静态文字会逐渐变浅。改为全屏传输和延长等待仍无法解决；补回硬件复位并重新配置 T94 后，用户确认准备阶段和后续约 481 次扫描中的文字保持正常。该修复已保留。后续局部上传与掩码复用已完成视频短测，日志未报告故障；这不替代静态保持、对比度和残影的光学检查。

初始参数、策略说明与变浅问题的定位见 [早期实验记录](docs/experiment.md)，后续频率档位、传输优化和 60FPS 内容验证见 [60Hz 实验](docs/60hz-experiment.md)。本项目只验证黑白高刷，没有实现四灰阶、流体模拟或通用面板兼容。

## 许可

本项目原创代码采用 [MIT](LICENSE) 许可。第三方代码及依赖保留各自许可证；视频素材不包含在该授权内。
