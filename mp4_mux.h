// mp4_mux.h —— 极简 MP4 (ISO BMFF) 封装器
// 功能: 把“已编码好的 H.264 帧(4 字节长度前缀 NAL)”顺序写入 .mp4,
//       生成单视频轨(avc1/avcC)文件; moov 放在文件末尾(边录边写, 结束封口)。
// 不依赖 FFmpeg / Media Foundation, 纯文件 I/O, 可独立测试。
#pragma once
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

// H.264 帧级封装: 只负责容器, 不负责编码。
class Mp4Mux {
public:
    Mp4Mux();
    ~Mp4Mux();

    // 打开输出文件并写 ftyp + mdat 头。sps/pps 为完整 NAL 载荷(含起始码后的 NAL 头字节)。
    bool Open(const std::wstring& path, int width, int height, int fps,
              const uint8_t* sps, int spsLen,
              const uint8_t* pps, int ppsLen,
              std::string* err);

    // 写入一帧。sample 指向“已打包好的长度前缀 NAL 序列”
    // (即 每个 NAL = u32 大端长度 + NAL 载荷; 不含 SPS/PPS)。
    // isKey: 本帧是否关键帧(IDR), 用于 stss。
    bool WriteFrame(const uint8_t* sample, size_t size, bool isKey, std::string* err);

    // 写完所有帧后调用: 回填 mdat 大小并写 moov, 然后关闭文件。
    bool Finish(std::string* err);

    // 直接关闭(出错/放弃时用; Finish 成功后无需再调)。
    void Close();

    bool IsOpen() const { return f_ != nullptr; }
    unsigned long long FileBytes() const { return bytes_; }   // Finish 后为文件总字节

private:
    bool Flush(const void* p, size_t n, std::string* err);

    FILE* f_ = nullptr;
    std::string pathUtf8_;           // 便于报错
    long long mdatHeaderPos_ = 0;    // mdat 的 size 字段位置(用于回填)
    long long dataStartPos_ = 0;     // mdat 数据起始(相对文件)
    unsigned long long bytes_ = 0;

    int width_ = 0, height_ = 0, fps_ = 30;

    std::vector<uint8_t> sps_, pps_;         // avcC 用(完整 NAL 载荷)
    std::vector<uint32_t> sampleSizes_;      // 每帧字节数
    std::vector<unsigned long long> offsets_; // 每帧在文件中的偏移
    std::vector<uint32_t> keyFrames_;        // 关键帧序号(1 起)
    int frameCount_ = 0;
};
