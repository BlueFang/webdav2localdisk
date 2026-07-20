/*
 * webdav2localdisk — fs.h
 *
 * Internal types shared between the WinFsp mount core (fs_driver.cpp)
 * and the WebDAV protocol client (webdav_client.cpp).
 */
#ifndef WDL_FS_H_
#define WDL_FS_H_

#include <atomic>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include <windows.h>

namespace wdl {

struct WebDavFile {
  std::string  name;          /* relative path, POSIX-style separators */
  bool         is_dir = false;
  uint64_t     size  = 0;
  uint64_t     mtime = 0;     /* Unix epoch seconds (converted to NT up front) */
  uint32_t     attrs = 0;
};

struct WebDavCtx {
  /* --- WebDAV endpoint ----------------------------------------------- */
  std::wstring  server_url;            /* e.g. https://dav.example.com/dav */
  std::wstring  username;
  std::wstring  password;

  /* --- Reported volume size (queried from server once on mount) ----- */
  uint64_t      total = 1ull << 40;     /* 1 TiB default phantom value */
  uint64_t      free  = 1ull << 39;

  /* --- Local on-disk cache root ------------------------------------- */
  std::string   cache_dir;              /* empty => disabled */

  /* --- Volume serial shown to GetVolumeInformationW ----------------- */
  uint32_t      serial   = 0x2D774C44;  /* 'wD2L' little-endian */
  std::wstring  label    = L"webdav2localdisk";

  /* --- Runtime state, owned by webdav_client.cpp -------------------- */
  std::mutex    mu;
  uint64_t      creation_time = 0;     /* FILETIME epoch (hecto-ns) */
};

struct MountOptions {
  std::wstring mount_point;            /* e.g. L"W:" */
  uint64_t     creation_time = 0;
};

bool FsDriverMount(const MountOptions &opt, WebDavCtx *ctx,
                   void **out_fs_handle);
void FsDriverUnmount(void *fs_handle);

}  // namespace wdl

#endif  // WDL_FS_H_
