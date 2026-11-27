// MirRoar server (Windows) - COM smart pointer and UTF-8/UTF-16 helpers.
#pragma once
#include <winsock2.h>
#include <windows.h>
#include <string>
#include "../../core/log.h"
#include "../platform.h"

template <typename T>
class ComPtr {
public:
    ComPtr() = default;
    ComPtr(T* p) : p_(p) {}
    ComPtr(const ComPtr& o) : p_(o.p_) {
        if (p_) p_->AddRef();
    }
    ComPtr& operator=(const ComPtr& o) {
        if (this != &o) {
            reset();
            p_ = o.p_;
            if (p_) p_->AddRef();
        }
        return *this;
    }
    ~ComPtr() { reset(); }
    void reset() {
        if (p_) {
            p_->Release();
            p_ = nullptr;
        }
    }
    T* get() const { return p_; }
    T* operator->() const { return p_; }
    T** put() {
        reset();
        return &p_;
    }
    explicit operator bool() const { return p_ != nullptr; }
    template <typename U>
    HRESULT as(ComPtr<U>& out) const {
        return p_ ? p_->QueryInterface(__uuidof(U), (void**)out.put()) : E_POINTER;
    }

private:
    T* p_ = nullptr;
};

inline std::string wideToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

inline std::wstring utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}
