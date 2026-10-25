#pragma once
#include "DeckLinkAPI.h"
#include <CoreFoundation/CoreFoundation.h>
#include <cstring>
#include <memory>
#include <utility>

// Small owning wrapper for the SDK's COM-style macOS interfaces. No Windows
// COM initialization, Windows GUID operators, BSTR or WRL is used on macOS.
template<class T> REFIID interfaceId();
#define VTRPON_DECKLINK_IID(T) template<> inline REFIID interfaceId<T>() { return IID_##T; }
VTRPON_DECKLINK_IID(IDeckLinkProfileAttributes)
VTRPON_DECKLINK_IID(IDeckLinkInput)
VTRPON_DECKLINK_IID(IDeckLinkOutput)
VTRPON_DECKLINK_IID(IDeckLinkConfiguration)
VTRPON_DECKLINK_IID(IDeckLinkStatus)
VTRPON_DECKLINK_IID(IDeckLinkVideoBuffer)
#undef VTRPON_DECKLINK_IID
inline bool sameIid(REFIID a, REFIID b) { return std::memcmp(&a, &b, sizeof(a)) == 0; }
// Apple's IUnknownUUID is a CFUUIDRef, unlike the SDK's CFUUIDBytes IIDs.
inline bool sameIid(REFIID a, CFUUIDRef b) { return sameIid(a, CFUUIDGetUUIDBytes(b)); }
template<class T> class ComPtr {
    T *value_ = nullptr;
public:
    ComPtr() = default;
    ComPtr(const ComPtr &p) : value_(p.Get()) { if (value_) value_->AddRef(); }
    template<class U> ComPtr(const ComPtr<U> &p) : value_(p.Get()) { if (value_) value_->AddRef(); }
    ComPtr(ComPtr &&p) noexcept : value_(std::exchange(p.value_, nullptr)) {}
    ~ComPtr() { Reset(); }
    ComPtr &operator=(const ComPtr &p) { if (this != std::addressof(p)) { auto *v=p.Get(); if(v)v->AddRef(); Reset();value_=v; } return *this; }
    ComPtr &operator=(ComPtr &&p) noexcept { if(this!=std::addressof(p)){Reset();value_=std::exchange(p.value_,nullptr);}return *this; }
    template<class U> ComPtr &operator=(const ComPtr<U> &p) { auto *v=p.Get();if(v)v->AddRef();Reset();value_=v;return *this; }
    T *Get() const { return value_; }
    T *operator->() const { return value_; }
    explicit operator bool() const { return value_ != nullptr; }
    T **GetAddressOf() { return &value_; }
    T **operator&() { Reset(); return &value_; }
    void Reset() { auto *v=std::exchange(value_,nullptr);if(v)v->Release(); }
    void Attach(T *p) { Reset();value_=p; }
    template<class U> HRESULT As(ComPtr<U> *p) const { p->Reset();return value_ ? value_->QueryInterface(interfaceId<U>(),reinterpret_cast<void **>(p->GetAddressOf())) : E_NOINTERFACE; }
};
