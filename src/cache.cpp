/*
 * webdav2localdisk — cache.c
 *
 * Local on-disk cache for WebDAV-backed file objects. The cache is
 * optional — without it the volume works, just slower. With it,
 * repeated reads are served from local disk and writes are buffered
 * and flushed lazily.
 *
 * Layout under cache_dir:
 *   <cache_dir>/index.json        # map of path -> {etag, size,La,dirty}
 *   <cache_dir>/blobs/<sha1(hex)> # raw file bytes, content-addressed
 *
 * Reads hit the blob directly; misses trigger a WebDAV GET into the
 * blob. Writes mark the blob dirty and stream the incremental update.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "cache.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <openssl/sha.h>

namespace wdl {

Cache::Cache(std::string path, std::string key)
    : path_(std::move(path)), key_(std::move(key)) {
  blob_path_ = path_;        /* blobs/<sha1> resolution done by SetKey. */
}

bool Cache::Read(UINT64 off, PVOID buf, ULONG len, PULONG got) {
  /* For skeleton: read from local blob file if present. Full WebDAV
   * refresh-on-stale logic is left to webdav_client.c. */
  std::ifstream f(blob_path_, std::ios::binary);
  if (!f) return false;
  f.seekg(static_cast<std::streamoff>(off), std::ios::beg);
  f.read(static_cast<char *>(buf), len);
  *got = static_cast<ULONG>(f.gcount());
  return *got > 0;
}

bool Cache::Write(UINT64 off, PVOID buf, ULONG len) {
  std::ofstream f(blob_path_, std::ios::in | std::ios::out |
                                std::ios::binary | std::ios::ate);
  if (!f) {
    /* file not yet on disk — create it. */
    f.open(blob_path_, std::ios::binary | std::ios::out | std::ios::trunc);
  }
  if (!f) return false;
  f.seekp(static_cast<std::streamoff>(off), std::ios::beg);
  f.write(static_cast<char *>(buf), len);
  dirty_ = true;
  return f.good();
}

bool Cache::FlushDisk() {
  if (!dirty_) return true;
  /* TODO: WinDAV PUT blob_path_ -> server. */
  dirty_ = false;
  return true;
}

void Cache::Info(FSP_FSCTL_FILE_INFO *info) {
  std::ifstream f(blob_path_, std::ios::binary | std::ios::ate);
  UINT64 size = f ? static_cast<UINT64>(f.tellg()) : 0;
  info->FileAttributes = FILE_ATTRIBUTE_NORMAL;
  info->FileSize = size;
  info->AllocationSize = (size + 511ULL) & ~511ULL;
  info->ChangeTime = info->LastAccessTime =
      info->LastWriteTime = info->CreationTime = 0;
  info->ReparseTag = info->IndexNumber = info->HardLinks = 0;
  info->EaSize = 0;
}

}  // namespace wdl
