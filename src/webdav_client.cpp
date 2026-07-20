/*
 * webdav2localdisk — webdav_client.c
 *
 * The WebDAV protocol client. Implements the WinFsp file-system
 * callbacks in terms of WebDAV verbs (PROPFIND / GET / PUT / DELETE /
 * MKCOL / MOVE / COPY). Has zero knowledge of the DRIVE_FIXED trick;
 * the mount-time hack lives entirely in fs_driver.c.
 *
 * Persistence model: WebDAV is a stateless protocol over HTTP, so each
 * WinFsp transact translates to one or more HTTP requests below. A
 * local on-disk cache (cache.c) absorbs repeated reads and writes;
 * flushing happens lazily via Close/Flush and aggressively on Read
 * when a fresh HEAD confirms the cache matches ETag.
 *
 * The file is laid out to map 1:1 to the WinFsp FSP_FILE_SYSTEM
 * interface table `g_kWdlFsInterface` declared here and consumed by
 * fs_driver.c. Keep the function order identical for easy review.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "fs.h"
#include "webdav_client.h"
#include "cache.h"

#include <string>
#include <vector>
#include <cstring>
#include <cstdio>

namespace wdl {

/* ---------------------------------------------------------------- */
/* Forward-declare the interface table so fs_driver.c can &ref it   */
/* ---------------------------------------------------------------- */

extern "C" FSP_FILE_SYSTEM_INTERFACE g_kWdlFsInterface;

namespace {

/* A thin RAII handle around WinHttp HINTERNET handles. */
class HttpSession {
 public:
  HttpSession(const std::wstring &user_agent) {
    session_ = WinHttpOpen(user_agent.c_str(),
                           WINHTTP_ACCESS_TYPE_NO_PROXY,
                           WINHTTP_NO_PROXY_NAME,
                           WINHTTP_NO_PROXY_BYPASS, 0);
  }
  ~HttpSession() {
    if (session_) WinHttpCloseHandle(session_);
  }
  HINTERNET handle() const { return session_; }
  operator bool() const { return session_ != nullptr; }

 private:
  HINTERNET session_ = nullptr;
};

/* Build a WebDAV URL from a context + relative path. */
std::wstring BuildUrl(const WebDavCtx &ctx, const wchar_t *path) {
  std::wstring url = ctx.server_url;
  if (!url.empty() && url.back() != L'/') url.push_back(L'/');
  if (path && *path) url += path;       /* std::wstring concat fix below */
  /* Simplify - replace backslashes coming from WinFsp with forward. */
  for (auto &c : url) {
    if (c == L'\\') c = L'/';
  }
  return url;
}

/* Auth header for Basic credentials. */
std::wstring BuildAuthHeader(const WebDavCtx &ctx) {
  return L"Authorization: Basic " + ctx.username + L":" + ctx.password; // TODO base64
}

/* ---------------------------------------------------------------- */
/* Sync helpers — map WinFsp requests to WebDAV verbs               */
/* ---------------------------------------------------------------- */

/* PROPFIND path -> fill FSP_FSCTL_FILE_INFO. Returns STATUS_*. */
NTSTATUS DavPropFindToInfo(const WebDavCtx &ctx, const wchar_t *path,
                            FSP_FSCTL_FILE_INFO *info) {
  /* TODO: implement using xml parser. Skeleton below. */
  (void)ctx; (void)path; (void)info;
  return STATUS_OBJECT_NAME_NOT_FOUND;
}

}  // namespace

/* ---------------------------------------------------------------- */
/* WinFsp callbacks — forwarded to WebDAV.                            */
/* ---------------------------------------------------------------- */

static NTSTATUS TPICreate(FSP_FILE_SYSTEM *fs,
                          FSP_FSCTL_TRANSACT_REQ *req, PWSTR name,
                          UINT32 create_opts, UINT32 granted_access,
                          UINT32 attrs,
                          PSECURITY_DESCRIPTOR sd, UINT64 alloc_size,
                          PVOID *out_file_context,
                          FSP_FSCTL_FILE_INFO *out_info) {
  auto *ctx = static_cast<WebDavCtx *>(FspFileSystemGetUserContext(fs));
  (void)req; (void)granted_access; (void)sd; (void)alloc_size;

  if ((create_opts & FILE_DIRECTORY_FILE) != 0) {
    /* MKCOL */
  } else {
    /* PUT empty blob to create. */
  }
  FspFileSystemSetFileInfo(ctx->mu, FspFileSystemOpCreate, *out_file_context,
                           out_info);
  (void)ctx; (void)name; (void)attrs; (void)out_file_context; (void)out_info;
  return STATUS_NOT_IMPLEMENTED;
}

static NTSTATUS TPIOpen(FSP_FILE_SYSTEM *fs,
                        FSP_FSCTL_TRANSACT_REQ *req, PWSTR path,
                        UINT32 rights, PVOID *ctx_out) {
  (void)fs; (void)req; (void)path; (void)rights; (void)ctx_out;
  return STATUS_NOT_IMPLEMENTED;
}

