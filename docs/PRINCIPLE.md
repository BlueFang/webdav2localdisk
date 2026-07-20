# The `GetDriveType` → `DRIVE_FIXED` trick — full principle

This document is the "why does this work". The README tells you what
the project *does*; this file explains the single load-bearing line in
`fs_driver.cpp` and the kernel mechanics behind it. If you are going to
contribute, or try to replicate the trick somewhere else, read this
first.

## The Win32 surface

`GetDriveTypeW(L"X:\\")` — the function we are trying to fool — is a
thin shim living in `kernel32.dll`. Its decision tree:

1. Normalise the root path (e.g. `X:\` → `\Device\HarddiskVolumeN` or
   `\Device\Mup\...` via `RtlDosPathNameToNtPathName_U`).
2. Open the root directory of that volume.
3. Call `NtQueryVolumeInformationFile(FileFsDeviceInformation, …)`.
4. Read the `FILE_FS_DEVICE_INFORMATION.DeviceType` field.
5. Map it:

   | DeviceType                                   | GetDriveType result     |
   |---------------------------------------------|-------------------------|
   | `FILE_DEVICE_DISK`                          | `DRIVE_FIXED`   (3)     |
   | `FILE_DEVICE_CD_ROM`                        | `DRIVE_CDROM`   (5)     |
   | `FILE_DEVICE_NETWORK_FILE_SYSTEM`           | `DRIVE_REMOTE`  (4)     |
   | (anything else / open fails)                | `DRIVE_NO_ROOT_DIR`(1) |

So the entire game is "make sure the volume's device object reports
`FILE_DEVICE_DISK`, not `FILE_DEVICE_NETWORK_FILE_SYSTEM`".

## WinFsp's device-type decision

WinFsp runs as a user-mode file system. When your user-mode FS calls
`FspFileSystemCreate`, WinFsp creates one of **two kinds** of NT device
objects:

- **Local-disk** device — device type `FILE_DEVICE_DISK`.
- **Network** device — device type `FILE_DEVICE_NETWORK_FILE_SYSTEM`,
  registered with MUP (Multiple UNC Provider) so `\Device\WinFsp\Mup`
  paths route here.

The branch WinFsp picks depends on **one** piece of input: the
`VolumePrefix` field of `FSP_FSCTL_VOLUME_PARAMS`.

From `winfsp/src/dll/fs.c` (paraphrased):

```c
if (L'\0' == VolumeParams->Prefix[0]) {
    /* Local-disk mode. Create a /Device/WinFsp/<VolumeName> disk
     * device and mount it on the requested MountPoint through the
     * regular FSD volume mounting path. DeviceType =
     * FILE_DEVICE_DISK. */
    ...
} else {
    /* Network mode. The Prefix is a UNC name like \server\share.
     * Register with MUP; VolumePrefix becomes the UNC path; device
     * type becomes FILE_DEVICE_NETWORK_FILE_SYSTEM. */
    ...
}
```

That is the single API-honoured branch point. There are ~30 other
fields in `FSP_FSCTL_VOLUME_PARAMS`, but none of them flip device type.
Only `Prefix`.

## Why "empty prefix" wins

So the project's trick is:

```c
StringCchCopyW(params->Prefix, /* size */, L"");    // ← empty
```

This sends WinFsp down its local-disk branch:

1. WinFsp creates `\\Device\\WinFsp\\{VolumeGuid}` with
   `DeviceType = FILE_DEVICE_DISK`.
2. WinFsp mounts that device onto the drive letter you supplied via
   `FspFileSystemSetMountPoint("W:")`.
3. When Win32 calls `GetDriveTypeW(L"W:\\")`, the device-type probe hits
   `FILE_DEVICE_DISK` ⇒ returns `DRIVE_FIXED`.

There is no registry hack, no device-driver patching, no hooking of
`kernel32`. It is purely “fill the prefix correctly on the way in.”
Nothing to maintain against Windows updates; the only contract is that
WinFsp will keep treating empty `Prefix` as local-disk-mode.

## Other fields we still get right (for cosmetic consistency)

The empty prefix alone delivers `DRIVE_FIXED`. But other Win32 probes
(`GetVolumeInformationW`, `DeviceIoControl(IOCTL_STORAGE_*)`, etc.) may
further inspect the volume, so we set:

- `FileSystemName = L"NTFS"` so `GetVolumeInformationW`'s `lpFileSystemName`
  buffer reports "NTFS" — keeping apps that sniffs FS name happy.
- `SectorSize = 512`, `SectorsPerAllocationUnit = 1`,
  `MaxComponentLength = 255` — same as a 4 KiB-cluster NTFS.
- `VolumeLabel = "webdav2localdisk"`, but you can override this from CLI.
- Volume serial is a fixed (`'wD2L'` little-endian) value; patchable.

These are frosting; the cake is the empty prefix.

## What changes if you DON'T do the trick

If we instead set, say, `Prefix = L"\\webdav2localdisk\\share"`:

1. WinFsp routes through MUP, the device type becomes
   `FILE_DEVICE_NETWORK_FILE_SYSTEM`.
2. `GetDriveTypeW` ⇒ `DRIVE_REMOTE`.
3. The volume also appears in `\Device\Mup`, files are addressed as
   `\\webdav2localdisk\\share\…`, which is literally UNC again.

That is essentially what `rclone mount` and the native Windows "Map
Network Drive" UI produce — and exactly why this project exists.

## Cross-checks

Things that *also* flip to "local-disk behaviour" as a side effect of the
empty-prefix trick, but do not themselves drive the decision:

- `fsutil fsinfo drives` lists the volume under the drive letter.
- `wmic logicaldisk get name,drivetype` returns `3` (Local Disk).
- `IOCTL_STORAGE_GET_DEVICE_NUMBER` and
  `IOCTL_DISK_GET_LENGTH_INFO` return real-looking answers because
  WinFsp implements the storage-class IOCTLs in its local-disk branch
  and not in its MUP branch.
- `QueryDosDeviceW("W:")` returns `\Device\WinFsp\{VolumeGuid}` — the
  signature of a regular local disk, not `\Device\Mup\…`.

## Caveats

- **SCSI/ATA inquiry IOCTLs** (`IOCTL_SCSI_PASS_THROUGH` etc.) will fail
  with `STATUS_INVALID_DEVICE_REQUEST`, since there is no actual SCSI
  device behind the volume. Forensic/drive-health tools relying on those
  will detect the volume is virtual. This is **intended**: the project's
  goal is label-fidelity, not hardware deception.
- **Latency** of every operation still reflects the WebDAV server's
  latency. The volume *says* "local disk" but *behaves* like a WAN drive.
  Use the cache layer if you need warm read paths.
- **No denial of host OS observability**: `fltmc`, Process Explorer's
  "Filter Drivers" tab will show `\Driver\WinFsp`. The volume is a real
  WinFsp volume; the empty-prefix trick only flips the
  *volume* device-type bit, it doesn't pretend WinFsp doesn't exist.

## References

- WinFsp source, `src/dll/fs.c::FspFileSystemCreate` — the branch.
- Windows DDK headers, `FILE_FS_DEVICE_INFORMATION`,
  `IOCTL_STORAGE_GET_DEVICE_NUMBER` — what Win32 probes.
- Win32 `kernelbase.dll::GetDriveTypeW` — the API surface consumers see.
- Microsoft docs: [GetDriveTypeW](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-getdrivetypew),
  [FSP_FSCTL_VOLUME_PARAMS](https://winfsp.dev/apidoc/struct_f_s_p___f_s_c_t_l___v_o_l_u_m_e___p_a_r_a_m_s.html).
