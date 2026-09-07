// main.cpp —— ScreenRecord（gogui/C++ 录屏工具）入口
// 只负责：初始化 gogui，创建窗口，主循环每帧调用界面模块。
// 界面在 main_ui.h/cpp，录制核心在 recorder.h/cpp。
#include "gogui.h"
#include "main_ui.h"

#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

// windows.h 把 CreateWindow 展开成宏，会破坏 GoGui::CreateWindow，故解除
#ifdef CreateWindow
#undef CreateWindow
#endif

static void RemoveIniFile() { ::DeleteFileW(L"imgui.ini"); }

int main() {
    RemoveIniFile();                              // 不保留旧配置
    if (!GoGui::Init()) return 1;
    if (!GoGui::CreateWindow(560, 600, ui::WindowTitle())) return 1;
    GoGui::CreateContext();
    GoGui::UseLightTheme();

    ui::State state;
    ui::Init(&state);

    while (!GoGui::WindowShouldClose()) {
        GoGui::BeginFrame();
        GoGui::PushFont(0);                       // 内置字体（含中文）
        ui::BuildUI(&state);
        GoGui::PopFont();
        GoGui::EndFrame();

        // 窗口隐藏（录制中）时放慢主循环，避免白耗 CPU；后台录制线程不受影响。
        if (!GoGui::IsWindowVisible() && GoGui::GetFrameCount() > 1)
            GoGui::WaitEventsTimeout(0.05);
    }

    ui::Shutdown(&state);                         // 若在录：停录并等待封口
    GoGui::DestroyContext();
    GoGui::Shutdown();
    RemoveIniFile();
    return 0;
}
