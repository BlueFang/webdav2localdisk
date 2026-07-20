/*
 * webdav2localdisk — fs.h
 *
 * Public surfaces shared between the WinFsp mount core (fs_driver.c)
 * and the WebDAV protocol client (webdav_client.c). Internal header;
 * do not ship as part of a public API.
 */
#ifndef WDL_FS_H_
#define WDL_FS_H_

#include <cstdint>
#include <string>
#include <mutex>

#include <windows.h>

namespace wdl {

struct WebDavCtx {
  /* WebDAV endpoint */
  std::wstring  server_url;
  std::wstring  username;
  std::wstring  password;

  /* Reported volume space (bytes). */
  std::uint64_t total = 1ull << 40; /* 1 TiB default phantom value */
  std::uint64_t free  = 1ull << 39;

  /* Local cache root, if any. Empty => uncached (works but slower). */
  std::string   cache_dir;

  /* Serial for the volume (GetVolumeInformationW). */
  std::uint32_t serial = 0x2D774C44; /* 'wD2L' in little-endian */

  std::mutex    mu;   /* guards members above from concurrent transacts */
};

struct MountOptions {
  /* Drive letter to mount on, e.g. "W:". Must be free & untaken. */
  std::wstring mount_point;

  /* VolumeCreationTime (FILETIME epoch). 0 => "right now" on mount. */
  LARGE_INTEGER creation_time{0};
};

/* Drives-device-type the volume reports.Implemented in fs_driver.c. */
bool FsDriverMount(const MountOptions &opt, WebDavCtx *ctx,
                   FSP_FILE_SYSTEM **out_fs);
void FsDriverUnmount(FSP_FILE_SYSTEM *fs);

}  // namespace wdl

#endif  // WDL_FS_H_
