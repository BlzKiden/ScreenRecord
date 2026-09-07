# 前提:gogui.h/libgogui.a 与 openh264.h/libopenh264.a 已放入 MinGW 的 include/lib
cmake_minimum_required(VERSION 3.15)
project(ScreenRecord LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 安装前缀:默认 MinGW(可改成你的编译器/依赖前缀)
set(GOGUI_PREFIX "D:/ProgramFiles/MinGW")

find_package(OpenGL REQUIRED)
find_library(GOGUI_LIB    NAMES gogui       PATHS "${GOGUI_PREFIX}/lib" NO_DEFAULT_PATH)
find_library(GLFW_LIB     NAMES glfw3       PATHS "${GOGUI_PREFIX}/lib" NO_DEFAULT_PATH)
find_library(OPENH264_LIB NAMES openh264    PATHS "${GOGUI_PREFIX}/lib" NO_DEFAULT_PATH)
if(NOT GOGUI_LIB OR NOT GLFW_LIB OR NOT OPENH264_LIB)
    message(FATAL_ERROR "找不到 libgogui.a / libglfw3.a / libopenh264.a，请检查安装目录")
endif()

# WIN32 => GUI 子系统，运行时不弹控制台黑框
# 结构: main.cpp(入口) + main_ui.cpp(界面)
#       recorder.cpp(抓屏) + mp4_mux.cpp(极简 MP4 容器) + OpenH264(编码)
add_executable(ScreenRecorder WIN32
    main.cpp
    main_ui.cpp
    recorder.cpp
    mp4_mux.cpp
)
target_include_directories(ScreenRecorder PRIVATE "${GOGUI_PREFIX}/include")
target_link_libraries(ScreenRecorder PRIVATE
    ${GOGUI_LIB} OpenGL::GL ${GLFW_LIB}
    ${OPENH264_LIB}              # H.264 编码(静态编进 exe, 分发零依赖)
    gdi32                        # 屏幕抓帧(BitBlt / DIB)
    user32                       # 消息/窗口/GetSystemMetrics
    shell32                      # 目录选择、托盘 Shell_NotifyIcon
    ole32                        # CoTaskMemFree 等
)
