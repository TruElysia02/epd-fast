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

## 内容帧率与运动补帧

转换器和数据解码器支持 1…120 个内容帧/秒。对于帧率较低的原片，单独指定 `--fps 60` 或 `--fps 120` 会由 FFmpeg 选择或重复原有帧；增加 `--interpolate` 才会执行运动补帧：

```sh
python tools/pack_apple.py path/to/your-video.mp4 assets/badapple.bin --fps 60 --codec deflate --interpolate
```

补帧在缩放之后进行，使用 FFmpeg 的 `minterpolate`（运动估计补帧）滤镜，参数为 `mi_mode=mci:mc_mode=aobmc:me_mode=bidir:vsbmc=1:scd=fdiff`。生成的中间帧是估计结果，可能在遮挡、转场或高对比轮廓处出现瑕疵；不能称为原生 60FPS 或 120FPS 素材。

本次 Bad Apple 的原有画面只占原生行 63…233，两侧黑边始终为黑。运动补帧会使少量像素溢出黑边，复现本次转换时显式恢复这个范围之外的黑色：

```sh
python tools/pack_apple.py path/to/local-original.mp4 assets/badapple.bin --fps 60 --codec deflate --interpolate --content-rows 63 233
```

`--content-rows` 使用包含端点的原生行号，只恢复原本的黑边；它不改变面板 gate 扫描设置。其他视频应按自己的内容范围处理，不能直接套用 63…233。此次转换使用已有的本地原片，结果为 **13920 帧、232 秒、3,474,244 字节**，未下载现成的 60FPS 影片。设备端验证见 [60Hz 实验](../docs/60hz-experiment.md)。

## 120FPS 短片对照

`--start` 指定原片起点，`--duration` 指定要编码的时长，单位均为秒。下面从同一原片的 10…22 秒分别生成 60FPS 和 120FPS 运动补帧，不从已有的 60FPS 影片继续插帧：

```sh
python tools/pack_apple.py path/to/local-original.mp4 .scratch/120fps/motion60.bin --fps 60 --start 10 --duration 12 --codec deflate --interpolate --content-rows 63 233 --temporal-filter none
python tools/pack_apple.py path/to/local-original.mp4 .scratch/120fps/motion120.bin --fps 120 --start 10 --duration 12 --codec deflate --interpolate --content-rows 63 233 --temporal-filter none
```

本次本地原片生成的两份数据分别为 720 帧、172,121 字节和 1440 帧、265,647 字节，均为 12 秒。两组都使用 `none`，便于在相同运动片段上单独比较帧率；设备的驱动预算、波形和传输设置也应记录。软件能够编码、解码 120FPS 数据，不能据此认定设备已经完成每秒 120 次扫描，或面板已经实现光学 120FPS 转色。

## 可选的单帧闪回过滤

预录视频可增加 `--temporal-filter confirm2`。转换器逐像素预读下一帧，只有连续两帧保持新状态才接受变化；持续变化仍从第一帧开始，首末帧和总帧数不变。此处理保留所选帧率的时间采样，但会去掉仅出现一帧的细线、星点或轮廓。

```sh
python tools/pack_apple.py path/to/local-original.mp4 assets/badapple.bin --fps 60 --codec deflate --interpolate --content-rows 63 233 --temporal-filter confirm2
```

本次全片过滤后仍为 13920 帧、232 秒，数据缩为 3,260,412 字节。约 0.0476% 的像素时间样本发生改变，总转向减少 10.76%，四帧内的快速反向减少 36.46%。这是内容序列统计，实屏残影需要另行对照。完整记录见 [过滤统计](../measurements/60hz-temporal-filter.json)。默认不启用过滤。

`confirm2` 按帧确认：在 60FPS 下，单帧闪回持续约 16.67ms；在 120FPS 下约为 8.33ms。因此相同选项在两种帧率下并不是相同的时间过滤强度。帧率对照先使用两组 `none`，过滤候选另行比较。

## 播放与停止

播放全部内容可省略监看脚本的 `--seconds`。脚本运行时可将 .scratch/bad-apple/control.json 写为 `{"stop":true}` 请求冻结收尾；脚本会尝试确认停机和关闭升压。视频本身在设备上解码与播放，关闭电脑程序不等于停止设备。

直接向设备串口发送 `apple loop` 可循环播放完整视频，`apple loop 2` 可循环前 2 秒用于验证。每轮结束先完成剩余驱动，再全刷并从头开始；循环不依赖电脑持续发送命令。`stop` 或 `settle` 会取消循环，重启后仍回到停止状态。监看脚本用于单次播放，不负责启动或维持循环。

默认波形与传输参数保持保守设置。素材标成 60FPS 或 120FPS，只表示视频时钟按对应的目标帧率推进；设备是否及时解码和扫描、面板是否充分完成转色，需要分别检查。`MEASURE` 中的 `content_hz` 统计接受过的视频帧索引，即使连续索引的位图相同、无需扫描，也分别计数；它不代表不同画面数或光学转色率。实验参数与采样条件见 [60Hz 实验](../docs/60hz-experiment.md)。

## 桌面解码校验

需先生成 assets/badapple.bin 和同名 .hashes，然后在仓库根目录运行：

```sh
g++ -std=c++17 -O2 -Wall -Wextra -Werror -Isrc tests/apple_test.cpp lib/miniz/miniz_tinfl.c -o apple_test
./apple_test
```

Windows 可将输出名改为 apple_test.exe。测试逐帧核对生成时保存的 FNV-1a 校验值，并检查随机跳转、长度、偏移和损坏输入。deflate 格式还检查损坏的 zlib 校验信息。该命令必须链接附带的 miniz 解压实现；设备端使用 ESP32-C6 ROM 中的解压器。

## 数据格式

两种格式均使用 16 字节小端头：标识 4 字节、宽 u16、高 u16、fps u16、关键帧间隔 u16、帧数 u32；随后是帧数+1 个 u32 数据偏移和压缩帧。内容帧率允许 1…120，关键帧间隔允许 1…60 帧；转换器使用 `min(fps, 60)`，因此 120FPS 时每半秒一个关键帧。关键帧为绝对图，其余帧与前帧异或；解码跳转至多重建 60 帧。

| 标识 | 压缩帧格式 |
| --- | --- |
| `BA29` | 字节 RLE（游程编码）：控制字节最高位为重复/字面量，低 7 位为长度减一 |
| `BAZ9` | 每帧一个完整 zlib 数据流，含校验；解压后必须恰为 4736 字节 |

.hashes 保存每帧完整原生图像的 FNV-1a 参考值，用于桌面测试，也可与停机状态下的串口 `verify N` 对照。

## 素材来源记录

2026-10-29 的早期 20FPS 实测使用了 [bad-apple-lab/Bad-Apple 中的视频](https://github.com/bad-apple-lab/Bad-Apple/blob/main/badapple.mp4)，4641 帧、232.05 秒，RLE 数据约 3.08MB；完整播放记录见 measurements/video-playback.json。后续 60FPS 数据由本地原片补帧转换生成。来源说明不表示影片、音轨或转换帧获得了本项目 MIT 许可。

120FPS 短片对照同样来自该本地原片，原帧率为 30000/1001；截取 10…22 秒后分别补至 60FPS 和 120FPS。来源元数据记录原片 SHA256、截取区间、补帧与过滤参数、帧数、编码数据 SHA256 和桌面逐帧验证结果。测量记录应将这些来源参数与设备扫描率、实际接受的目标帧率、光学观察分别列出；不将影片或编码帧数据纳入仓库。