static NTSTATUS TPIRead(FSP_FILE_SYSTEM *fs,
                        PVOID file_context, PVOID buffer, UINT64 offset,
                        ULONG length, PULONG p_transferred) {
  /* GET with Range: bytes=offset-offset+length-1 => buffer[]. */
  Cache *cache = static_cast<Cache *>(file_context);
  ULONG got = 0;
  if (cache->Read(offset, buffer, length, &got) && got > 0) {
    *p_transferred = got;
    return STATUS_SUCCESS;
  }
  return STATUS_END_OF_FILE;
}

static NTSTATUS TPIWrite(FSP_FILE_SYSTEM *fs,
                         PVOID file_context, PVOID buffer, UINT64 offset,
                         ULONG length, PULONG p_transferred,
                         FSP_FSCTL_FILE_INFO *info) {
  Cache *cache = static_cast<Cache *>(file_context);
  if (!cache->Write(offset, buffer, length)) {
    return STATUS_INSUFFICIENT_RESOURCES;
  }
  *p_transferred = length;
  cache->Info(info);
  return STATUS_SUCCESS;
}

static NTSTATUS TPIFlush(FSP_FILE_SYSTEM *fs,
                          PVOID file_context,
                          FSP_FSCTL_FILE_INFO *info) {
  Cache *cache = static_cast<Cache *>(file_context);
  if (!cache->FlushDisk()) return STATUS_IO_DEVICE_ERROR;
  if (info) cache->Info(info);
  return STATUS_SUCCESS;
}

static NTSTATUS TPIClose(FSP_FILE_SYSTEM *fs,
                          FSP_FSCTL_TRANSACT_REQ *req,
                          PVOID file_context) {
  Cache *cache = static_cast<Cache *>(file_context);
  cache->FlushDisk();           /* best-effort */
  delete cache;
  (void)fs; (void)req;
  return STATUS_SUCCESS;
}

static NTSTATUS TPIGetInfo(FSP_FILE_SYSTEM *fs,
                          PVOID file_context,
                          FSP_FSCTL_FILE_INFO *info) {
  Cache *cache = static_cast<Cache *>(file_context);
  cache->Info(info);
  return STATUS_SUCCESS;
}

static NTSTATUS TPISetInfo(FSP_FILE_SYSTEM *fs,
                          PVOID file_context,
                          UINT32 info_class,
                          PVOID buf,
                          FSP_FSCTL_FILE_INFO *info) {
  /* Reduce to: rename (MOVE), delete-on-close (DELETE), attrs (PROPPATCH). */
  (void)fs; (void)file_context; (void)info_class; (void)buf; (void)info;
  return STATUS_NOT_IMPLEMENTED;
}

static NTSTATUS TPIEnumDirectory(FSP_FILE_SYSTEM *fs,
                                  FSP_FSCTL_TRANSACT_REQ *req,
                                  PCWSTR pattern, PVOID *marker,
                                  UINT64 *out_marker) {
  /* PROPFIND depth:1 -> emit each <D:response>. */
  (void)fs; (void)req; (void)pattern; (void)marker; (void)out_marker;
  return STATUS_NOT_IMPLEMENTED;
}

/* ---------------------------------------------------------------- */
/* The interface table — order MUST match FSP_FILE_SYSTEM_INTERFACE  */
/* (see Winfsp docs for exact member layout).                        */
/* ---------------------------------------------------------------- */

extern "C" FSP_FILE_SYSTEM_INTERFACE g_kWdlFsInterface = {
  /* GetVolumeInfo       */ nullptr,                /* filled at runtime */
  /* SetVolumeLabel      */ nullptr,                /* no-op             */
  /* GetSecurity         */ nullptr,                /* see below         */
  /* SetSecurity         */ nullptr,                /* see below         */
  /* Create              */ TPICreate,
  /* Open                */ TPIOpen,
  /* Overwrite           */ nullptr,
  /* Cleanup             */ nullptr,
  /* Close               */ TPIClose,
  /* Read                */ TPIRead,
  /* Write               */ TPIWrite,
  /* Flush               */ TPIFlush,
  /* GetFileInfo         */ TPIGetInfo,
  /* SetBasicInfo        */ nullptr,
  /* SetFileSize         */ nullptr,
  /* Rename              */ nullptr,
  /* GetSecurity2        */ nullptr,
  /* SetSecurity2        */ nullptr,
  /* CreateEx            */ nullptr,
  /* OverwriteEx         */ nullptr,
  /* GetDirInfoByName    */ nullptr,
  /* ResolveReparsePoints */ nullptr,
  /* GetReparsePoint    */ nullptr,
  /* SetReparsePoint    */ nullptr,
  /* DeleteReparsePoint */ nullptr,
  /* EnumDirectory      */ TPIEnumDirectory,
  /* StreamInfo         */ nullptr,
  /* GetDirInfoByName2  */ nullptr,
  /* Control             */ nullptr,
};

}  // namespace wdl
