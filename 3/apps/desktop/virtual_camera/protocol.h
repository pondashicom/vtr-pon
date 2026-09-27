#pragma once
#include <windows.h>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <shlobj.h>

namespace vtrpon::vcam {
inline constexpr GUID clsid{0x78686b3a,0x6797,0x4b6c,{0xa2,0x25,0x81,0x27,0x05,0x22,0x17,0x4f}};
inline constexpr wchar_t clsidText[] = L"{78686B3A-6797-4B6C-A225-81270522174F}";
inline constexpr unsigned width=1920, height=1080, fps=30, bytes=width*height*3/2;
inline constexpr LONG magic=0x56504332;
// V2 uses nonwaiting byte-range locks for slots (bytes 1..3) and producer (0).
// Windows releases locks on process death. The owner field is diagnostic only.
// Separate file/version prevents V1 readers from bypassing these locks.
struct alignas(64) Slot {
    volatile LONG owner;
    LONG generation;
    ULONGLONG serial;
    ULONGLONG timestamp;
    unsigned char pixels[bytes];
};
struct Shared {
    volatile LONG signature;
    volatile LONG generation;
    volatile LONG live;
    volatile LONG latest;
    alignas(8) volatile LONG64 heartbeat;
    alignas(8) volatile LONG64 consumerHeartbeat;
    Slot frames[3];
};
static_assert(sizeof(Shared)==9331456,"Update install.ps1 mapping size with ABI changes");
inline LONG read(volatile LONG& v) { return InterlockedCompareExchange(&v,0,0); }
inline std::wstring framePath() {
    PWSTR directory=nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_ProgramData,0,nullptr,&directory))) return {};
    std::wstring path=std::wstring(directory)+L"\\VTR-PON\\VirtualCameraPOC\\frames-v2.bin";
    CoTaskMemFree(directory);
    return path;
}
class FileLease {
public:
    FileLease(HANDLE file,DWORD offset):file_(file) {
        overlap_.Offset=offset;
        locked_=file!=INVALID_HANDLE_VALUE && LockFileEx(file,
            LOCKFILE_EXCLUSIVE_LOCK|LOCKFILE_FAIL_IMMEDIATELY,0,1,0,&overlap_);
    }
    ~FileLease() { if(locked_) UnlockFileEx(file_,0,1,0,&overlap_); }
    FileLease(const FileLease&)=delete;
    FileLease& operator=(const FileLease&)=delete;
    explicit operator bool() const { return locked_; }
private:
    HANDLE file_;
    OVERLAPPED overlap_{};
    bool locked_=false;
};
class Mapping {
public:
    ~Mapping() { close(); }
    Mapping()=default;
    Mapping(const Mapping&)=delete;
    Mapping& operator=(const Mapping&)=delete;
    FileLease lockSlot(unsigned index) { return FileLease(file_,index+1); }
    bool lockProducer() {
        OVERLAPPED overlap{};
        return LockFileEx(file_,LOCKFILE_EXCLUSIVE_LOCK|LOCKFILE_FAIL_IMMEDIATELY,0,1,0,&overlap);
    }
    bool open(const std::wstring& path=framePath()) {
        if (data) return true;
        file_=CreateFileW(path.c_str(),GENERIC_READ|GENERIC_WRITE,
            FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
        if (file_==INVALID_HANDLE_VALUE) return false;
        LARGE_INTEGER size{};
        if (!GetFileSizeEx(file_,&size) || size.QuadPart!=sizeof(Shared)) { close(); return false; }
        mapping_=CreateFileMappingW(file_,nullptr,PAGE_READWRITE,0,0,nullptr);
        if (mapping_) data=static_cast<Shared*>(MapViewOfFile(mapping_,FILE_MAP_ALL_ACCESS,0,0,sizeof(Shared)));
        if (!data) { close(); return false; }
        return true;
    }
    void close() {
        if (data) UnmapViewOfFile(data);
        if (mapping_) CloseHandle(mapping_);
        if (file_!=INVALID_HANDLE_VALUE) CloseHandle(file_);
        data=nullptr; mapping_=nullptr; file_=INVALID_HANDLE_VALUE;
    }
    Shared* data=nullptr;
private:
    HANDLE file_=INVALID_HANDLE_VALUE, mapping_=nullptr;
};
inline bool copyLatest(Mapping& mapping, unsigned char* output, ULONGLONG* timestamp=nullptr, LONG* copiedGeneration=nullptr) {
    auto* s=mapping.data;
    if (!s || read(s->signature)!=magic || !read(s->live)) return false;
    const auto heartbeat=static_cast<ULONGLONG>(InterlockedCompareExchange64(&s->heartbeat,0,0));
    const auto now=GetTickCount64();
    if (heartbeat>now || now-heartbeat>1500) return false;
    const LONG generation=read(s->generation), index=read(s->latest);
    if (index<0 || index>=3) return false;
    auto lease=mapping.lockSlot(static_cast<unsigned>(index));
    if(!lease) return false;
    Slot& slot=s->frames[index];
    InterlockedExchange(&slot.owner,2);
    const bool valid=slot.generation==generation && slot.timestamp<=now && now-slot.timestamp<=1500;
    if (valid) {
        std::memcpy(output,slot.pixels,bytes);
        if(timestamp) *timestamp=slot.timestamp;
        if(copiedGeneration) *copiedGeneration=generation;
    }
    InterlockedExchange(&slot.owner,0);
    return valid && read(s->live) && read(s->generation)==generation;
}
// DLL-side standby. GDI is used once; only luminance differs from neutral black.
inline std::vector<unsigned char> blackFrame() {
    std::vector<unsigned char> result(bytes,128);
    std::memset(result.data(),16,width*height);
    return result;
}
inline bool showDisabledSlate(Shared* s) {
    if(!s || read(s->signature)!=magic || read(s->live)) return false;
    const auto heartbeat=static_cast<ULONGLONG>(InterlockedCompareExchange64(&s->heartbeat,0,0));
    const auto now=GetTickCount64();
    return heartbeat<=now && now-heartbeat<=1500;
}
inline std::vector<unsigned char> standby() {
    auto result=blackFrame();
    BITMAPINFO info{}; info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth=width; info.bmiHeader.biHeight=-static_cast<LONG>(height);
    info.bmiHeader.biPlanes=1; info.bmiHeader.biBitCount=32; info.bmiHeader.biCompression=BI_RGB;
    void* pixels=nullptr;
    HDC dc=CreateCompatibleDC(nullptr);
    HBITMAP bitmap=CreateDIBSection(dc,&info,DIB_RGB_COLORS,&pixels,nullptr,0);
    if (dc && bitmap && pixels) {
        auto old=SelectObject(dc,bitmap);
        std::memset(pixels,0,width*height*4);
        HFONT font=CreateFontW(-22,0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,ANTIALIASED_QUALITY,DEFAULT_PITCH,L"Segoe UI");
        auto oldFont=SelectObject(dc,font);
        SetTextColor(dc,RGB(255,255,255)); SetBkMode(dc,TRANSPARENT);
        RECT rect{48,0,static_cast<LONG>(width)-48,static_cast<LONG>(height)};
        DrawTextW(dc,L"VTR-PON Virtual Camera",-1,&rect,DT_LEFT|DT_VCENTER|DT_SINGLELINE|DT_NOPREFIX);
        auto* src=static_cast<unsigned char*>(pixels);
        for (unsigned i=0;i<width*height;++i) result[i]=static_cast<unsigned char>(16+(src[i*4]*219+127)/255);
        SelectObject(dc,oldFont); DeleteObject(font); SelectObject(dc,old);
    }
    if (bitmap) DeleteObject(bitmap);
    if (dc) DeleteDC(dc);
    return result;
}
}
