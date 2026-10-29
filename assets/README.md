# 可选板载视频

默认构建不需要本目录中的数据。仓库仅提供转换与播放代码，不分发 Bad Apple 素材、原始视频或其转换产物。

准备自己有权使用的视频，并安装 FFmpeg（须能从命令行调用）和 tools/requirements.txt 中的依赖。在仓库根目录执行：

```sh
python tools/pack_apple.py path/to/your-video.mp4 assets/badapple.bin --fps 20
pio run -e xiao_esp32c6_video
pio run -e xiao_esp32c6_video -t upload --upload-port COM7
python tools/play_apple.py --port COM7 --seconds 12
```

数据文件名沿用 badapple.bin，但可装入其他内容。转换保留比例，放入横向 296×128，旋转到原生 128×296；去除音轨。4MB 闪存中应用和素材共用单一应用分区，超出空间会导致构建失败；可缩短视频或降低内容帧率。当前命令限长 233 秒。

播放全部内容可省略 --seconds。监看脚本运行时可将 .scratch/bad-apple/control.json 写为 `{"stop":true}` 请求冻结收尾；串口 `settle` 保留画面，`stop` 全刷停机。关闭电脑监看脚本不等于设备自动停机，应确认脚本报告 healthy idle。

解码器与转换输出核对：

```sh
g++ -std=c++17 -O2 -Wall -Wextra -Werror -Isrc tests/apple_test.cpp -o apple_test
./apple_test
```

数据格式：16 字节小端头（BA29、宽 u16、高 u16、fps u16、关键帧间隔 u16、帧数 u32），随后是帧数+1 个 u32 偏移和字节 RLE 数据。控制字节最高位为重复/字面量，低 7 位为长度减一；每秒关键帧为绝对图，其余帧与前帧异或。配套 .hashes 用于逐帧 FNV-1a 校验。

2026-10-29 的历史实测使用了 [bad-apple-lab/Bad-Apple 中的视频](https://github.com/bad-apple-lab/Bad-Apple/blob/main/badapple.mp4)，4641 帧、232.05 秒、20 内容帧/秒，编码数据约 3.08MB。此链接仅记录历史来源，不表示素材获得了本项目 MIT 许可。完整播放记录见 measurements/video-playback.json。
