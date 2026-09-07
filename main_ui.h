// main_ui.h —— ScreenRecord 界面模块（基于 gogui）
// 只负责呈现/交互（控制面板、倒计时、托盘、Alt+Z 全局热键），
// 具体录制/编码逻辑全部委托给 recorder.h。
#pragma once
#include <string>
#include "recorder.h"

namespace ui {

// 应用阶段
enum class Phase { Idle, Countdown, Recording };

// 界面状态（由 main.cpp 持有，每帧传给 BuildUI）
struct State {
    int  fpsIndex    = 2;         // 0..4 -> 5/10/15/20/30 FPS
    bool useDesktopDefault = true; // 保存到 桌面\ScreenRecorder（默认）
    std::string saveDir;          // UTF-8；useDesktopDefault==false 时生效

    Phase phase         = Phase::Idle;
    int   countdownLeft = 3;      // 倒计时剩余秒数
    double nextTick     = 0.0;    // 下一整秒时刻（GoGui::GetTime）

    rec::ScreenRecorder recorder; // 录制核心（内部后台线程）
    bool  stopRequested = false;  // 已请求停止，等待线程结束取结果

    std::string lastOutPath;      // 最近一次输出（UTF-8）
    std::string lastError;        // 最近一次错误（UTF-8）

    std::string log;              // 日志原文（UTF-8）
};

// 窗口标题（UTF-8），main.cpp 建窗与主界面定位共用
const char* WindowTitle();

void Init(State* s);
void Shutdown(State* s);
void BuildUI(State* s);      // 每帧调用一次

} // namespace ui
