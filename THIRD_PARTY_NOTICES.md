# Third-party notices

src/waveform.h contains a modified version of Waveshare's `_WF_PARTIAL_2IN9`
table from Arduino/epd2in9_V2/epd2in9_V2.cpp in
https://github.com/waveshareteam/e-Paper.
Its original Copyright (C) Waveshare Nov 9 2020 and complete permission notice
are retained in src/waveform.h. Changes are listed beside the array and in README.md.

The pixel-budget and net-drive concepts are based on the public descriptions
and implementations in [PinkiePie1/SmoothEPD](https://github.com/PinkiePie1/SmoothEPD/tree/242c5bab652c4001eec39bf0d277e95f96673ed0)
and [kuslota/InkBoy](https://github.com/kuslota/InkBoy/tree/823265ea5a9307749e5f61ea07eb7a40df793cc8). The planner and
ESP32-C6 transport here are independently written for this experiment; no WCH
hardware implementation or full-refresh waveform from SmoothEPD is included.

SmoothEPD did not include an explicit repository license in the inspected
revision. Its public visibility is not treated as permission to redistribute
its code. This repository references its ideas and does not relicense it.
InkBoy uses the MIT license, copyright (c) 2026 kuslota; no InkBoy source files
are bundled here. InkBoy attributes the pixel-state approach to Wenting Zhang's
PaperBoy. This is a chain of ideas, not a claim of shared Git history.

The T94 controller setup was checked against GxEPD2_290_T94 in
https://github.com/ZinggJM/GxEPD2. This project does not bundle or link GxEPD2.

PlatformIO fetches Adafruit GFX Library 1.12.6 and Adafruit BusIO 1.17.4
(BSD license notices in their library distributions), Arduino-ESP32 3.3.8
(LGPL-2.1), and Espressif's supporting ESP-IDF libraries (Apache-2.0 and
component-specific notices). Their licenses remain applicable; this file does
not replace their full license texts.

The repository license covers original project code and documentation, not
external dependencies or any video supplied by the user. No original Bad Apple
video, soundtrack, or converted video frames are distributed here.
