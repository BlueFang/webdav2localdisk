# webdav2localdisk

> 将 WebDAV 服务映射为本地 Windows 驱动器，`GetDriveType()` 返回 **`DRIVE_FIXED`**，
> 在内核卷设备层将远程存储伪装成本地固定磁盘。

[English](./README.md) | 中文

---

## 为什么需要它？

一些 Windows 程序会拒绝在"网络驱动器"上运行 — 它们调用 `GetDriveTypeW("X:\\")`，
只要返回不是 `DRIVE_FIXED`(3) 就直接退出。典型场景：

- 存档必须写在"本地盘"上的游戏
- 拒绝安装到网络路径的安装器
- 索引/同步/性能工具
- 自动跳过 `DRIVE_REMOTE` 的备份软件

云 WebDAV（你的 NAS、Nextcloud、Synology Drive 的 WebDAV、`rclone serve webdav` 等）
完全能承载这些数据——只要卷不被标成"remote"。本项目做的就是：用 WinFsp 空 VolumePrefix
的 trick，让内核报告 `DRIVE_FIXED`，上面跑一个真正可用的 WebDAV 客户端 + GUI。

## 截图

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

## 编译

依赖：

- Windows 10/11 x64
- CMake ≥ 3.21, Visual Studio 2022 (MSVC)
- Qt 6.7+ (Widgets 模块)
- [WinFsp 2023+](https://winfsp.dev/) (开发者 SDK + 可再发行包)

```bat
git clone https://github.com/BlueFang/webdav2localdisk.git
cd webdav2localdisk
cmake -B build -S .
cmake --build build --config Release
```

💡 GitHub Actions 每次 push 自动构建，到 **Actions** 页下最新的
`Build (Windows)` workflow 里下载 artifact 即拿即用。

## 原理（一句话版）

命门在 `fs_driver.cpp` 里：

```c
StringCchCopyW(params.Prefix, ..., L"");   // 空前缀 = 本地盘模式
```

`FspFileSystemCreate` 被调用时，`VolumePrefix` 为空 → WinFsp 走**本地盘**路径，
创建的 device object 类型为 `FILE_DEVICE_DISK`。`GetDriveTypeW` 检查的正好是这个字段
→ 返回 `DRIVE_FIXED`(3)。

如果前缀非空（比如 `"\\server\share"`），WinFsp 走 MUP 网络栈，device 类型变成
`FILE_DEVICE_NETWORK_FILE_SYSTEM` → `GetDriveTypeW` 返回 `DRIVE_REMOTE`(4)。
这就是所有常规 WebDAV 挂载的结果，也是本项目存在的理由。

完整推导见 **[`docs/PRINCIPLE.md`](./docs/PRINCIPLE.md)**。

## 状态 / 路线图

- [x] **DRIVE_FIXED 核心 trick** — fs_driver.cpp 里的空 VolumePrefix
- [x] **完整 WebDAV 实现** — PROPFIND / GET / PUT / MKCOL / MOVE / DELETE
- [x] **Qt GUI** — 填表挂载、一键测试 GetDriveType
- [x] **内存目录缓存** — 避免反复 PROPFIND
- [x] **GitHub Actions CI** — push 自动构建 exe
- [ ] 本地磁盘文件缓存（目前仅内存缓存）
- [ ] Digest / Bearer 认证（目前仅 Basic）
- [ ] Inno Setup 安装包（含 WinFsp）
- [ ] CI x86 目标

## License

MIT — 见 [`LICENSE`](./LICENSE)。
