// recorder.h —— ScreenRecord：屏幕录制核心（纯功能，无 UI 依赖）
// 职责：用 GDI 抓取全屏帧，用 OpenH264(H.264)编码并封装为 .mp4。
// 本模块不引用 gogui / ImGui / Qt，可独立测试（对应原 Image2Icon 工程里 generator 的地位）。
#pragma once
#include <atomic>
#include <string>
#include <thread>

namespace rec {

// 录制结束后的统计 / 结果（Finish() 返回）
struct Stats {
    bool        ok = false;
    std::string errorUtf8;          // 失败原因（UTF-8；ok==false 时有效）
    std::string outPathUtf8;        // 输出文件路径（UTF-8）
    int         width = 0, height = 0;
    int         fps = 0;
    int         frames = 0;         // 实际写入的帧数
    double      seconds = 0.0;      // 视频时长（按帧率与帧数换算）
    unsigned long long outBytes = 0; // 输出文件字节数
};

// 全屏屏幕录制器（后台线程抓帧 + 编码）。
// 线程安全；退出前若仍在录制应 RequestStop() 后 Finish()。
class ScreenRecorder {
public:
    ScreenRecorder();
    ~ScreenRecorder();              // 若在录，自动请求停止并等待线程结束

    // 启动录制：后台线程抓全屏并编码到 outPathUtf8（.mp4）。
    // 成功返回 true；失败返回 false 并把原因写入 *err（不启动线程）。
    bool Start(const std::string& outPathUtf8, int fps, std::string* err);

    void RequestStop();             // 线程安全；请求在下一帧边界停止
    bool IsRunning() const;         // 录制线程是否仍在运行
    int  CurrentFrame() const;      // 已写入帧数（轮询用）
    int  ScreenWidth() const;       // Start 成功后有效
    int  ScreenHeight() const;

    // 等待线程结束并取结果（可重复调用；未启动时直接返回空 Stats）。
    Stats Finish();

private:
    void ThreadMain();

    std::thread th_;
    std::atomic<bool> running_{ false };
    std::atomic<bool> stopRequested_{ false };
    std::atomic<int>  frames_{ 0 };
    int screenW_ = 0;
    int screenH_ = 0;
    int fps_ = 15;
    std::string outPathUtf8_;
    Stats stats_;
};

} // namespace rec
