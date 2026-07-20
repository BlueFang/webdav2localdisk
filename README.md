# webdav2localdisk

> Map a WebDAV server to a local Windows drive that reports **`DRIVE_FIXED`**
> to `GetDriveType()` — masquerade remote WebDAV storage as a local fixed
> disk at the kernel volume-device level.

[中文](./README_zh.md) | English

---

## Why?

Some Windows applications refuse to run on — or even list — "network"
drives. They call `GetDriveTypeW("X:\\")` and bail out unless the answer
is `DRIVE_FIXED` (3). Examples:

- Games that store profile/save data on a "local" drive
- Installers that won't install onto network paths
- Drive-indexing tools, sync clients, performance counters
- Backup software that skips `DRIVE_REMOTE`

Cloud WebDAV providers (your NAS, Nextcloud, Synology Drive's WebDAV,
`rclone serve webdav`, …) are perfectly fine for this — *if* the volume
isn't labeled as "remote". `webdav2localdisk` does exactly that labeling
trick (see [PRINCIPLE.md](./docs/PRINCIPLE.md)) and wires it to a working
WebDAV client with a simple GUI.

## Screenshot

```
┌─────────────────────────────────────────┐
│  webdav2localdisk          — □ ✕        │
├─────────────────────────────────────────┤
│  WebDAV Connection                      │
│  URL: [https://dav.example.com/dav    ] │
│  User: [alice          ] Pass: [*****] │
│  Drive: [W: (free) ▼]  Cache: [......] │
│        [ Mount ] [ Unmount ] [ Test  ]  │
│  Status: Mounted on W: (DRIVE_FIXED)    │
├─────────────────────────────────────────┤
│  [OK] W: mounted — DRIVE_FIXED.        │
│  GetDriveTypeW(W:\) == 3 (FIXED)       │
│  ✅  PASS — The trick works!            │
└─────────────────────────────────────────┘
```

## Build

Requirements:

- Windows 10/11 x64
- CMake ≥ 3.21, Visual Studio 2022 (MSVC)
- Qt 6.7+ (Widgets module)
- [WinFsp 2023+](https://winfsp.dev/) (developer SDK + redistributable)

```bat
git clone https://github.com/BlueFang/webdav2localdisk.git
cd webdav2localdisk
cmake -B build -S .
cmake --build build --config Release
```

💡 GitHub Actions auto-builds each push — download a ready-to-run exe from
the **Actions** tab under the latest `Build (Windows)` workflow artifact.

## How it works (TL;DR)

The whole project hinges on one line in `fs_driver.cpp`:

```c
StringCchCopyW(params.Prefix, ..., L"");   // empty = local-disk mode
```

When `FspFileSystemCreate` is called with an empty `VolumePrefix`, WinFsp
takes its **local-disk** code path: the device object is created with
`DeviceType = FILE_DEVICE_DISK`. `GetDriveTypeW` inspects that exact
field and answers `DRIVE_FIXED` (3).

With any non-empty UNC prefix ("`\\server\share`"), WinFsp routes through
MUP and the device becomes `FILE_DEVICE_NETWORK_FILE_SYSTEM`, answering
`DRIVE_REMOTE` (4). That's what all standard WebDAV mounts produce — and
exactly why this project exists.

Full walkthrough in **[`docs/PRINCIPLE.md`](./docs/PRINCIPLE.md)**.

## Status / Roadmap

- [x] **DRIVE_FIXED trick** — VolumePrefix="" in fs_driver.cpp
- [x] **Full WebDAV implementation** — PROPFIND / GET / PUT / MKCOL / MOVE / DELETE
- [x] **Qt GUI** — fill form, mount/unmount, one-click `GetDriveType` test
- [x] **In-memory directory cache** — avoids repeated PROPFIND calls
- [x] **GitHub Actions CI** — auto-builds exe on push
- [ ] On-disk file cache (currently in-memory only)
- [ ] Digest / Bearer auth (currently Basic only)
- [ ] Inno Setup installer with WinFsp bundling
- [ ] CI x86 target

## License

MIT — see [`LICENSE`](./LICENSE).
