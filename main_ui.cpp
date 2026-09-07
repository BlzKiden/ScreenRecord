// main_ui.cpp —— ScreenRecord 界面实现（基于 gogui）
// 控制面板(开始/停止 + 3 秒倒计时) + 设置(帧率/保存目录) + 日志；
// 附 Win32 集成：系统托盘、Alt+Z 全局热键(后台钩子线程)、保存目录浏览。
// 录制逻辑全部委托给 recorder.h（内部后台线程抓帧/编码，本模块不碰）。
#include "main_ui.h"
#include "recorder.h"

#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "gogui.h"      // 先于 windows.h：避免 CreateWindow 宏污染 gogui.h
#include <windows.h>
#include <shellapi.h>   // 系统托盘: NOTIFYICONDATA / Shell_NotifyIcon
#include <shlobj.h>     // 目录选择: SHBrowseForFolderW / SHGetFolderPathW

#ifdef CreateWindow
#undef CreateWindow
#endif

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

enum { kPathBuf = 4096, kMaxLog = 1 << 18, kTrimLog = 1 << 16 };

const int   kFps[5]   = { 5, 10, 15, 20, 30 };
const char* kFpsNames[5] = { "5", "10", "15", "20", "30" };

// ==========================================================================
// UTF-8 <-> UTF-16
// ==========================================================================
std::wstring Utf8ToWide(const std::string& u) {
    if (u.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, u.c_str(), (int)u.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, u.c_str(), (int)u.size(), &w[0], n);
    return w;
}
std::string WideToUtf8(const std::wstring& w) {
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string u(n, '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &u[0], n, nullptr, nullptr);
    return u;
}

// ==========================================================================
// 路径 / 目录 / 文件名
// ==========================================================================
std::string JoinPath(const std::string& dir, const std::string& name) {
    if (dir.empty()) return name;
    char last = dir[dir.size() - 1];
    if (last == '/' || last == '\\') return dir + name;
    return dir + "\\" + name;
}

// 桌面\ScreenRecorder
std::string DesktopDefaultDir() {
    wchar_t buf[MAX_PATH];
    if (SHGetFolderPathW(nullptr, CSIDL_DESKTOPDIRECTORY, nullptr, SHGFP_TYPE_CURRENT, buf) == S_OK)
        return WideToUtf8(std::wstring(buf)) + "\\ScreenRecorder";
    return "C:\\ScreenRecorder";
}

std::string EffectiveDir(const ui::State* s) {
    if (s->useDesktopDefault) return DesktopDefaultDir();
    if (!s->saveDir.empty()) return s->saveDir;
    return DesktopDefaultDir();
}

bool FileExistsWideUtf8(const std::string& p) {
    std::wstring w = Utf8ToWide(p);
    return GetFileAttributesW(w.c_str()) != INVALID_FILE_ATTRIBUTES;
}

// 逐级创建目录(含最后一段);成功返回 true。
bool EnsureDirs(const std::wstring& path) {
    if (path.empty()) return true;
    std::wstring p = path;
    for (size_t i = 0; i < p.size(); ++i) if (p[i] == L'/') p[i] = L'\\';
    while (!p.empty() && (p.back() == L'\\')) p.pop_back();   // 去掉尾分隔符
    if (p.empty()) return true;
    size_t start = 0;
    if (p.size() >= 2 && p[1] == L':') start = 3;     // 跳过盘符 "C:\"
    // 逐级创建中间目录
    size_t pos = start;
    while ((pos = p.find(L'\\', pos)) != std::wstring::npos) {
        std::wstring sub = p.substr(0, pos);
        if (!sub.empty() && GetFileAttributesW(sub.c_str()) == INVALID_FILE_ATTRIBUTES) {
            if (!CreateDirectoryW(sub.c_str(), nullptr)) {
                if (GetLastError() != ERROR_ALREADY_EXISTS) return false;
            }
        }
        ++pos;
    }
    // 创建最后一段(如 ...\Desktop\ScreenRecorder)
    if (GetFileAttributesW(p.c_str()) == INVALID_FILE_ATTRIBUTES) {
        if (!CreateDirectoryW(p.c_str(), nullptr)) {
            if (GetLastError() != ERROR_ALREADY_EXISTS) return false;
        }
    }
    return true;
}

// recording_YYYY-MM-DD HH-MM-SS.mp4（重名自动加 (n)）
std::string MakeOutputPath(const std::string& dir) {
    SYSTEMTIME st;
    GetLocalTime(&st);
    char base[128];
    std::snprintf(base, sizeof(base), "recording_%04d-%02d-%02d %02d-%02d-%02d",
                  (int)st.wYear, (int)st.wMonth, (int)st.wDay,
                  (int)st.wHour, (int)st.wMinute, (int)st.wSecond);
    for (int i = 0;; ++i) {
        char name[176];
        if (i == 0) std::snprintf(name, sizeof(name), "%s.mp4", base);
        else        std::snprintf(name, sizeof(name), "%s (%d).mp4", base, i);
        std::string p = JoinPath(dir, name);
        if (!FileExistsWideUtf8(p)) return p;
    }
}

// 原生“选择目录”对话框；成功返回 true 并把 UTF-8 路径写入 out。
bool NativeFolder(std::string* out) {
    BROWSEINFOW bi;
    std::memset(&bi, 0, sizeof(bi));
    bi.lpszTitle = L"选择录制视频保存目录";
    bi.ulFlags   = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE | BIF_USENEWUI;
    LPITEMIDLIST pidl = SHBrowseForFolderW(&bi);
    if (!pidl) return false;
    wchar_t path[MAX_PATH];
    bool ok = SHGetPathFromIDListW(pidl, path) != FALSE;
    CoTaskMemFree(pidl);
    if (!ok) return false;
    *out = WideToUtf8(path);
    return true;
}

// ==========================================================================
// 日志
// ==========================================================================
void Log(ui::State* s, const char* fmt, ...) {
    char tmp[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    s->log += tmp;
    s->log += '\n';
    if (s->log.size() > kMaxLog)
        s->log = s->log.substr(s->log.size() - kTrimLog);
}

// ==========================================================================
// Win32 集成：系统托盘 + Alt+Z 全局热键（独立后台消息线程）
//  ------------------------------------------------------------------------
// 说明：托盘图标与低级键盘钩子都放到一个独立线程里跑自己的 GetMessage 循环，
//      不依赖 GLFW 的消息泵；与 UI 线程之间只通过几个 atomic 标志通信。
// ==========================================================================
const UINT kTrayMsg = WM_APP + 1;

struct Shell {
    HWND    hwnd = nullptr;
    HHOOK   hook = nullptr;
    DWORD   threadId = 0;
    HANDLE  thread = nullptr;
    std::atomic<bool> altZ{ false };
    std::atomic<bool> reqShow{ false };
    std::atomic<bool> reqQuit{ false };
};
Shell* g_shell = nullptr;

Shell* ShellOf(HWND h) { return reinterpret_cast<Shell*>(GetWindowLongPtrW(h, GWLP_USERDATA)); }

LRESULT CALLBACK TrayWndProc(HWND h, UINT msg, WPARAM wParam, LPARAM lParam) {
    Shell* sh = ShellOf(h);
    if (msg == kTrayMsg) {
        if (lParam == WM_LBUTTONDBLCLK || lParam == WM_LBUTTONUP) {
            if (sh) sh->reqShow.store(true);
            return 0;
        }
        if (lParam == WM_RBUTTONUP) {
            HMENU menu = CreatePopupMenu();
            AppendMenuW(menu, MF_STRING, 1, L"显示主窗口");
            AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(menu, MF_STRING, 2, L"退出");
            SetForegroundWindow(h);
            POINT pt;
            GetCursorPos(&pt);
            int cmd = (int)TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON,
                                          pt.x, pt.y, 0, h, nullptr);
            DestroyMenu(menu);
            if (sh) {
                if (cmd == 1) sh->reqShow.store(true);
                else if (cmd == 2) sh->reqQuit.store(true);
            }
            return 0;
        }
        return 0;
    }
    return DefWindowProcW(h, msg, wParam, lParam);
}

LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION) {
        if (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN) {
            KBDLLHOOKSTRUCT* ks = reinterpret_cast<KBDLLHOOKSTRUCT*>(lParam);
            if (ks->vkCode == 'Z' && (ks->flags & LLKHF_ALTDOWN)) {
                if (g_shell) g_shell->altZ.store(true);
            }
        }
    }
    return CallNextHookEx(nullptr, nCode, wParam, lParam);
}

