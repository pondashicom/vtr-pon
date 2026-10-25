#pragma once

#include <QCryptographicHash>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>

#include <windows.h>
#include <aclapi.h>

#include <iostream>
#include <string>
#include <vector>

namespace pdf_test {

inline QByteArray sha256(const QString &path) {
  QFile file(path);
  QCryptographicHash hash(QCryptographicHash::Sha256);
  return file.open(QIODevice::ReadOnly) && hash.addData(&file)
             ? hash.result().toHex() : QByteArray{};
}

inline QString firstPng(const QString &root) {
  QDirIterator files(root, {QStringLiteral("*.png")}, QDir::Files,
                     QDirIterator::Subdirectories);
  return files.hasNext() ? files.next() : QString{};
}

// Apply only to a newly created fixture file. Preserve attributes and the DACL.
class ReadDataDeny final {
public:
  explicit ReadDataDeny(const QString &path)
      : path_(QDir::toNativeSeparators(path).toStdWString()) {}
  ~ReadDataDeny() {
    if (!restore()) std::cerr << "Could not restore PDF fixture DACL\n";
    if (descriptor_) LocalFree(descriptor_);
  }
  ReadDataDeny(const ReadDataDeny &) = delete;
  ReadDataDeny &operator=(const ReadDataDeny &) = delete;

  bool apply() {
    if (GetNamedSecurityInfoW(path_.data(), SE_FILE_OBJECT,
                             DACL_SECURITY_INFORMATION, nullptr, nullptr,
                             &original_, nullptr, &descriptor_) != ERROR_SUCCESS)
      return false;
    DWORD revision = 0;
    if (!GetSecurityDescriptorControl(descriptor_, &control_, &revision)) return false;
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    DWORD size = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    std::vector<unsigned char> bytes(size);
    const bool queried = GetTokenInformation(token, TokenUser, bytes.data(), size, &size);
    CloseHandle(token);
    if (!queried) return false;
    EXPLICIT_ACCESSW entry{};
    entry.grfAccessPermissions = FILE_READ_DATA;
    entry.grfAccessMode = DENY_ACCESS;
    entry.grfInheritance = NO_INHERITANCE;
    entry.Trustee.TrusteeForm = TRUSTEE_IS_SID;
    entry.Trustee.TrusteeType = TRUSTEE_IS_USER;
    entry.Trustee.ptstrName = reinterpret_cast<LPWSTR>(
        reinterpret_cast<TOKEN_USER *>(bytes.data())->User.Sid);
    PACL denied = nullptr;
    if (SetEntriesInAclW(1, &entry, original_, &denied) != ERROR_SUCCESS) return false;
    const DWORD error = SetNamedSecurityInfoW(path_.data(), SE_FILE_OBJECT,
        DACL_SECURITY_INFORMATION, nullptr, nullptr, denied, nullptr);
    LocalFree(denied);
    active_ = error == ERROR_SUCCESS;
    return active_;
  }

  bool restore() {
    if (!active_) return true;
    const SECURITY_INFORMATION protection = control_ & SE_DACL_PROTECTED
        ? PROTECTED_DACL_SECURITY_INFORMATION : UNPROTECTED_DACL_SECURITY_INFORMATION;
    if (SetNamedSecurityInfoW(path_.data(), SE_FILE_OBJECT,
        DACL_SECURITY_INFORMATION | protection, nullptr, nullptr, original_, nullptr)
        != ERROR_SUCCESS) return false;
    active_ = false;
    return true;
  }
private:
  std::wstring path_;
  PSECURITY_DESCRIPTOR descriptor_ = nullptr;
  PACL original_ = nullptr;
  SECURITY_DESCRIPTOR_CONTROL control_ = 0;
  bool active_ = false;
};

class DeleteDeny final {
public:
  ~DeleteDeny() { release(); }
  DeleteDeny() = default;
  DeleteDeny(const DeleteDeny &) = delete;
  DeleteDeny &operator=(const DeleteDeny &) = delete;
  bool hold(const QString &path) {
    if (path.isEmpty()) return false;
    const auto native = QDir::toNativeSeparators(path).toStdWString();
    handle_ = CreateFileW(native.c_str(), GENERIC_READ,
                         FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                         OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle_ == INVALID_HANDLE_VALUE) return false;
    const bool deleted = DeleteFileW(native.c_str());
    const DWORD error = GetLastError();
    std::cout << "DeleteFileW denied=" << !deleted << " error=" << error << '\n';
    return !deleted && error == ERROR_SHARING_VIOLATION && QFileInfo::exists(path);
  }
  void release() {
    if (handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_);
    handle_ = INVALID_HANDLE_VALUE;
  }
private:
  HANDLE handle_ = INVALID_HANDLE_VALUE;
};

} // namespace pdf_test
