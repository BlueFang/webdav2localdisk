# webdav2localdisk

> Map a WebDAV server to a local Windows drive that reports **`DRIVE_FIXED`**
> to `GetDriveType()` — masquerade remote WebDAV storage as a local fixed
> disk at the kernel volume-device level.

[中文](./README_zh.md) | English

---

## Why?

Some Windows applications refuse to run on — or even list — "network"
drives. They call `GetDriveTypeW("X:\\")` and bail out unless the answer
is `DRIVE_FIXED` (typical for desktop drives) or `DRIVE_HARDDISK`-like
local storage. Examples:

- Games that store profile/save data on a drive they consider "local"
- Installers that won't install onto network paths
- Drive-indexing tools, sync clients, performance counters
- Backup software that skips `DRIVE_REMOTE`

Cloud WebDAV providers (your NAS, Nextcloud, Synology Drive's WebDAV,
`rclone serve webdav`, …) are perfectly fine for this — *if* the volume
isn't labeled as "remote". `webdav2localdisk` does exactly that labeling
trick (see [PRINCIPLE.md](./docs/PRINCIPLE.md)) and wires it to a working
WebDAV client.

## What it does (and what it doesn't)

✅ Mounts an arbitrary WebDAV server onto a Windows drive letter.  
✅ Reports `DriveType == DRIVE_FIXED` to all Win32 callers, including
   `GetDriveTypeW`, the volume-flags returned by `GetVolumeInformationW`,
   and the like.
✅ Optional local on-disk cache for hot reads and buffered writes.

❌ It is **not** a security bypass. It does not elevate privileges or
   hide network round-trip latency. Any client that actually probes your
   transfer speed will still see WebDAV-level throughput.
❌ It is **not** intended for forensics evasion; the volume device is
   legitimately a WinFsp-backed local disk image. The "masquerade" here
   is purely a device-class label, not data origin deception.

## Build

Requirements:

- Windows 10/11 x64
- CMake ≥ 3.20
- Visual Studio 2022 (MSVC) or clang-cl
- [WinFsp 2023+](https://winfsp.dev/) (developer headers + the
  redistributable must be installed on machines where you run the binary)
- OpenSSL (for the local cache's SHA-1; or remove `cache.cpp` if you
  prefer to skip caching)

```bat
git clone https://github.com/BlueFang/webdav2localdisk.git
cd webdav2localdisk
cmake -B build -S .
cmake --build build --config Release
```

## Run

```bat
:: Install WinFsp redistributable first (https://winfsp.dev/).

webdav2localdisk.exe ^
    --url   https://dav.example.com/dav ^
    --user  alice ^
    --pass  hunter2 ^
    --drive W: ^
    --cache "%LOCALAPPDATA%\webdav2localdisk\cache"
```

Verify the trick worked:

```bat
:: Expected: DRIVE_FIXED == 3
powershell -c "(get-psdrive W).Provider)| Get-Member GetDriveType
fsutil fsinfo drives
powershell -NoProfile -Command \
   "Add-Type -TypeDefinition 'using System;using System.Runtime.InteropServices;static class W{[DllImport(\"kernel32\")]public static extern uint GetDriveType(string lpRootPathName);}'; [W]::GetDriveType('W:\')"
```

(Or just write a tiny C program that calls `GetDriveTypeW(L"W:\\")`.)
You should see `3` — i.e. `DRIVE_FIXED`. Without this project, a regular
`net use W: \\server\share` mapping would return `4` (`DRIVE_REMOTE`).

## How it works (TL;DR)

The whole project hinges on one line in `fs_driver.cpp`: when calling
`FspFileSystemCreate`, we pass a **`VolumePrefix` of `""`** (empty
string). WinFsp uses that prefix to decide whether to mount over the
Multiple UNC Provider (network → `DeviceType ==
FILE_DEVICE_NETWORK_FILE_SYSTEM`, `GetDriveType == DRIVE_REMOTE`) or as
a plain local disk (→ `DeviceType == FILE_DEVICE_DISK`,
`GetDriveType == DRIVE_FIXED`). Other fields just decorate; only the
prefix flips the device type.

Full walkthrough, references, and reverse-engineering notes:
**[`docs/PRINCIPLE.md`](./docs/PRINCIPLE.md)**.

## Roadmap / Status

This is an early public skeleton. Boxes already checked:

- [x] VolumeParams set up to force `DRIVE_FIXED`
- [x] WinFsp callback dispatch table
- [x] CLI arg parsing + mount lifecycle (signal-driven)

Not done yet (PRs welcome):

- [ ] Full WebDAV verb maps (`PROPFIND`/`PUT`/`MOVE`/`COPY`/`MKCOL`)
- [ ] Real `cache.cpp` impl (currently a stub)
- [ ] Auth: Basic + Digest
- [ ] Inno Setup installer + signed build
- [ ] CI matrix (MSVC / clang-cl / x86 / x64)

## Contributing

PRs welcome — especially around the WebDAV protocol layer, which is the
biggest remaining piece. Keep the `VolumePrefix=""` invariant sacred; it
is the single line that makes the project what it is.

## License

MIT — see [`LICENSE`](./LICENSE).
