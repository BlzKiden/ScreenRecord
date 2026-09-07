// recorder.cpp —— ScreenRecord 录制核心实现
// 抓帧:  GDI BitBlt 抓取主屏 → 顶向下 BGRA DIB → 转换 I420
// 编码:  OpenH264::Encoder (静态链接 libopenh264.a, 不依赖系统/目标机编码器)
// 封装:  Mp4Mux 自写极简 MP4 容器(avc1/avcC)
// 全部在内部后台线程执行, 不触碰任何 UI / gogui 对象。
#include "recorder.h"
#include "mp4_mux.h"
#include "openh264.h"

#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <chrono>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace rec {

// ==========================================================================
// UTF-8 <-> UTF-16
// ==========================================================================
static std::wstring Utf8ToWide(const std::string& u) {
    if (u.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, u.c_str(), (int)u.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, u.c_str(), (int)u.size(), &w[0], n);
    return w;
}

// 确保输出文件的父目录存在(逐级创建, 含最后一段)。
static bool EnsureDirOfFile(const std::wstring& filePath) {
    std::wstring p = filePath;
    for (size_t i = 0; i < p.size(); ++i) if (p[i] == L'/') p[i] = L'\\';
    size_t slash = p.find_last_of(L'\\');
    if (slash == std::wstring::npos) return true;
    std::wstring dir = p.substr(0, slash);
    while (!dir.empty() && dir.back() == L'\\') dir.pop_back();
    if (dir.empty()) return true;

    size_t start = 0;
    if (dir.size() >= 2 && dir[1] == L':') start = 3;
    size_t pos = start;
    while ((pos = dir.find(L'\\', pos)) != std::wstring::npos) {
        std::wstring sub = dir.substr(0, pos);
        if (!sub.empty() && GetFileAttributesW(sub.c_str()) == INVALID_FILE_ATTRIBUTES)
            CreateDirectoryW(sub.c_str(), nullptr);
        ++pos;
    }
    if (GetFileAttributesW(dir.c_str()) == INVALID_FILE_ATTRIBUTES)
        CreateDirectoryW(dir.c_str(), nullptr);
    return GetFileAttributesW(dir.c_str()) != INVALID_FILE_ATTRIBUTES;
}

// ==========================================================================
// 构造 / 析构
// ==========================================================================
ScreenRecorder::ScreenRecorder() {}
ScreenRecorder::~ScreenRecorder() {
    if (running_.load()) {
        stopRequested_ = true;
        if (th_.joinable()) th_.join();
        running_.store(false);
    }
}

bool ScreenRecorder::Start(const std::string& outPathUtf8, int fps, std::string* err) {
    if (err) err->clear();
    if (running_.load()) {
        if (err) *err = "已在录制中，请先停止。";
        return false;
    }
    if (outPathUtf8.empty()) {
        if (err) *err = "输出路径为空。";
        return false;
    }
    if (fps < 1) fps = 1;
    if (fps > 60) fps = 60;

    screenW_ = GetSystemMetrics(SM_CXSCREEN);
    screenH_ = GetSystemMetrics(SM_CYSCREEN);
    if (screenW_ < 2 || screenH_ < 2) {
        if (err) *err = "无法获取屏幕尺寸。";
        return false;
    }

    outPathUtf8_ = outPathUtf8;
    fps_ = fps;
    stopRequested_ = false;
    frames_.store(0);
    stats_ = Stats();
    stats_.outPathUtf8 = outPathUtf8_;

    running_.store(true);
    try {
        th_ = std::thread(&ScreenRecorder::ThreadMain, this);
    } catch (...) {
        running_.store(false);
        if (err) *err = "无法创建录制线程。";
        return false;
    }
    return true;
}

void ScreenRecorder::RequestStop() { stopRequested_ = true; }
bool ScreenRecorder::IsRunning() const { return running_.load(); }
int  ScreenRecorder::CurrentFrame() const { return frames_.load(); }
int  ScreenRecorder::ScreenWidth() const { return screenW_; }
int  ScreenRecorder::ScreenHeight() const { return screenH_; }