DWORD WINAPI ShellThreadProc(LPVOID param) {
    Shell* sh = static_cast<Shell*>(param);
    HINSTANCE hInst = GetModuleHandleW(nullptr);

    WNDCLASSEXW wc;
    std::memset(&wc, 0, sizeof(wc));
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = TrayWndProc;
    wc.hInstance     = hInst;
    wc.lpszClassName = L"ScreenRecord_TrayWnd";
    RegisterClassExW(&wc);

    sh->hwnd = CreateWindowExW(0, L"ScreenRecord_TrayWnd", L"", 0,
                               0, 0, 0, 0, HWND_MESSAGE, nullptr, hInst, nullptr);
    if (sh->hwnd) SetWindowLongPtrW(sh->hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(sh));

    NOTIFYICONDATAW nid;
    std::memset(&nid, 0, sizeof(nid));
    nid.cbSize = sizeof(nid);
    nid.hWnd   = sh->hwnd;
    nid.uID    = 1;
    nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    nid.uCallbackMessage = kTrayMsg;
    nid.hIcon  = LoadIconW(nullptr, (LPCWSTR)(ULONG_PTR)IDI_APPLICATION);
    wcsncpy(nid.szTip, L"录屏工具 ScreenRecord（录制中按 Alt+Z 停止）", 127);
    Shell_NotifyIconW(NIM_ADD, &nid);

    sh->hook = SetWindowsHookExW(WH_KEYBOARD_LL, LowLevelKeyboardProc, hInst, 0);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (sh->hook) { UnhookWindowsHookEx(sh->hook); sh->hook = nullptr; }
    Shell_NotifyIconW(NIM_DELETE, &nid);
    if (sh->hwnd) { DestroyWindow(sh->hwnd); sh->hwnd = nullptr; }
    return 0;
}

