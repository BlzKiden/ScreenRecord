# 录屏工具

一款基于Python的屏幕录制软件，具有简洁易用的图形用户界面。

## 功能特性

- **窗口居中显示**：应用程序启动时自动在屏幕中央显示
- **倒计时录制**：开始录制前有3秒倒计时提示
- **系统托盘集成**：支持最小化到系统托盘并从托盘恢复窗口
- **自动保存**：录制的视频文件会自动保存到桌面的ScreenRecorder文件夹中
- **快捷键退出**：录制过程中可通过Alt+Z组合键退出录制

## 系统要求

- Python 3.6+
- Windows系统（支持pyautogui和cv2库）
- PyQt5库

## 安装依赖

```bash
pip install opencv-python pyautogui PyQt5 numpy Pillow
```

## 使用方法

1. 运行程序：
   ```bash
   python screen_recorder.py
   ```

2. 点击"开始录制"按钮，将显示3秒倒计时

3. 倒计时结束后开始录制屏幕

4. 录制过程中可以点击"停止录制"按钮结束录制

5. 或者使用快捷键 Alt+Z 退出录制

## 注意事项

- 录制的视频文件保存在桌面的ScreenRecorder文件夹中
- 程序会自动创建必要的目录结构
- 在倒计时期间可以点击"停止录制"取消录制计划
- 应用程序支持最小化到系统托盘，双击托盘图标可恢复窗口

## 项目结构

```
.
├── screen_recorder.py    # 主程序文件
├── ico.ico               # 程序图标文件
└── README.md             # 说明文档
```

## 开发环境

- Python 3.6+
- PyQt5
- OpenCV (cv2)
- pyautogui
- numpy
- Pillow

## 许可证
  GPL-3.0 license

本项目仅供学习和参考使用。