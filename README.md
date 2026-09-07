# ScreenRecord · 全屏录屏工具(gogui / C++)

基于 **gogui**(C++ / ImGui 风格 GUI)的屏幕录制工具:

- **抓屏**:Windows GDI `BitBlt`(全系统自带,无第三方依赖)
- **编码**:**OpenH264(H.264)** 静态链接进 exe —— 不依赖目标机是否装有系统编码器,
  可分发到任意 Win7+ 机器
- **封装**:自写极简 **MP4** 容器(`avc1/avcC`),纯文件 I/O,无 FFmpeg/Media Foundation

## 功能特性

- 一键全屏录制,输出标准 `.mp4`(H.264),常规播放器直接可放。
- **3 秒倒计时**后开始录制(期间可取消)。
- 录制中窗口**自动隐藏**,不会把自己录进画面。
- 快捷键 **Alt+Z** 随时停止(全局热键)。
- **系统托盘**:双击/右键菜单可“显示主窗口 / 退出”。
- 可选手动保存目录(默认 `桌面\ScreenRecorder`),重名自动加 `(n)`。
- 帧率可选 **5 / 10 / 15 / 20 / 30 FPS**,界面实时显示分辨率、已写帧数与时长。
- 自带滚动日志,显示录制结果(路径 / 帧数 / 时长 / 体积)。

## 目录结构

```
ScreenRecord/
├── main.cpp            # 入口:初始化 gogui、建窗、主循环
├── main_ui.h/.cpp      # 界面模块(gogui):控制面板/倒计时/托盘/Alt+Z
├── recorder.h/.cpp     # 录制核心(无 UI):GDI 抓屏 + OpenH264 编码
├── mp4_mux.h/.cpp      # 极简 MP4 封装器(纯文件 I/O)
├── CMakeLists.txt      # 构建脚本(目标 ScreenRecorder.exe)
├── build.bat           # 一键构建(MinGW + CMake)
└── README.md
```

模块划分:

- `recorder` / `mp4_mux` —— 纯功能、可独立测试(不依赖 gogui/UI)。
- `main_ui` —— 只负责界面呈现与交互,把录制交给 `recorder`。
- `main.cpp` —— 最薄入口。

## 环境要求

- Windows(7 及以上;GUI 程序)
- MinGW-w64(GCC 15.x,`D:\ProgramFiles\MinGW`)
- gogui:`gogui.h` + `libgogui.a`(封装 GLFW+ImGui+OpenGL)
- **OpenH264**:`openh264.h`(统一封装头)+ `libopenh264.a`(放入 MinGW include/lib)
- GLFW / OpenGL(MinGW 自带或按 CMakeLists 前缀查找)
- 无需 FFmpeg / Media Foundation / 第三方运行库

> OpenH264 以静态库方式编进 exe,运行时不需要随附 DLL;也不要求目标机装有
> “微软 H.264 编码器”(Win7/精简版系统也 OK)。

## 构建

```bat
build.bat          :: 构建(默认 Release)
build.bat Clean    :: 清理 build 目录
build.bat Rebuild  :: 重新配置并构建
```

产出:`ScreenRecorder.exe`(GUI 子系统、无控制台黑框)。

等价手动命令:

```bat
cmake -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release -B build
cmake --build build
```

> 若 `GOGUI_PREFIX` 与本机 MinGW 路径不同,请改 `CMakeLists.txt` 顶部的
> `set(GOGUI_PREFIX "D:/ProgramFiles/MinGW")`。

## 使用方法

1. 运行 `ScreenRecorder.exe`(窗口置顶、居中)。
2.(可选)设帧率、保存目录(默认桌面 `ScreenRecorder`)。
3. 点「开始录制」→ 3 秒倒计时(可点按钮取消)。
4. 开始录制,窗口自动隐藏;按 **Alt+Z** 或托盘「显示主窗口」后点「停止录制」结束。
5. 完成后提示并自动显示保存路径;视频在 `桌面\ScreenRecorder\recording_….mp4`。

## 录制原理

1. GDI `BitBlt` 抓取主屏为顶向下 32bpp BGRA(自动隐藏本窗口,避免录入自身)。
2. 转换 **BGRA → I420**(YUV420 平面,行距=宽;Y 有限范围,色度 2×2 平均)。
3. **OpenH264** 编码为 H.264 Annex-B(每 1s 一个 IDR;关键帧内带 SPS/PPS)。
4. 自写 **MP4 muxer**:`ftyp + mdat(边录边写) + moov(结束封口)`,
   `avc1/avcC`,样本表 `stts/stss/stsc/stsz/stco`(文件大时自动 `co64`)。
5. 结束写入 moov 并回填 mdat 大小,产出标准可播放 `.mp4`。

## 常见问题

- **提示“无法初始化屏幕抓取设备”**:一般不会出现;请确认在桌面会话运行。
- **录制出来黑屏/空白**:个别环境需以桌面会话运行(远程/服务会话下 GDI 抓屏可能
  拿到空桌面)。
- **想录到别的格式/加音频**:目前只做 H.264+MP4;架构上在 `recorder` 层可扩展。

## 更新日志

- **v2.0(gogui / C++ 重写版)**
  - 由 PyQt5 版 `screen_recorder.py` 重写为 gogui 原生;
  - 编码改 **OpenH264 静态内嵌**(分发零依赖),输出 H.264 `.mp4`;
  - 自写极简 MP4 muxer,不依赖 FFmpeg/Media Foundation;
  - 界面支持倒计时、Alt+Z、系统托盘、帧率/目录设置与日志。
