/*
 * webdav2localdisk — fs_driver.cpp
 *
 * WinFsp mount core. Leans on webdav_client.cpp for the full callback table.
 *
 * >>> THE WHOLE PROJECT IN ONE LINE <<<
 *
 *   VolumeParams.Prefix = L""  (empty, no UNC prefix)
 *
 * An empty VolumePrefix forces WinFsp down its LOCAL-DISK code path.
 * The NT device object is created with DeviceType = FILE_DEVICE_DISK
 * (not FILE_DEVICE_NETWORK_FILE_SYSTEM), so Win32 GetDriveTypeW()
 * answers DRIVE_FIXED (3), not DRIVE_REMOTE (4).
 *
 * Any non-empty UNC prefix would push the volume through MUP and flip
 * device-type to network. This is the whole project's identity.
 * See docs/PRINCIPLE.md.
 */
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define WIN32_NO_STATUS
#include <windows.h>
#undef WIN32_NO_STATUS
#include <winternl.h>
#include <ntstatus.h>
/* older WinFsp + newer Windows SDK: PNTSTATUS may be absent from winternl.h */
#ifndef PNTSTATUS
typedef NTSTATUS *PNTSTATUS;
#endif
#include <strsafe.h>
#include <winfsp/winfsp.h>
#include <winfsp/fsctl.h>

#include "fs.h"
#include "webdav_client.h"

namespace wdl {

extern "C" FSP_FILE_SYSTEM_INTERFACE g_kWdlFsInterface;

bool FsDriverMount(const MountOptions &opt, WebDavCtx *cfg,
                   void **out_fs_handle) {
  auto *rt = DavRuntimeCreate(cfg);
  if (!rt) return false;

  FSP_FSCTL_VOLUME_PARAMS p{}; ::memset(&p, 0, sizeof(p));

  /* ▶ THE LOAD-BEARING LINE: empty Prefix → local disk → DRIVE_FIXED ◀ */
  ::StringCchCopyW(p.Prefix, ARRAYSIZE(p.Prefix), L"");

  ::StringCchCopyW(p.FileSystemName, ARRAYSIZE(p.FileSystemName), L"NTFS");

  p.SectorSize              = 512;
  p.SectorsPerAllocationUnit = 1;
  p.MaxComponentLength      = 255;
  p.VolumeCreationTime      = cfg->creation_time ? cfg->creation_time
                                                  : opt.creation_time;
  p.VolumeSerialNumber      = cfg->serial;
  p.FileInfoTimeout         = 1000;
  p.FlushAndPurgeOnCleanup  = 1;

  FSP_FILE_SYSTEM *fs = nullptr;
  /* FSP_FSCTL_DISK_DEVICE_NAME expands to "WinFsp.Disk" (narrow).
   *  Use the wide literal directly for FspFileSystemCreate. */
  NTSTATUS st = ::FspFileSystemCreate(const_cast<PWSTR>(L"WinFsp.Disk"),
                                      &p, &g_kWdlFsInterface, &fs);
  if (st != STATUS_SUCCESS) { DavRuntimeDestroy(rt); return false; }

  fs->UserContext = rt;

  if (opt.mount_point.empty()) {
    ::FspFileSystemDelete(fs); DavRuntimeDestroy(rt); return false;
  }
  st = ::FspFileSystemSetMountPoint(
      fs, const_cast<PWSTR>(opt.mount_point.c_str()));
  if (st != STATUS_SUCCESS) {
    ::FspFileSystemDelete(fs); DavRuntimeDestroy(rt); return false;
  }
  if (!::FspFileSystemStartDispatcher(fs, 0)) {
    ::FspFileSystemDelete(fs); DavRuntimeDestroy(rt); return false;
  }
  *out_fs_handle = fs;
  return true;
}

void FsDriverUnmount(void *fs_handle) {
  if (!fs_handle) return;
  auto *fs = static_cast<FSP_FILE_SYSTEM *>(fs_handle);
  auto *rt = static_cast<DavRuntime *>(fs->UserContext);
  ::FspFileSystemStopDispatcher(fs);
  ::FspFileSystemDelete(fs);
  DavRuntimeDestroy(rt);
}

}  // namespace wdl