void ShellStart() {
    if (g_shell) return;
    Shell* sh = new Shell();
    g_shell = sh;
    sh->thread = CreateThread(nullptr, 0, ShellThreadProc, sh, 0, &sh->threadId);
    if (!sh->thread) { g_shell = nullptr; delete sh; }
}

void ShellStop() {
    if (!g_shell) return;
    Shell* sh = g_shell;
    g_shell = nullptr;
    if (sh->thread) {
        PostThreadMessageW(sh->threadId, WM_QUIT, 0, 0);
        WaitForSingleObject(sh->thread, 3000);
        CloseHandle(sh->thread);
    }
    // 进程即将退出，故意不 delete，避免线程仍在回调时悬垂访问
}

// ==========================================================================
// 主窗口显示 / 隐藏 / 布局
// ==========================================================================
void ShowAppWindow() {
    GoGui::ShowWindow();
    GoGui::FocusWindow();
    std::wstring title = Utf8ToWide(ui::WindowTitle());
    HWND hw = ::FindWindowW(nullptr, title.c_str());
    if (hw) {
        SetWindowPos(hw, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
        SetForegroundWindow(hw);
    }
}

void HideAppWindow() {
    GoGui::HideWindow();
}

// 首帧(窗口仍隐藏)一次性：固定尺寸 -> 居中 -> 置顶。
void LayoutWindow(float contentH) {
    static bool done = false;
    if (done) return;
    done = true;

    std::wstring title = Utf8ToWide(ui::WindowTitle());
    HWND hw = ::FindWindowW(nullptr, title.c_str());
    if (hw) {
        LONG_PTR st = GetWindowLongPtrW(hw, GWL_STYLE);
        LONG_PTR ns = st & ~((LONG_PTR)WS_MAXIMIZEBOX | (LONG_PTR)WS_THICKFRAME);
        if (ns != st) {
            SetWindowLongPtrW(hw, GWL_STYLE, ns);
            SetWindowPos(hw, nullptr, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
        }
    }

    const int kWinW = 560;
    int need = (int)(contentH + 24.0f);
    if (need < 460) need = 460;
    GoGui::SetWindowSize(kWinW, need);

    int w = 0, h = 0;
    GoGui::GetWindowSize(&w, &h);
    int mx = 0, my = 0, mw = 0, mh = 0;
    if (GoGui::GetMonitorWorkArea(GoGui::GetPrimaryMonitorIndex(), &mx, &my, &mw, &mh)) {
        int px = mx + (mw - w) / 2;
        int py = my + (mh - h) / 2;
        if (px < mx) px = mx;
        if (py < my) py = my;
        GoGui::SetWindowPos(px, py);
    }
    if (hw) SetWindowPos(hw, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
}

// ==========================================================================
// 状态机动作
// ==========================================================================
void BeginCountdown(ui::State* s) {
    s->phase = ui::Phase::Countdown;
    s->countdownLeft = 3;
    s->nextTick = GoGui::GetTime() + 1.0;
    Log(s, "[倒计时] 3 秒后开始全屏录制…（点“停止”或按 Alt+Z 可取消）");
}

void CancelCountdown(ui::State* s) {
    s->phase = ui::Phase::Idle;
    s->countdownLeft = 3;
    Log(s, "[取消] 已取消本次录制。");
}

void BeginRecording(ui::State* s) {
    int fps = kFps[s->fpsIndex < 0 ? 0 : (s->fpsIndex > 4 ? 4 : s->fpsIndex)];
    std::string dir = EffectiveDir(s);
    if (!EnsureDirs(Utf8ToWide(dir))) {
        Log(s, "[开始] 无法创建保存目录：%s", dir.c_str());
        s->lastError = "无法创建保存目录：" + dir;
        s->phase = ui::Phase::Idle;
        return;
    }
    std::string path = MakeOutputPath(dir);

    HideAppWindow();                       // 先隐藏自身，避免把自己录进画面
    std::string err;
    if (!s->recorder.Start(path, fps, &err)) {
        Log(s, "[开始] 启动录制失败：%s", err.c_str());
        s->lastError = err;
        ShowAppWindow();
        s->phase = ui::Phase::Idle;
        return;
    }
    s->phase = ui::Phase::Recording;
    s->stopRequested = false;
    s->lastOutPath = path;
    s->lastError.clear();
    Log(s, "[录制] 开始：%dx%d @%d FPS -> %s",
        s->recorder.ScreenWidth(), s->recorder.ScreenHeight(), fps, path.c_str());
}

void RequestStopUI(ui::State* s) {
    if (s->recorder.IsRunning()) {
        s->recorder.RequestStop();
        s->stopRequested = true;
        Log(s, "[停止] 正在停止录制并封口…");
    }
}

void OnRecordingEnd(ui::State* s, const rec::Stats& st) {
    s->phase = ui::Phase::Idle;
    s->stopRequested = false;
    ShowAppWindow();
    if (st.ok) {
        char line[192];
        std::snprintf(line, sizeof(line), "[完成] %d 帧 · %.1f 秒 · %.2f MB",
                      st.frames, st.seconds, st.outBytes / 1048576.0);
        Log(s, "%s", line);
        Log(s, "输出: %s", st.outPathUtf8.c_str());
        s->lastOutPath = st.outPathUtf8;
        s->lastError.clear();
        std::wstring wmsg = L"录制完成！\n\n" + Utf8ToWide(st.outPathUtf8) +
                            L"\n\n（已写入 " + std::to_wstring(st.frames) + L" 帧）";
        MessageBoxW(nullptr, wmsg.c_str(), L"录屏工具", MB_OK | MB_ICONINFORMATION | MB_TOPMOST);
    } else {
        Log(s, "[失败] %s", st.errorUtf8.c_str());
        s->lastError = st.errorUtf8;
        std::wstring wmsg = L"录制失败：\n" + Utf8ToWide(st.errorUtf8);
        MessageBoxW(nullptr, wmsg.c_str(), L"录屏工具", MB_OK | MB_ICONERROR | MB_TOPMOST);
    }
}

void ShellPoll(ui::State* s) {
    if (!g_shell) return;
    if (g_shell->altZ.exchange(false)) {
        if (s->phase == ui::Phase::Countdown) CancelCountdown(s);
        else if (s->phase == ui::Phase::Recording) RequestStopUI(s);
    }
    if (g_shell->reqShow.exchange(false)) ShowAppWindow();
    if (g_shell->reqQuit.exchange(false)) GoGui::SetWindowShouldClose(true);
}

// 每帧推进状态机
void Tick(ui::State* s) {
    ShellPoll(s);

    if (s->phase == ui::Phase::Recording) {
        // 线程结束（正常停止或出错）即收尾
        if (!s->recorder.IsRunning()) {
            rec::Stats st = s->recorder.Finish();
            OnRecordingEnd(s, st);
        }
    } else if (s->phase == ui::Phase::Countdown) {
        if (GoGui::GetTime() >= s->nextTick) {
            --s->countdownLeft;
            s->nextTick = GoGui::GetTime() + 1.0;
            if (s->countdownLeft <= 0) BeginRecording(s);
        }
    }
}

} // namespace

namespace ui {

const char* WindowTitle() { return "录屏工具 ScreenRecord"; }

void Init(State* s) {
    s->fpsIndex         = 2;       // 15 FPS
    s->useDesktopDefault = true;
    s->saveDir.clear();
    s->phase          = Phase::Idle;
    s->countdownLeft  = 3;
    s->nextTick       = 0.0;
    s->stopRequested  = false;
    s->lastOutPath.clear();
    s->lastError.clear();
    s->log.clear();

    ShellStart();
    int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
    Log(s, "就绪：屏幕 %dx%d · 默认 %d FPS · 保存到 %s",
        sw, sh, kFps[s->fpsIndex], EffectiveDir(s).c_str());
    Log(s, "提示：录制中窗口自动隐藏；按 Alt+Z 停止；托盘图标可显示/退出。");
}

void Shutdown(State* s) {
    if (s && s->recorder.IsRunning()) {
        s->recorder.RequestStop();
        s->recorder.Finish();          // 等待封口
    }
    ShellStop();
    if (s) {
        s->lastOutPath.clear();
        s->lastError.clear();
        s->log.clear();
    }
}

void BuildUI(State* s) {
    Tick(s);

    int vw = 0, vh = 0;
    GoGui::GetWindowSize(&vw, &vh);
    if (vw < 200) vw = 560;
    if (vh < 200) vh = 600;
    GoGui::SetNextWindowPos(GoGui::Vec2(0.0f, 0.0f), GoGui::Cond_Always);
    GoGui::SetNextWindowSize(GoGui::Vec2((float)vw, (float)vh), GoGui::Cond_Always);
    GoGui::Begin("##screenrecord-main", nullptr,
                 GoGui::Wnd_NoTitleBar | GoGui::Wnd_NoResize | GoGui::Wnd_NoMove |
                 GoGui::Wnd_NoCollapse | GoGui::Wnd_NoSavedSettings | GoGui::Wnd_NoScrollbar);
    float yStart = GoGui::GetCursorPosY();
    const float sp = 8.0f;

    const bool idle = (s->phase == Phase::Idle);

    // ---------------- 1. 状态大标题 ----------------
    int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
    int fps = kFps[s->fpsIndex < 0 ? 0 : (s->fpsIndex > 4 ? 4 : s->fpsIndex)];
    char big[160];
    GoGui::Color bigCol = GoGui::ColorRGBA(52, 120, 220);
    char sub[256];
    sub[0] = 0;
    if (s->phase == Phase::Idle) {
        std::snprintf(big, sizeof(big), "●  全屏录屏 · 就绪");
        bigCol = GoGui::ColorRGBA(52, 120, 220);
        std::snprintf(sub, sizeof(sub), "屏幕 %dx%d · %d FPS · 保存：%s",
                      sw, sh, fps, EffectiveDir(s).c_str());
    } else if (s->phase == Phase::Countdown) {
        std::snprintf(big, sizeof(big), "开始录制将在 %d 秒后开始…", s->countdownLeft);
        bigCol = GoGui::ColorRGBA(230, 150, 60);
        std::snprintf(sub, sizeof(sub), "点「停止」或按 Alt+Z 可取消");
    } else {
        int sec = s->recorder.CurrentFrame() / (fps > 0 ? fps : 1);
        std::snprintf(big, sizeof(big), "●  正在录制  %02d:%02d", sec / 60, sec % 60);
        bigCol = GoGui::ColorRGBA(210, 70, 70);
        std::snprintf(sub, sizeof(sub), "%dx%d · %d FPS · 已写 %d 帧 · Alt+Z 停止",
                      s->recorder.ScreenWidth() ? s->recorder.ScreenWidth() : sw,
                      s->recorder.ScreenHeight() ? s->recorder.ScreenHeight() : sh,
                      fps, s->recorder.CurrentFrame());
    }
    GoGui::PushStyleColorRGBA(GoGui::Col_Text, bigCol);
    GoGui::TextWrapped("%s", big);
    GoGui::PopStyleColor();
    if (sub[0]) GoGui::TextDisabled("%s", sub);

    // ---------------- 2. 主按钮 ----------------
    GoGui::Spacing();
    const float btnH = 46.0f;
    GoGui::Color btnCol, btnHov, btnAct;
    const char* label = "●  开始录制";
    if (s->phase == Phase::Idle) {
        btnCol = GoGui::ColorRGBA(52, 158, 255);
        btnHov = GoGui::ColorRGBA(82, 178, 255);
        btnAct = GoGui::ColorRGBA(42, 138, 235);
    } else if (s->phase == Phase::Countdown) {
        label = "■  取消倒计时";
        btnCol = GoGui::ColorRGBA(220, 140, 50);
        btnHov = GoGui::ColorRGBA(240, 160, 60);
        btnAct = GoGui::ColorRGBA(200, 120, 40);
    } else {
        label = "■  停止录制";
        btnCol = GoGui::ColorRGBA(210, 70, 70);
        btnHov = GoGui::ColorRGBA(235, 95, 95);
        btnAct = GoGui::ColorRGBA(185, 55, 55);
    }
    GoGui::PushStyleColorRGBA(GoGui::Col_Button, btnCol);
    GoGui::PushStyleColorRGBA(GoGui::Col_ButtonHovered, btnHov);
    GoGui::PushStyleColorRGBA(GoGui::Col_ButtonActive, btnAct);
    if (GoGui::Button(label, GoGui::Vec2(-1.0f, btnH))) {
        if (s->phase == Phase::Idle)      BeginCountdown(s);
        else if (s->phase == Phase::Countdown) CancelCountdown(s);
        else                              RequestStopUI(s);
    }
    GoGui::PopStyleColor(3);
    if (s->phase == Phase::Recording)
        GoGui::TextDisabled("录制时本窗口已隐藏，不会进入画面。");
    else
        GoGui::TextDisabled("点击后先显示 3 秒倒计时，随后自动开始全屏录制。");

    // ---------------- 3. 设置 ----------------
    GoGui::Spacing();
    GoGui::SeparatorText("设置");
    GoGui::BeginDisabled(!idle);

    GoGui::AlignTextToFramePadding();
    GoGui::Text("帧率:");
    GoGui::SameLine(0.0f, sp);
    GoGui::SetNextItemWidth(96.0f);
    GoGui::Combo("##fps", &s->fpsIndex, kFpsNames, 5);
    GoGui::SameLine(0.0f, 18.0f);
    GoGui::TextDisabled("帧/秒（越高越流畅、文件越大）");

    GoGui::AlignTextToFramePadding();
    GoGui::Checkbox("保存到桌面 ScreenRecorder 文件夹", &s->useDesktopDefault);
    if (GoGui::IsItemHovered())
        GoGui::SetTooltip("取消勾选后可指定其它保存目录");
    if (!s->useDesktopDefault) {
        char obuf[kPathBuf];
        std::memset(obuf, 0, sizeof(obuf));
        std::string dir = s->saveDir.empty() ? EffectiveDir(s) : s->saveDir;
        std::strncpy(obuf, dir.c_str(), kPathBuf - 1);
        GoGui::SetNextItemWidth(GoGui::GetContentRegionAvail().x - 118.0f - sp);
        GoGui::InputText("##savedir", obuf, kPathBuf, GoGui::Itxt_ReadOnly);
        GoGui::SameLine(0.0f, sp);
        if (GoGui::Button("浏览…##dir", GoGui::Vec2(110.0f, 0.0f))) {
            std::string p;
            if (NativeFolder(&p) && !p.empty()) {
                s->saveDir = p;
                Log(s, "[设置] 保存目录：%s", p.c_str());
            }
        }
    }
    GoGui::EndDisabled();

    // ---------------- 4. 最近结果 ----------------
    GoGui::Spacing();
    GoGui::SeparatorText("最近结果");
    if (!s->lastError.empty()) {
        GoGui::PushStyleColorRGBA(GoGui::Col_Text, GoGui::ColorRGBA(200, 60, 60));
        GoGui::TextWrapped("出错：%s", s->lastError.c_str());
        GoGui::PopStyleColor();
    } else if (!s->lastOutPath.empty()) {
        GoGui::PushStyleColorRGBA(GoGui::Col_Text, GoGui::ColorRGBA(40, 150, 70));
        GoGui::TextWrapped("已保存：%s", s->lastOutPath.c_str());
        GoGui::PopStyleColor();
    } else {
        GoGui::TextDisabled("（尚未录制过）");
    }

    // ---------------- 5. 日志 ----------------
    GoGui::Spacing();
    GoGui::SeparatorText("日志");
    const float logH = GoGui::GetTextLineHeightWithSpacing() * 7.0f + 14.0f;
    GoGui::OutputTextMultiline("##srlog", s->log.c_str(), s->log.size() + 1,
                               GoGui::Vec2(-1.0f, logH), GoGui::Itxt_Wrap);

    float yEnd = GoGui::GetCursorPosY();
    GoGui::End();
    LayoutWindow(yEnd - yStart);
}

} // namespace ui
