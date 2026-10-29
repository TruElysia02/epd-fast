# 可选板载视频

默认构建不需要本目录中的数据。仓库仅提供转换与播放代码，不分发 Bad Apple 素材、原始视频或其转换产物。

准备自己有权使用的视频，并安装 FFmpeg（须能从命令行调用）和 tools/requirements.txt 中的依赖。普通 20FPS 转换与构建：

```sh
python tools/pack_apple.py path/to/your-video.mp4 assets/badapple.bin --fps 20
pio run -e xiao_esp32c6_video
pio run -e xiao_esp32c6_video -t upload --upload-port COM7
python tools/play_apple.py --port COM7 --seconds 12
```

数据文件名沿用 badapple.bin，但可装入其他内容。转换保留比例，放入横向 296×128，旋转到原生 128×296；去除音轨。4MB 闪存中应用和素材共用单一应用分区，超出空间会导致构建失败；可缩短视频、降低内容帧率或使用 deflate 编码。当前播放命令限长 233 秒。

## 60FPS 运动补帧

转换器支持 1…60 个内容帧/秒。对于帧率较低的原片，单独指定 `--fps 60` 会由 FFmpeg 选择或重复原有帧；增加 `--interpolate` 才会执行运动补帧：

```sh
python tools/pack_apple.py path/to/your-video.mp4 assets/badapple.bin --fps 60 --codec deflate --interpolate
```

补帧在缩放之后进行，使用 FFmpeg 的 `minterpolate`（运动估计补帧）滤镜，参数为 `mi_mode=mci:mc_mode=aobmc:me_mode=bidir:vsbmc=1:scd=fdiff`。生成的中间帧是估计结果，可能在遮挡、转场或高对比轮廓处出现瑕疵；不能称为原生 60FPS 素材。

本次 Bad Apple 的原有画面只占原生行 63…233，两侧黑边始终为黑。运动补帧会使少量像素溢出黑边，复现本次转换时显式恢复这个范围之外的黑色：

```sh
python tools/pack_apple.py path/to/local-original.mp4 assets/badapple.bin --fps 60 --codec deflate --interpolate --content-rows 63 233
```

`--content-rows` 使用包含端点的原生行号，只恢复原本的黑边；它不改变面板 gate 扫描设置。其他视频应按自己的内容范围处理，不能直接套用 63…233。此次转换使用已有的本地原片，结果为 **13920 帧、232 秒、3,474,244 字节**，未下载现成的 60FPS 影片。设备端验证见 [60Hz 实验](../docs/60hz-experiment.md)。

## 可选的单帧闪回过滤

预录视频可增加 `--temporal-filter confirm2`。转换器逐像素预读下一帧，只有连续两帧保持新状态才接受变化；持续变化仍从第一帧开始，首末帧和总帧数不变。此处理保留 60 次/秒的时间采样，但会去掉仅出现一帧的细线、星点或轮廓。

```sh
python tools/pack_apple.py path/to/local-original.mp4 assets/badapple.bin --fps 60 --codec deflate --interpolate --content-rows 63 233 --temporal-filter confirm2
```

本次全片过滤后仍为 13920 帧、232 秒，数据缩为 3,260,412 字节。约 0.0476% 的像素时间样本发生改变，总转向减少 10.76%，四帧内的快速反向减少 36.46%。这是内容序列统计，实屏残影需要另行对照。完整记录见 [过滤统计](../measurements/60hz-temporal-filter.json)。默认不启用过滤。

## 播放与停止

播放全部内容可省略监看脚本的 `--seconds`。脚本运行时可将 .scratch/bad-apple/control.json 写为 `{"stop":true}` 请求冻结收尾；脚本会尝试确认停机和关闭升压。视频本身在设备上解码与播放，关闭电脑程序不等于停止设备。

直接向设备串口发送 `apple loop` 可循环播放完整视频，`apple loop 2` 可循环前 2 秒用于验证。每轮结束先完成剩余驱动，再全刷并从头开始；循环不依赖电脑持续发送命令。`stop` 或 `settle` 会取消循环，重启后仍回到停止状态。监看脚本用于单次播放，不负责启动或维持循环。

默认波形与传输参数保持保守设置。素材标成 60FPS，只表示视频时钟按 60 个目标帧/秒推进；设备是否及时解码和扫描、面板是否充分完成转色，需要分别检查。`MEASURE` 中的 `content_hz` 统计接受过的视频帧索引，即使连续索引的位图相同、无需扫描，也分别计数；它不代表不同画面数或光学转色率。实验参数与采样条件见 [60Hz 实验](../docs/60hz-experiment.md)。

## 桌面解码校验

需先生成 assets/badapple.bin 和同名 .hashes，然后在仓库根目录运行：

```sh
g++ -std=c++17 -O2 -Wall -Wextra -Werror -Isrc tests/apple_test.cpp lib/miniz/miniz_tinfl.c -o apple_test
./apple_test
```

Windows 可将输出名改为 apple_test.exe。测试逐帧核对生成时保存的 FNV-1a 校验值，并检查随机跳转、长度、偏移和损坏输入。deflate 格式还检查损坏的 zlib 校验信息。该命令必须链接附带的 miniz 解压实现；设备端使用 ESP32-C6 ROM 中的解压器。

## 数据格式

两种格式均使用 16 字节小端头：标识 4 字节、宽 u16、高 u16、fps u16、关键帧间隔 u16、帧数 u32；随后是帧数+1 个 u32 数据偏移和压缩帧。每秒关键帧为绝对图，其余帧与前帧异或；解码跳转至多重建一个关键帧间隔。

| 标识 | 压缩帧格式 |
| --- | --- |
| `BA29` | 字节 RLE（游程编码）：控制字节最高位为重复/字面量，低 7 位为长度减一 |
| `BAZ9` | 每帧一个完整 zlib 数据流，含校验；解压后必须恰为 4736 字节 |

.hashes 保存每帧完整原生图像的 FNV-1a 参考值，用于桌面测试，也可与停机状态下的串口 `verify N` 对照。

## 素材来源记录

2026-10-29 的早期 20FPS 实测使用了 [bad-apple-lab/Bad-Apple 中的视频](https://github.com/bad-apple-lab/Bad-Apple/blob/main/badapple.mp4)，4641 帧、232.05 秒，RLE 数据约 3.08MB；完整播放记录见 measurements/video-playback.json。后续 60FPS 数据由本地原片补帧转换生成。来源说明不表示影片、音轨或转换帧获得了本项目 MIT 许可。
