// mp4_mux.cpp —— 极简 MP4 封装器实现
// 参考 ISO/IEC 14496-12/-15。只处理“单视频轨 H.264 (avc1)”。
// 层次约定: Box  = size+type+body; FullBox = size+type+version+flags+body。
#include "mp4_mux.h"

#include <cstring>

#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>   // _fseeki64/_ftelli64

// 放全局: 供下面的匿名命名空间辅助函数与类成员函数共同使用
typedef std::vector<uint8_t> Bytes;

namespace {

// ---------------- 大端写入辅助 ----------------
inline void PutU8(Bytes& v, uint8_t x) { v.push_back(x); }
inline void PutU16(Bytes& v, uint16_t x) {
    v.push_back((uint8_t)(x >> 8)); v.push_back((uint8_t)x);
}
inline void PutU24(Bytes& v, uint32_t x) {
    v.push_back((uint8_t)(x >> 16)); v.push_back((uint8_t)(x >> 8)); v.push_back((uint8_t)x);
}
inline void PutU32(Bytes& v, uint32_t x) {
    v.push_back((uint8_t)(x >> 24)); v.push_back((uint8_t)(x >> 16));
    v.push_back((uint8_t)(x >> 8));  v.push_back((uint8_t)x);
}
inline void PutU64(Bytes& v, uint64_t x) {
    PutU32(v, (uint32_t)(x >> 32)); PutU32(v, (uint32_t)x);
}
inline void PutBytes(Bytes& v, const void* p, size_t n) {
    const uint8_t* b = (const uint8_t*)p;
    v.insert(v.end(), b, b + n);
}

Bytes Box(const char type[4], const Bytes& body) {
    Bytes b;
    PutU32(b, (uint32_t)(8 + body.size()));
    PutBytes(b, type, 4);
    b.insert(b.end(), body.begin(), body.end());
    return b;
}
Bytes FullBox(const char type[4], uint8_t version, uint32_t flags, const Bytes& body) {
    Bytes f;
    f.push_back(version);
    PutU24(f, flags);
    f.insert(f.end(), body.begin(), body.end());
    return Box(type, f);
}

} // namespace

Mp4Mux::Mp4Mux() {}
Mp4Mux::~Mp4Mux() {
    if (f_) { fclose(f_); f_ = nullptr; }
}

void Mp4Mux::Close() {
    if (f_) { fclose(f_); f_ = nullptr; }
}

bool Mp4Mux::Flush(const void* p, size_t n, std::string* err) {
    if (fwrite(p, 1, n, f_) != n) {
        if (err) *err = "写文件失败(磁盘空间不足?)";
        return false;
    }
    bytes_ += n;
    return true;
}

bool Mp4Mux::Open(const std::wstring& path, int width, int height, int fps,
                  const uint8_t* sps, int spsLen,
                  const uint8_t* pps, int ppsLen,
                  std::string* err) {
    if (err) err->clear();
    width_ = width; height_ = height;
    fps_ = fps > 0 ? fps : 30;
    sps_.assign(sps, sps + spsLen);
    pps_.assign(pps, pps + ppsLen);
    sampleSizes_.clear();
    offsets_.clear();
    keyFrames_.clear();
    frameCount_ = 0;
    bytes_ = 0;

    f_ = _wfopen(path.c_str(), L"wb");
    if (!f_) {
        if (err) {
            char tmp[160];
            std::snprintf(tmp, sizeof(tmp), "无法创建输出文件(错误码 %lu)。",
                          (unsigned long)GetLastError());
            *err = tmp;
        }
        return false;
    }

    // ---------- ftyp ----------
    Bytes ftypBody;
    PutU32(ftypBody, 0x69736F6D);  // major_brand 'isom'
    PutU32(ftypBody, 0);           // minor_version
    PutU32(ftypBody, 0x69736F6D);  // compatible 'isom'
    PutU32(ftypBody, 0x69736F32);  // 'iso2'
    PutU32(ftypBody, 0x61766331);  // 'avc1'
    PutU32(ftypBody, 0x6D703431);  // 'mp41'
    Bytes ftyp = Box("ftyp", ftypBody);
    if (!Flush(ftyp.data(), ftyp.size(), err)) return false;

    // ---------- mdat 占位(先写 8 字节头, 结束回填大小) ----------
    mdatHeaderPos_ = (long long)bytes_;
    const uint8_t ph[8] = { 0, 0, 0, 0, 'm', 'd', 'a', 't' };
    if (!Flush(ph, 8, err)) return false;
    return true;
}

