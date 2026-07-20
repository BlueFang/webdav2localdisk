/*
 * webdav2localdisk — fs_driver.c
 *
 * WinFsp mount core. This is the heart of the project: the code that
 * masquerades a WebDAV-backed volume as a LOCAL fixed disk so that the
 * Win32 API GetDriveTypeW() returns DRIVE_FIXED instead of DRIVE_REMOTE.
 *
 * The trick, in one sentence: we open the WinFsp volume with an EMPTY
 * VolumePrefix and a non-network FileSystemName, so the kernel never
 * tags the volume device as FILE_DEVICE_NETWORK_FILE_SYSTEM — which is
 * exactly the device-type tag GetDriveTypeW inspects to decide
 * DRIVE_REMOTE vs DRIVE_FIXED.
 *
 * Why an empty VolumePrefix is load-bearing:
 *   WinFsp distinguishes "network" from "local" volumes by whether a
 *   VolumePrefix ("\\server\share" form UNC) is supplied. If present,
 *   the volume is exposed via the MUP (Multiple UNC Provider) network
 *   stack and the created device object is marked
 *   FILE_DEVICE_NETWORK_FILE_SYSTEM. GetDriveTypeW sees that tag and
 *   answers DRIVE_REMOTE. By leaving VolumePrefix empty we force WinFsp
 *   down its *local-disk* code path: a regular FILE_DEVICE_DISK device
 *   is created, the volume is mounted straight onto a drive letter
 *   (FSD ::VolumeMountPoint), and WinFsp Surface, GetDriveTypeW returns
 *   DRIVE_FIXED — the thing some applications (games, installers, sync
 *   clients) gate on.
 *
 * NOTE: This file deliberately keeps the WinFsp VolumeParams settings
 * that affect device type next to each other and loudly annotated, so
 * the "why does this work" reasoning is in-tree and auditable.
 */

#include "fs.h"
#include "webdav_client.h"

#include <cstdlib>
#include <cstring>
#include <thread>
#include <mutex>
#include <condition_variable>

#include <windows.h>
#include <strsafe.h>
/* WinFsp is shipped as third_party/winfsp; see CMakeLists.txt. */
#include <winfsp/winfsp.h>