Stats ScreenRecorder::Finish() {
    if (running_.load()) {
        stopRequested_ = true;
        if (th_.joinable()) th_.join();
        running_.store(false);
    } else if (th_.joinable()) {
        th_.join();
    }
    return stats_;
}

// ==========================================================================
// BGRA(顶向下, 每像素 4 字节) -> I420(YUV420 平面, 行距 == 宽, 连续排布)
//  OpenH264 输入要求: I420, stride == width, 长度 width*height*3/2。
// ==========================================================================
static void BgraToI420(const BYTE* bgra, int w, int h, BYTE* dst) {
    const size_t ySize = (size_t)w * h;
    const size_t uSize = (size_t)(w / 2) * (h / 2);
    BYTE* Y = dst;
    BYTE* U = dst + ySize;
    BYTE* V = dst + ySize + uSize;

    // Y 平面(有限范围 BT.601)
    for (int y = 0; y < h; ++y) {
        const BYTE* s = bgra + (size_t)y * w * 4;
        BYTE* d = Y + (size_t)y * w;
        for (int x = 0; x < w; ++x) {
            int b = s[x * 4 + 0], g = s[x * 4 + 1], r = s[x * 4 + 2];
            int Yv = ((66 * r + 129 * g + 25 * b + 128) >> 8) + 16;
            if (Yv < 16) Yv = 16; else if (Yv > 235) Yv = 235;
            d[x] = (BYTE)Yv;
        }
    }
    // U/V 平面(每个 2x2 块取四像素平均)
    const int cw = w / 2, ch = h / 2;
    for (int by = 0; by < ch; ++by) {
        const BYTE* s0 = bgra + (size_t)(2 * by) * w * 4;
        const BYTE* s1 = bgra + (size_t)(2 * by + 1) * w * 4;
        BYTE* du = U + (size_t)by * cw;
        BYTE* dv = V + (size_t)by * cw;
        for (int bx = 0; bx < cw; ++bx) {
            int x0 = 2 * bx, x1 = 2 * bx + 1;
            int b = (s0[x0*4+0] + s0[x1*4+0] + s1[x0*4+0] + s1[x1*4+0]);
            int g = (s0[x0*4+1] + s0[x1*4+1] + s1[x0*4+1] + s1[x1*4+1]);
            int r = (s0[x0*4+2] + s0[x1*4+2] + s1[x0*4+2] + s1[x1*4+2]);
            b >>= 2; g >>= 2; r >>= 2;      // 平均(去 2 bit)
            int Uv = ((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128;
            int Vv = ((112 * r - 94 * g - 18 * b + 128) >> 8) + 128;
            if (Uv < 0) Uv = 0; else if (Uv > 255) Uv = 255;
            if (Vv < 0) Vv = 0; else if (Vv > 255) Vv = 255;
            du[bx] = (BYTE)Uv;
            dv[bx] = (BYTE)Vv;
        }
    }
}

// ==========================================================================
// 把 Annex-B 码流切成若干 NAL(起始码统一按 00 00 00 01, OpenH264 输出即此格式)
//  payload 含 NAL 头字节。返回 NAL 个数。
// ==========================================================================
static int SplitAnnexB(const uint8_t* d, size_t n,
                       std::vector<const uint8_t*>& payload,
                       std::vector<size_t>& len) {
    payload.clear();
    len.clear();
    std::vector<size_t> pos;                 // 每个起始码之后的偏移
    for (size_t i = 0; i + 4 <= n;) {
        if (d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 0 && d[i + 3] == 1) {
            pos.push_back(i + 4);
            i += 4;
        } else {
            ++i;
        }
    }
    if (pos.empty()) return 0;
    for (size_t k = 0; k < pos.size(); ++k) {
        size_t end = (k + 1 < pos.size()) ? (pos[k + 1] - 4) : n;
        if (end > pos[k]) {
            payload.push_back(d + pos[k]);
            len.push_back(end - pos[k]);
        }
    }
    return (int)payload.size();
}

// 按编码所需大致码率估算: ~0.12 bit/像素/帧
static int HeuristicBitrate(int w, int h, int fps) {
    long long b = (long long)w * h * fps * 12 / 100;
    if (b < 400000) b = 400000;
    if (b > 20000000) b = 20000000;
    return (int)b;
}

// ==========================================================================
// 把当前鼠标光标按屏幕坐标画进内存 DC(抓屏后调用, 使视频包含鼠标)。
static void DrawCursor(HDC memDC) {
    CURSORINFO ci;
    ci.cbSize = sizeof(ci);
    if (!GetCursorInfo(&ci)) return;
    if ((ci.flags & CURSOR_SHOWING) == 0 || !ci.hCursor) return;
    ICONINFO ii;
    if (GetIconInfo(ci.hCursor, &ii)) {
        int x = ci.ptScreenPos.x - ii.xHotspot;
        int y = ci.ptScreenPos.y - ii.yHotspot;
        DrawIconEx(memDC, x, y, ci.hCursor, 0, 0, 0, nullptr, DI_NORMAL);
        if (ii.hbmColor) DeleteObject(ii.hbmColor);   // GetIconInfo 创建的副本需释放
        if (ii.hbmMask)  DeleteObject(ii.hbmMask);
    }
}

// 后台线程: 抓帧 + OpenH264 编码 + MP4 封装
// ==========================================================================
void ScreenRecorder::ThreadMain() {
    const int w = screenW_, h = screenH_;
    const int capW = (w & ~1) < 2 ? 2 : (w & ~1);
    const int capH = (h & ~1) < 2 ? 2 : (h & ~1);

    bool ok = true;
    std::string failReason;
    int  idx = 0;            // 已写入帧数
    int  attempt = 0;
    double seconds = 0.0;

    HDC     screenDC = nullptr, memDC = nullptr;
    HBITMAP dib = nullptr;
    HGDIOBJ oldBmp = nullptr;
    void*   bits = nullptr;

    std::wstring wideOut = Utf8ToWide(outPathUtf8_);
    auto t0 = std::chrono::steady_clock::now();

    // ---------- 1. GDI 抓屏设备 ----------
    screenDC = GetDC(nullptr);
    if (screenDC) memDC = CreateCompatibleDC(screenDC);
    if (screenDC && memDC) {
        BITMAPINFO bi;
        std::memset(&bi, 0, sizeof(bi));
        bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth       = capW;
        bi.bmiHeader.biHeight      = -capH;      // 顶向下
        bi.bmiHeader.biPlanes      = 1;
        bi.bmiHeader.biBitCount    = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        dib = CreateDIBSection(screenDC, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (dib && bits) oldBmp = SelectObject(memDC, dib);
    }
    if (!screenDC || !memDC || !dib || !bits) {
        ok = false;
        failReason = "无法初始化屏幕抓取设备。";
    }

    // ---------- 2. 编码器 + 参数集 + 打开容器 ----------
    OpenH264::Encoder encoder;
    Mp4Mux mux;
    std::vector<uint8_t> i420((size_t)capW * capH * 3 / 2);
    std::vector<uint8_t> encOut((size_t)capW * capH * 3 + 65536);
    std::vector<uint8_t> sampleBuf;
    std::vector<const uint8_t*> nals;
    std::vector<size_t> nalsLen;

    if (ok) {
        OpenH264::Encoder::Config cfg;
        cfg.width  = capW;
        cfg.height = capH;
        cfg.fps    = fps_;
        int br = HeuristicBitrate(capW, capH, fps_);
        cfg.bitrate    = br;
        cfg.maxBitrate = br * 2;
        cfg.gopSeconds = 1;                    // 每 1 秒一个 IDR, 便于拖动/健壮
        if (!encoder.open(cfg)) {
            ok = false;
            failReason = "OpenH264 编码器打开失败。";
        }
    }

    // 取 SPS/PPS 放进 avcC
    std::vector<uint8_t> sps, pps;
    if (ok) {
        std::vector<uint8_t> ps(4096);
        int nps = encoder.parameterSets(ps.data(), (int)ps.size());
        if (nps > 0) {
            SplitAnnexB(ps.data(), (size_t)nps, nals, nalsLen);
            for (size_t k = 0; k < nals.size(); ++k) {
                int type = nals[k][0] & 0x1F;
                if (type == 7 && sps.empty())
                    sps.assign(nals[k], nals[k] + nalsLen[k]);
                else if (type == 8 && pps.empty())
                    pps.assign(nals[k], nals[k] + nalsLen[k]);
            }
        }
        if (sps.empty() || pps.empty()) {
            ok = false;
            failReason = "未能获取 H.264 SPS/PPS。";
        }
    }

    if (ok) {
        // 父目录不存在时自动创建(静态库自洽, 不依赖调用方)
        if (!EnsureDirOfFile(wideOut)) {
            ok = false;
            failReason = "无法创建保存目录。";
        }
    }
    if (ok) {
        if (!mux.Open(wideOut, capW, capH, fps_, sps.data(), (int)sps.size(),
                      pps.data(), (int)pps.size(), &failReason)) {
            ok = false;
        }
    }

    // ---------- 3. 抓帧 + 编码主循环 ----------
    while (ok && !stopRequested_.load()) {
        // 抓一帧
        BitBlt(memDC, 0, 0, capW, capH, screenDC, 0, 0, SRCCOPY | CAPTUREBLT);
        GdiFlush();
        DrawCursor(memDC);                 // 把鼠标光标叠加到画面里

        // BGRA -> I420
        BgraToI420((const BYTE*)bits, capW, capH, i420.data());

        // 编码(首帧强制 IDR)
        bool isKey = false;
        int n = encoder.encode(i420.data(), encOut.data(), (int)encOut.size(),
                               &isKey, (attempt == 0));
        if (n < 0) {
            ok = false;
            failReason = "编码失败(编码器返回错误)。";
            break;
        }
        if (n > 0) {
            int cnt = SplitAnnexB(encOut.data(), (size_t)n, nals, nalsLen);
            // 打包为长度前缀 NAL。
            // 注意: OpenH264 每个 IDR 会发一套新 id 的 SPS/PPS, 故 SPS/PPS 一并
            //       保留在关键帧样本里(avcC 仅作首帧预载), 保证任意关键帧可独立解码。
            sampleBuf.clear();
            for (int k = 0; k < cnt; ++k) {
                size_t ln = nalsLen[k];
                sampleBuf.push_back((uint8_t)(ln >> 24));
                sampleBuf.push_back((uint8_t)(ln >> 16));
                sampleBuf.push_back((uint8_t)(ln >> 8));
                sampleBuf.push_back((uint8_t)ln);
                sampleBuf.insert(sampleBuf.end(), nals[k], nals[k] + ln);
            }
            if (!sampleBuf.empty()) {
                if (!mux.WriteFrame(sampleBuf.data(), sampleBuf.size(), isKey, &failReason)) {
                    ok = false;
                    break;
                }
                ++idx;
                frames_.store(idx);
            }
        }
        ++attempt;

        // 帧率节流
        double targetSec = attempt / (double)fps_;
        auto target = t0 + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                              std::chrono::duration<double>(targetSec));
        std::this_thread::sleep_until(target);
    }

    seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

    // ---------- 4. 收尾 ----------
    if (ok && idx > 0) {
        ok = mux.Finish(&failReason);
    } else if (idx == 0 && ok) {
        ok = false;
        failReason = "未捕获到任何帧，录制已取消。";
    }

    if (memDC && oldBmp) SelectObject(memDC, oldBmp);
    if (dib)      { DeleteObject(dib); dib = nullptr; }
    if (memDC)    { DeleteDC(memDC);   memDC = nullptr; }
    if (screenDC) { ReleaseDC(nullptr, screenDC); screenDC = nullptr; }

    if (!ok) {
        mux.Close();                            // 关闭文件句柄
        DeleteFileW(wideOut.c_str());           // 删掉半成品
    }

    // ---------- 5. 统计 ----------
    stats_.ok       = ok;
    stats_.width    = capW;
    stats_.height   = capH;
    stats_.fps      = fps_;
    stats_.frames   = idx;
    stats_.seconds  = seconds;
    if (!ok) stats_.errorUtf8 = failReason;
    if (ok) stats_.outBytes = mux.FileBytes();

    running_.store(false);
}

} // namespace rec