bool Mp4Mux::WriteFrame(const uint8_t* sample, size_t size, bool isKey, std::string* err) {
    if (!f_ || size == 0) return false;
    if (size > 0xFFFFFFFFULL) { if (err) *err = "单帧过大。"; return false; }
    offsets_.push_back((unsigned long long)bytes_);
    if (!Flush(sample, size, err)) return false;
    sampleSizes_.push_back((uint32_t)size);
    if (isKey) keyFrames_.push_back((uint32_t)frameCount_ + 1);  // 1 起序号
    ++frameCount_;
    return true;
}

bool Mp4Mux::Finish(std::string* err) {
    if (err) err->clear();
    if (!f_) { if (err) *err = "文件未打开。"; return false; }
    if (frameCount_ == 0 || sps_.empty() || pps_.empty()) {
        if (err) *err = "没有任何帧或参数集，无法封装。";
        return false;
    }

    // ---------- 回填 mdat 大小 ----------
    unsigned long long mdatSize = bytes_ - (unsigned long long)mdatHeaderPos_;
    if (_fseeki64(f_, mdatHeaderPos_, SEEK_SET) != 0) { if (err) *err = "定位失败。"; return false; }
    uint8_t sz[4] = { (uint8_t)(mdatSize >> 24), (uint8_t)(mdatSize >> 16),
                      (uint8_t)(mdatSize >> 8), (uint8_t)mdatSize };
    if (fwrite(sz, 1, 4, f_) != 4) { if (err) *err = "写文件失败。"; return false; }
    if (_fseeki64(f_, 0, SEEK_END) != 0) { if (err) *err = "定位失败。"; return false; }

    // ---------- 组装 moov ----------
    unsigned long long durMs = (unsigned long long)((unsigned long long)frameCount_ * 1000ULL / (unsigned long long)fps_);
    if (durMs == 0) durMs = 1;
    static const uint32_t kIdentity[9] = { 0x00010000,0,0, 0,0x00010000,0, 0,0,0x40000000 };

    // ---- mvhd ----
    Bytes mvhdBody;
    PutU32(mvhdBody, 0);                      // creation_time
    PutU32(mvhdBody, 0);                      // modification_time
    PutU32(mvhdBody, 1000);                   // timescale
    PutU32(mvhdBody, (uint32_t)durMs);        // duration
    PutU32(mvhdBody, 0x00010000);             // rate 1.0 (16.16)
    PutU16(mvhdBody, 0x0100);                 // volume
    PutU16(mvhdBody, 0);                      // reserved
    PutU32(mvhdBody, 0);                      // reserved
    PutU32(mvhdBody, 0);                      // reserved
    for (int i = 0; i < 9; ++i) PutU32(mvhdBody, kIdentity[i]);
    for (int i = 0; i < 6; ++i) PutU32(mvhdBody, 0);
    PutU32(mvhdBody, 2);                      // next_track_ID
    Bytes mvhd = FullBox("mvhd", 0, 0, mvhdBody);

    // ---- tkhd ----
    Bytes tkhdBody;
    PutU32(tkhdBody, 0);                      // creation_time
    PutU32(tkhdBody, 0);                      // modification_time
    PutU32(tkhdBody, 1);                      // track_ID
    PutU32(tkhdBody, 0);                      // reserved
    PutU32(tkhdBody, (uint32_t)durMs);        // duration(与 mvhd 同一时间轴)
    PutU64(tkhdBody, 0);                      // reserved
    PutU16(tkhdBody, 0);                      // layer
    PutU16(tkhdBody, 0);                      // alternate_group
    PutU16(tkhdBody, 0);                      // volume
    PutU16(tkhdBody, 0);                      // reserved
    for (int i = 0; i < 9; ++i) PutU32(tkhdBody, kIdentity[i]);
    PutU32(tkhdBody, ((uint32_t)width_) << 16);
    PutU32(tkhdBody, ((uint32_t)height_) << 16);
    Bytes tkhd = FullBox("tkhd", 0, 0x000007, tkhdBody);

    // ---- mdhd ----
    Bytes mdhdBody;
    PutU32(mdhdBody, 0);                      // creation_time
    PutU32(mdhdBody, 0);                      // modification_time
    PutU32(mdhdBody, (uint32_t)fps_);         // media timescale = fps
    PutU32(mdhdBody, (uint32_t)frameCount_);  // media duration(帧数)
    PutU16(mdhdBody, 0x55C4);                 // language = und
    PutU16(mdhdBody, 0);                      // pre_defined
    Bytes mdhd = FullBox("mdhd", 0, 0, mdhdBody);

    // ---- hdlr ----
    Bytes hdlrBody;
    PutU32(hdlrBody, 0);                      // pre_defined
    PutU32(hdlrBody, 0x76696465);             // handler_type 'vide'
    PutU32(hdlrBody, 0);                      // reserved
    PutU32(hdlrBody, 0);                      // reserved
    PutU32(hdlrBody, 0);                      // reserved
    PutU8(hdlrBody, 0);                       // name(空串)
    Bytes hdlr = FullBox("hdlr", 0, 0, hdlrBody);

    // ---- vmhd ----
    Bytes vmhdBody;
    PutU16(vmhdBody, 0);                      // graphicsmode
    PutU16(vmhdBody, 0); PutU16(vmhdBody, 0); PutU16(vmhdBody, 0);  // opcolor
    Bytes vmhd = FullBox("vmhd", 0, 1, vmhdBody);

    // ---- dinf / dref / url ----
    Bytes urlBody;
    PutU8(urlBody, 0); PutU24(urlBody, 1);    // version 0, flags=1(自包含)
    Bytes url = Box("url ", urlBody);
    Bytes drefBody;
    PutU32(drefBody, 1);                      // entry_count
    drefBody.insert(drefBody.end(), url.begin(), url.end());
    Bytes dref = FullBox("dref", 0, 0, drefBody);
    Bytes dinf = Box("dinf", dref);

    // ---- avcC ----
    Bytes avcCBody;
    PutU8(avcCBody, 1);                       // configurationVersion
    PutU8(avcCBody, sps_.size() > 1 ? sps_[1] : 66);   // AVCProfileIndication
    PutU8(avcCBody, sps_.size() > 2 ? sps_[2] : 0);    // profile_compatibility
    PutU8(avcCBody, sps_.size() > 3 ? sps_[3] : 30);   // AVCLevelIndication
    PutU8(avcCBody, 0xFF);                    // lengthSizeMinusOne=3 (4字节)
    PutU8(avcCBody, 0xE1);                    // numSPS=1 (保留位 111)
    PutU16(avcCBody, (uint16_t)sps_.size());
    PutBytes(avcCBody, sps_.data(), sps_.size());
    PutU8(avcCBody, 1);                       // numPPS=1
    PutU16(avcCBody, (uint16_t)pps_.size());
    PutBytes(avcCBody, pps_.data(), pps_.size());
    Bytes avcC = Box("avcC", avcCBody);

    // ---- avc1 sample entry ----
    Bytes avc1Body;
    for (int i = 0; i < 6; ++i) PutU8(avc1Body, 0);  // reserved[6]
    PutU16(avc1Body, 1);                             // data_reference_index
    PutU16(avc1Body, 0);                             // pre_defined
    PutU16(avc1Body, 0);                             // reserved
    PutU32(avc1Body, 0); PutU32(avc1Body, 0); PutU32(avc1Body, 0);  // pre_defined[3]
    PutU16(avc1Body, (uint16_t)width_);
    PutU16(avc1Body, (uint16_t)height_);
    PutU32(avc1Body, 0x00480000);              // horizresolution
    PutU32(avc1Body, 0x00480000);              // vertresolution
    PutU32(avc1Body, 0);                       // reserved
    PutU16(avc1Body, 1);                       // frame_count
    char cname[32];
    std::memset(cname, 0, sizeof(cname));
    std::strncpy(cname, "OpenH264", 31);
    PutBytes(avc1Body, cname, 32);             // compressorname
    PutU16(avc1Body, 0x0018);                  // depth
    PutU16(avc1Body, 0xFFFF);                  // pre_defined(-1)
    avc1Body.insert(avc1Body.end(), avcC.begin(), avcC.end());
    Bytes avc1 = Box("avc1", avc1Body);

    // ---- stsd ----
    Bytes stsdBody;
    PutU32(stsdBody, 1);                       // entry_count
    stsdBody.insert(stsdBody.end(), avc1.begin(), avc1.end());
    Bytes stsd = FullBox("stsd", 0, 0, stsdBody);

    // ---- stts: 全部帧等时长, media timescale=fps => delta=1 ----
    Bytes sttsBody;
    PutU32(sttsBody, 1);                       // entry_count
    PutU32(sttsBody, (uint32_t)frameCount_);   // sample_count
    PutU32(sttsBody, 1);                       // sample_delta
    Bytes stts = FullBox("stts", 0, 0, sttsBody);

    // ---- stss(有则写) ----
    Bytes stss;
    if (!keyFrames_.empty()) {
        Bytes ss;
        PutU32(ss, (uint32_t)keyFrames_.size());
        for (size_t i = 0; i < keyFrames_.size(); ++i) PutU32(ss, keyFrames_[i]);
        stss = FullBox("stss", 0, 0, ss);
    }

    // ---- stsc: 每 chunk 1 帧 ----
    Bytes stscBody;
    PutU32(stscBody, 1);                       // entry_count
    PutU32(stscBody, 1);                       // first_chunk
    PutU32(stscBody, 1);                       // samples_per_chunk
    PutU32(stscBody, 1);                       // sample_description_index
    Bytes stsc = FullBox("stsc", 0, 0, stscBody);

    // ---- stsz ----
    Bytes stszBody;
    PutU32(stszBody, 0);                       // sample_size(不定长)
    PutU32(stszBody, (uint32_t)frameCount_);
    for (size_t i = 0; i < sampleSizes_.size(); ++i) PutU32(stszBody, sampleSizes_[i]);
    Bytes stsz = FullBox("stsz", 0, 0, stszBody);

    // ---- stco / co64 ----
    bool need64 = false;
    for (size_t i = 0; i < offsets_.size(); ++i)
        if (offsets_[i] > 0xFFFFFFFFULL) { need64 = true; break; }
    Bytes stco;
    if (!need64) {
        Bytes b;
        PutU32(b, (uint32_t)offsets_.size());
        for (size_t i = 0; i < offsets_.size(); ++i) PutU32(b, (uint32_t)offsets_[i]);
        stco = FullBox("stco", 0, 0, b);
    } else {
        Bytes b;
        PutU32(b, (uint32_t)offsets_.size());
        for (size_t i = 0; i < offsets_.size(); ++i) PutU64(b, offsets_[i]);
        stco = FullBox("co64", 0, 0, b);
    }

    // ---- 组装 stbl / minf / mdia / trak / moov ----
    Bytes stblBody;
    stblBody.insert(stblBody.end(), stsd.begin(), stsd.end());
    stblBody.insert(stblBody.end(), stts.begin(), stts.end());
    if (!stss.empty()) stblBody.insert(stblBody.end(), stss.begin(), stss.end());
    stblBody.insert(stblBody.end(), stsc.begin(), stsc.end());
    stblBody.insert(stblBody.end(), stsz.begin(), stsz.end());
    stblBody.insert(stblBody.end(), stco.begin(), stco.end());
    Bytes stbl = Box("stbl", stblBody);

    Bytes minfBody;
    minfBody.insert(minfBody.end(), vmhd.begin(), vmhd.end());
    minfBody.insert(minfBody.end(), dinf.begin(), dinf.end());
    minfBody.insert(minfBody.end(), stbl.begin(), stbl.end());
    Bytes minf = Box("minf", minfBody);

    Bytes mdiaBody;
    mdiaBody.insert(mdiaBody.end(), mdhd.begin(), mdhd.end());
    mdiaBody.insert(mdiaBody.end(), hdlr.begin(), hdlr.end());
    mdiaBody.insert(mdiaBody.end(), minf.begin(), minf.end());
    Bytes mdia = Box("mdia", mdiaBody);

    Bytes trakBody;
    trakBody.insert(trakBody.end(), tkhd.begin(), tkhd.end());
    trakBody.insert(trakBody.end(), mdia.begin(), mdia.end());
    Bytes trak = Box("trak", trakBody);

    Bytes moovBody;
    moovBody.insert(moovBody.end(), mvhd.begin(), mvhd.end());
    moovBody.insert(moovBody.end(), trak.begin(), trak.end());
    Bytes moov = Box("moov", moovBody);

    if (!Flush(moov.data(), moov.size(), err)) return false;

    fclose(f_);
    f_ = nullptr;
    return true;
}
