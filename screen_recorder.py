import sys
import os
import cv2
import pyautogui
import time
import numpy as np
from datetime import datetime
from PyQt5.QtWidgets import (QApplication, QMainWindow, QPushButton, QVBoxLayout, 
                              QWidget, QLabel, QMessageBox, QSystemTrayIcon, QMenu, QAction)
from PyQt5.QtCore import QTimer, Qt, QThread, pyqtSignal, QObject, pyqtSlot, QEvent
from PyQt5.QtGui import QIcon, QPixmap
from pynput import keyboard

class ScreenRecorder(QMainWindow):
    def __init__(self):
        super().__init__()
        self.is_recording = False
        self.recorder_thread = None
        self.timer = None
        self.countdown_seconds = 3
        self.screen_width, self.screen_height = pyautogui.size()
        
        # 设置窗口属性 - 移除系统装饰（最小化、最大化、关闭按钮）
        self.setWindowFlags(Qt.FramelessWindowHint | Qt.WindowStaysOnTopHint)
        self.setWindowTitle('录屏工具')
        self.setGeometry(100, 100, 250, 150)
        
        # 居中显示窗口
        self.center_window()
        
        # 创建系统托盘图标
        self.tray_icon = QSystemTrayIcon(self)
        self.tray_icon.setIcon(QIcon('ico.ico'))
        tray_menu = QMenu()
        
        restore_action = QAction("显示", self)
        quit_action = QAction("退出", self)
        
        restore_action.triggered.connect(self.show_window)
        quit_action.triggered.connect(QApplication.quit)
        
        tray_menu.addAction(restore_action)
        tray_menu.addAction(quit_action)
        self.tray_icon.setContextMenu(tray_menu)
        self.tray_icon.setVisible(True)
        
        # 创建界面
        self.init_ui()
        
        # 初始化键盘监听器
        self.keyboard_listener = None
        self.start_keyboard_listener()
        
    def init_ui(self):
        layout = QVBoxLayout()
        
        # 标题标签
        self.title_label = QLabel('按Alt+Z退出录制')
        self.title_label.setAlignment(Qt.AlignCenter)
        self.title_label.setStyleSheet("font-size: 18px; font-weight: bold;")
        layout.addWidget(self.title_label)
        
        # 状态标签
        # self.status_label = QLabel('点击"开始录制"开始录制')
        # self.status_label.setAlignment(Qt.AlignCenter)
        # layout.addWidget(self.status_label)
        
        # 开始按钮
        self.start_button = QPushButton('开始录制')
        self.start_button.clicked.connect(self.start_recording)
        self.start_button.setStyleSheet("font-size: 16px; padding: 10px;")
        layout.addWidget(self.start_button)
        
        # 停止按钮（初始隐藏）
        self.stop_button = QPushButton('停止录制')
        self.stop_button.clicked.connect(self.stop_recording)
        self.stop_button.setVisible(False)
        self.stop_button.setStyleSheet("font-size: 16px; padding: 10px;")
        layout.addWidget(self.stop_button)
        
        # 倒计时标签
        # self.countdown_label = QLabel('')
        # self.countdown_label.setAlignment(Qt.AlignCenter)
        # layout.addWidget(self.countdown_label)
        
        container = QWidget()
        container.setLayout(layout)
        self.setCentralWidget(container)
        
    def show_window(self):
        """显示主窗口"""
        self.showNormal()
        self.activateWindow()
        
    def center_window(self):
        """将窗口居中显示"""
        # 获取屏幕尺寸
        screen_geometry = QApplication.primaryScreen().geometry()
        screen_width = screen_geometry.width()
        screen_height = screen_geometry.height()
        
        # 获取窗口尺寸
        window_width = self.width()
        window_height = self.height()
        
        # 计算居中位置
        x = (screen_width - window_width) // 2
        y = (screen_height - window_height) // 2
        
        # 设置窗口位置
        self.move(x, y)
        
    def start_recording(self):
        """开始录制前的倒计时"""
        if not self.is_recording:
            # self.status_label.setText('准备开始录制...')
            self.start_button.setVisible(False)
            self.stop_button.setVisible(True)
            
            # 启动倒计时
            self.countdown_timer = QTimer()
            self.countdown_timer.timeout.connect(self.update_countdown)
            self.countdown_timer.start(1000)  # 每秒更新一次
            
            self.countdown_seconds = 3
            self.update_countdown()  # 立即显示倒计时
    
    def update_countdown(self):
        """更新倒计时显示"""
        if self.countdown_seconds > 0:
            self.title_label.setText(f'开始录制将在 {self.countdown_seconds} 秒后开始')
            self.countdown_seconds -= 1
        else:
            # 倒计时结束，开始录制
            self.countdown_timer.stop()
            self.title_label.setText('正在录制...')
            self.start_actual_recording()
    
    def start_actual_recording(self):
        """实际开始录制"""
        try:
            print("开始实际录制...")
            # 获取当前时间作为文件名
            now = datetime.now()
            timestamp = now.strftime("%Y-%m-%d %H-%M")
            
            # 创建保存目录（桌面）
            desktop_path = os.path.join(os.path.expanduser("~"), "Desktop")
            save_folder = os.path.join(desktop_path, "ScreenRecorder")
            if not os.path.exists(save_folder):
                os.makedirs(save_folder)
                
            file_name = f"recording_{timestamp}.avi"
            save_path = os.path.join(save_folder, file_name)
            
            # 创建录制线程并启动
            self.recorder_thread = RecordingThread()
            # 将必要的参数传递给线程
            self.recorder_thread.set_parameters(self.screen_width, self.screen_height, save_path)
            self.recorder_thread.finished.connect(self.on_recording_finished)  # 添加完成信号连接
            self.recorder_thread.start()
            
            self.is_recording = True
            
            # 录制开始后隐藏窗口
            self.hide()
            
        except Exception as e:
            QMessageBox.critical(self, '错误', f'开始录制时出错: {str(e)}')
    
    
    @pyqtSlot()
    def stop_recording_from_key(self):
        """通过键盘快捷键调用停止录制 - 确保在主线程中执行"""
        # 使用 Qt 的信号机制，确保在主线程中安全地调用 GUI 方法
        self.stop_recording()

    def stop_recording(self):
        """停止录制"""
        try:
            print("开始停止录制...")
            # 如果正在倒计时期间，直接取消倒计时并恢复状态
            if not self.is_recording and hasattr(self, 'countdown_timer') and self.countdown_timer.isActive():
                self.countdown_timer.stop()
                self.start_button.setVisible(True)
                self.stop_button.setVisible(False)
                # 恢复标题文本
                self.title_label.setText('按Alt+Z退出录制')
                return
            
            # 如果已经进入实际录制状态，正常处理停止逻辑
            self.is_recording = False
            
            if self.recorder_thread is not None:
                # 停止录制线程
                self.recorder_thread.stop_recording()
                self.recorder_thread.wait()  # 等待线程结束
                
            # 停止倒计时
            if hasattr(self, 'countdown_timer'):
                self.countdown_timer.stop()
                
            self.start_button.setVisible(True)
            self.stop_button.setVisible(False)
            
            # 恢复标题文本
            self.title_label.setText('按Alt+Z退出录制')
            
            QMessageBox.information(self, '完成', '录制已保存到桌面的ScreenRecorder文件夹中')
            
        except Exception as e:
            QMessageBox.critical(self, '错误', f'停止录制时出错: {str(e)}')

    @pyqtSlot()
    def on_recording_finished(self):
        """录制完成后执行"""
        print("录制完成")
        # 确保在主线程中处理
        self.start_button.setVisible(True)
        self.stop_button.setVisible(False)
        self.title_label.setText('按Alt+Z退出录制')

    def start_keyboard_listener(self):
        """启动键盘监听器"""
        try:
            # Simple and robust approach to detect Alt+Z combination
            class KeyboardHandler(QObject):
                key_pressed = pyqtSignal(object)
                
                def __init__(self, parent=None):
                    super().__init__(parent)
                    self.alt_pressed = False
                    
                def on_key_press(self, key):
                    # Check if alt was pressed
                    if key == keyboard.Key.alt or key == keyboard.Key.alt_l or key == keyboard.Key.alt_r:
                        self.alt_pressed = True
                    # Check if z was pressed (either lowercase or uppercase)
                    elif (hasattr(key, 'char') and (key.char == 'z' or key.char == 'Z')) or \
                         (key == keyboard.KeyCode.from_char('z') or key == keyboard.KeyCode.from_char('Z')):
                        if self.alt_pressed:
                            # Emit signal to main thread when Alt+Z is pressed
                            self.key_pressed.emit(key)
                
                def on_key_release(self, key):
                    # Reset alt state when released
                    if key == keyboard.Key.alt or key == keyboard.Key.alt_l or key == keyboard.Key.alt_r:
                        self.alt_pressed = False
            
            handler = KeyboardHandler()
            handler.key_pressed.connect(self.stop_recording_from_key)
            
            keyboard_listener = keyboard.Listener(
                on_press=handler.on_key_press,
                on_release=handler.on_key_release
            )
            
            # Run listener in a daemon thread to avoid hanging when app exits
            import threading
            def start_keyboard_thread():
                try:
                    print("Starting keyboard listener thread...")
                    keyboard_listener.start()
                except Exception as e:
                    print(f"Failed to start keyboard listener: {e}")
                    
            keyboard_thread = threading.Thread(target=start_keyboard_thread, daemon=True)
            keyboard_thread.start()
            
        except Exception as e:
            print(f"启动键盘监听器时出错: {str(e)}")

