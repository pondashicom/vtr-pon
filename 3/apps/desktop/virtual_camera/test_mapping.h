#pragma once
#include "protocol.h"
#include <memory>
#include <stdexcept>

// Test-only storage; never opens the installed camera's frame file.
struct TestMapping {
    std::wstring path;
    std::shared_ptr<vtrpon::vcam::Mapping> mapping;
    TestMapping() {
        wchar_t directory[MAX_PATH]{},file[MAX_PATH]{};
        if(!GetTempPathW(MAX_PATH,directory) || !GetTempFileNameW(directory,L"vct",0,file))
            throw std::runtime_error("temporary mapping path");
        path=file;
        HANDLE handle=CreateFileW(file,GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_TEMPORARY,nullptr);
        LARGE_INTEGER size{}; size.QuadPart=sizeof(vtrpon::vcam::Shared);
        const bool sized=handle!=INVALID_HANDLE_VALUE && SetFilePointerEx(handle,size,nullptr,FILE_BEGIN) && SetEndOfFile(handle);
        if(handle!=INVALID_HANDLE_VALUE) CloseHandle(handle);
        if(!sized) { DeleteFileW(file); throw std::runtime_error("temporary mapping size"); }
        mapping=std::make_shared<vtrpon::vcam::Mapping>();
        if(!mapping->open(path)) { DeleteFileW(file); throw std::runtime_error("temporary mapping open"); }
    }
    ~TestMapping() { mapping.reset(); DeleteFileW(path.c_str()); }
    TestMapping(const TestMapping&)=delete;
    TestMapping& operator=(const TestMapping&)=delete;
};
