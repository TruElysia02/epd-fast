# EPD Fast

在 **Seeed XIAO ESP32-C6 + 2.9 寸 T94 黑白墨水屏**上验证逐像素短脉冲驱动的实验。包含驱动固件、两种像素规划策略、测试画面、串口测量工具和实机日志。

2026-10-29，同一块实物在完整 RAM 写入、20MHz SPI、不限速条件下，移动图案测得约 **38.5 次扫描/秒**。默认运行目标为 30Hz。扫描率表示控制器完成短扫描的次数；一个像素通常需要 7 次扫描预算，不能把它理解为每秒完成 38.5 张充分黑白转色的图像。尚未做光学反射率和面板寿命测量。

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
| `settle` | 完成当前目标的剩余预算，关闭升压并保留画面 |
| `stop` | 标准全刷并关闭升压 |
| `status` / `help` | 状态与完整命令表 |

可先发送 `budget`，观察方块和背景文字，再切换 `net` 对比。实验结束发送 `stop`。高刷期间模拟升压持续开启；当前参数是实验设置，尚未验证长期使用寿命。

固件默认每 45 秒或 2000 次扫描执行标准全刷，以先到者为准。全刷切换快刷时保留 3 秒诊断等待，整次停顿约 4.75 秒；这些停顿不计入短时扫描率。

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

最初在标准全刷后直接装载快刷波形，静态文字会逐渐变浅。改为全屏传输和延长等待仍无法解决；补回硬件复位并重新配置 T94 后，用户确认准备阶段和后续约 481 次扫描中的文字保持正常。该修复已保留，局部 RAM 写入路径尚未重新做实机验证。

完整参数、策略说明、版本差异与验证过程见 [实验记录](docs/experiment.md)。本项目只验证黑白高刷，没有实现四灰阶、流体模拟或通用面板兼容。

## 许可

本项目原创代码采用 [MIT](LICENSE) 许可。第三方代码及依赖保留各自许可证；视频素材不包含在该授权内。