# 添加录制线程类
class RecordingThread(QThread):
    # 定义信号，用于传输帧数据和完成状态
    frame_ready = pyqtSignal(object)  # 发送每一帧图像数据
    recording_finished = pyqtSignal()  # 录制完成后发出信号
    
    def __init__(self):
        super().__init__()
        self.screen_width = None
        self.screen_height = None
        self.save_path = None
        self.is_recording = True
        
        # 设置视频编码器和参数
        self.fourcc = cv2.VideoWriter_fourcc(*'XVID')
        self.fps = 15.0  # 帧率
        self.video_writer = None
        
    def set_parameters(self, screen_width, screen_height, save_path):
        """设置录制参数"""
        self.screen_width = screen_width
        self.screen_height = screen_height
        self.save_path = save_path
    
    def run(self):
        """线程运行函数"""
        try:
            # 创建VideoWriter对象 - 在线程中创建，但不要使用任何Qt GUI组件
            self.video_writer = cv2.VideoWriter(
                self.save_path, 
                self.fourcc, 
                self.fps, 
                (self.screen_width, self.screen_height)
            )
            
            if not self.video_writer.isOpened():
                print("无法创建视频文件")
                return
                
            frame_count = 0  # 计数器用于调试
            print("开始录制循环...")
            while self.is_recording:
                # 截取屏幕
                screenshot = pyautogui.screenshot()
                frame = cv2.cvtColor(np.array(screenshot), cv2.COLOR_RGB2BGR)
                
                # 写入视频文件 - 在线程中操作，避免Qt对象访问
                if self.video_writer is not None and self.video_writer.isOpened():
                    try:
                        self.video_writer.write(frame)
                        frame_count += 1
                    except Exception as e:
                        print(f"写入帧时出错: {str(e)}")
                        # 继续尝试下一帧，不中断录制
                        continue
                        
                # 控制帧率
                time.sleep(1/15)  # 约15 FPS
                
        except Exception as e:
            print(f'录制过程中出错: {str(e)}')
        finally:
            # 关闭视频流 - 在线程中进行
            if self.video_writer is not None and self.video_writer.isOpened():
                try:
                    self.video_writer.release()
                except Exception as e:
                    print(f"释放视频流时出错: {str(e)}")
            
            # 发送完成信号
            self.recording_finished.emit()
    
    def stop_recording(self):
        """停止录制"""
        self.is_recording = False

def main():
    app = QApplication(sys.argv)
    
    # 设置应用程序图标 - 增加错误处理
    try:
        if os.path.exists('ico.ico'):
            app.setWindowIcon(QIcon('ico.ico'))
    except Exception as e:
        print(f"设置图标时出错: {str(e)}")
        
    recorder = ScreenRecorder()
    recorder.show()
    
    sys.exit(app.exec_())

if __name__ == '__main__':
    main()
