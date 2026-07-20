/*
 * webdav2localdisk — cache.h
 *
 * Local on-disk cache for WebDAV-backed file objects.
 */
#ifndef WDL_CACHE_H_
#define WDL_CACHE_H_

#include <cstdint>
#include <string>

#include <windows.h>
#include <winfsp/winfsp.h>

namespace wdl {

class Cache {
 public:
  Cache(std::string path, std::string key);

  /* Read `len` bytes from offset `off` of the cached blob into `buf`.
   * Sets `*got` to the actually-read byte count. Returns success.
   * Returns false if the blob is absent on disk and no caller has
   * fetched it upstream yet (webdav_client.c should fetch then retry). */
  bool Read(UINT64 off, PVOID buf, ULONG len, PULONG got);

  /* Writes `len` bytes from `buf` at offset `off` into local blob.
   * Creates blob on first write. Marks blob dirty so FlushDisk will
   * push it upstream via WebDAV PUT. */
  bool Write(UINT64 off, PVOID buf, ULONG len);

  /* Flush all dirty data to the WebDAV server. Best-effort. */
  bool FlushDisk();

  void Info(FSP_FSCTL_FILE_INFO *info);

 private:
  std::string path_;             /* cache_dir/blobs/<hex> path */
  std::string key_;              /* logical path                 */
  std::string blob_path_;        /* resolved on-disk path        */
  bool        dirty_ = false;
};

}  // namespace wdl

#endif  // WDL_CACHE_H_