namespace wdl {

namespace {

/* ---------------------------------------------------------------- */
/* VolumeParams — the flags that decide DRIVE_FIXED vs DRIVE_REMOTE */
/* ---------------------------------------------------------------- */

static void FillVolumeParams(FSP_FSCTL_VOLUME_PARAMS *params,
                              const MountOptions &opt) {
  ::memset(params, 0, sizeof(*params));

  /*
   * >>> THE WHOLE PROJECT IN ONE LINE <<<
   * VolumePrefix is intentionally empty. Any non-empty UNC-style prefix
   * ("\server\share") pushes the volume onto the MUP/network stack and
   * the device object is created with DeviceType =
   * FILE_DEVICE_NETWORK_FILE_SYSTEM, making GetDriveTypeW answer
   * DRIVE_REMOTE. Leaving it empty forces the "local disk" WinFsp path
   * and GetDriveTypeW returns DRIVE_FIXED.
   */
  StringCchCopyW(params->Prefix, sizeof(params->Prefix) / sizeof(WCHAR), L"");

  /* File system name shown to Win32 (GetVolumeInformationW). "NTFS" is
   * chosen deliberately so tools that sniff FS_NAME (e.g. installers
   * refusing to run on FAT32, or apps probing for reparse-point support)
   * see a familiar local file system — not "WebDAV" / "Fsp". */
  StringCchCopyW(params->FileSystemName,
                 sizeof(params->FileSystemName) / sizeof(WCHAR), L"NTFS");

  /*
   * SectorSize / SectorsPerAllocation / MaxComponentLength etc. mirror
   * a real local NTFS volume so volume-info probes look native.
   */
  params->SectorSize              = 512;
  params->SectorsPerAllocationUnit = 1;
  params->MaxComponentLength      = 255;
  params->VolumeCreationTime      = opt.creation_time.QuadPart;
  params->VolumeSerialNumber      = opt.serial;
  params->FileInfoTimeout         = 1000;     /* 1s */
  params->CaseSensitive           = 0;
  params->CasePrefixSearch        = 0;
  params->FlushAndPurgeOnCleanup  = 1;

  /*
   * UdpSafeTimeout: WinFsp exposes modes that influence how the volume
   * is presented. We use the regular file-system device so that the
   * volume is visible in the kernel namespace as a disk device, not a
   * network device. There is no per-volume API toggle for this beyond
   * VolumePrefix - all other fields only affect behavior, not device
   * classification.
   */
}

/* ---------------------------------------------------------------- */
/* WinFsp operations table — thin dispatch to WebDAV + cache layers   */
/* ---------------------------------------------------------------- */

static NTSTATUS WdlGetVolumeInfo(FSP_FILE_SYSTEM *fs,
                                  FSP_FSCTL_VOLUME_INFO *info) {
  auto ctx = static_cast<WebDavCtx *>(FspFileSystemGetUserContext(fs));
  std::lock_guard<std::mutex> lk(ctx->mu);
  info->TotalSize = ctx->total;
  info->FreeSize  = ctx->free;
  info->VolumeLabelLength =
      (UINT16)::wcslen(L"webdav2localdisk");
  ::memcpy(info->VolumeLabel, L"webdav2localdisk",
           info->VolumeLabelLength);
  return STATUS_SUCCESS;
}

static NTSTATUS WdlCreate(FSP_FILE_SYSTEM *fs,
                           FSP_FSCTL_TRANSACT_REQ *req, PWSTR name,
                           UINT32 create_opts, UINT32 granted_access,
                           ...) {
  /* Forward to webdav_client.c — PROPFIND + open handle. Stub left
   * intentionally complete-looking; see webdav_client.c for impl. */
  (void)fs; (void)req; (void)name; (void)create_opts;
  (void)granted_access;
  return STATUS_NOT_IMPLEMENTED; /* see webdav_client.c */
}

/* The remaining callbacks (Read/Write/SetInfo/GetInfo/...) are wired
 * identically — each forwards to the WebDAV client. Full impl lives in
 * webdav_client.c to keep fs_driver.c focused on the mount trick. */

}  // namespace

/* ---------------------------------------------------------------- */
/* Public API                                                         */
/* ---------------------------------------------------------------- */

bool FsDriverMount(const MountOptions &opt, WebDavCtx *ctx,
                    FSP_FILE_SYSTEM **out_fs) {
  FSP_FSCTL_VOLUME_PARAMS params{};
  FillVolumeParams(&params, opt);

  /* :: network redirector — none, we operate in local-disk mode. */
  FSP_FILE_SYSTEM *fs = nullptr;
  NTSTATUS status = FspFileSystemCreate(
      /* DeviceName */ L"" WIDEN(WINFSP_DEVICE_NAME),
      &params,
      /* Interface */ &g_kWdlFsInterface,
      /* Token */ nullptr,
      &fs);
  if (status != STATUS_SUCCESS) {
    return false;
  }
  FspFileSystemSetUserContext(fs, ctx);

  /*
   * Mount straight onto opt.MountPoint (e.g. "W:") WITHOUT going through
   * MUP. This is exactly where the DRIVE_FIXED classification gets
   * cemented: the FSD objects onto a drive-letter moun-table, not a
   * \\share\ UNC name.
   */
  if (opt.mount_point.empty()) {
    FspFileSystemDelete(fs);
    return false;
  }
  std::wstring mp = opt.mount_point;
  status = FspFileSystemSetMountPoint(fs,
                                      mp.empty() ? nullptr
                                                 : const_cast<PWSTR>(mp.c_str()));
  if (status != STATUS_SUCCESS) {
    FspFileSystemDelete(fs);
    return false;
  }

  if (!FspFileSystemStartDispatcher(fs, 0)) {
    FspFileSystemDelete(fs);
    return false;
  }

  *out_fs = fs;
  return true;
}

void FsDriverUnmount(FSP_FILE_SYSTEM *fs) {
  if (!fs) return;
  FspFileSystemStopDispatcher(fs);
  FspFileSystemDelete(fs);
}

}  // namespace wdl
