# webdav2localdisk

> 把 WebDAV 服务器映射成本地 Windows 驱动器，并让 `GetDriveType()` 返回
> **`DRIVE_FIXED`** —— 在内核卷设备层级，把远程 WebDAV 存储伪装成本地固定磁盘。

[English](./README.md) | 中文

---

## 为什么需要它？

Windows 上有些程序会拒绝在「网络驱动器」上运行，甚至连列出都不行。它们调用
`GetDriveTypeW("X:\\")`，只要返回值不是 `DRIVE_FIXED`（典型本地磁盘）就直接退出。
常见案例：

- 把存档写在「本地盘」上的游戏
- 拒绝安装到网络路径的安装器
- 索引工具、同步客户端、性能计数器
- 自动跳过 `DRIVE_REMOTE` 的备份软件

云 WebDAV 提供商（你的 NAS、Nextcloud、Synology Drive 的 WebDAV、
`rclone serve webdav` 等）完全够用 —— *只要卷不被标成 "remote"*。
`webdav2localdisk` 干的就是这个贴标签的活（原理见
[PRINCIPLE.md](./docs/PRINCIPLE.md)），并把它接到一个真正能用的 WebDAV 客户端上。

## 能做什么 / 不能做什么

✅ 把任意 WebDAV 服务器挂到一个 Windows 盘符上。  
✅ 对所有 Win32 调用方（`GetDriveTypeW`、`GetVolumeInformationW` 等）返回
   `DriveType == DRIVE_FIXED`。  
✅ 可选的本地缓存层：热读快、写后批量刷盘。

❌ **不是**安全绕过工具。它不提升权限，也不会把网络延迟藏起来 —— 任何真的去探你
   吞吐量的程序都会看到 WebDAV 级的速度。  
❌ **不是**取证规避工具。卷设备本身就是一个合法的 WinFsp 本地磁盘映像。所谓
   "伪装" 只是设备类别标签，不是数据来源欺骗。

## 编译

依赖：

- Windows 10/11 x64
- CMake ≥ 3.20
- Visual Studio 2022 (MSVC) 或 clang-cl
- [WinFsp 2023+](https://winfsp.dev/)（运行机器上也需要装 WinFsp 可再发行包）
- OpenSSL（缓存层 SHA-1 用，不需要缓存可以删掉 `cache.cpp`）

```bat
git clone https://github.com/BlueFang/webdav2localdisk.git
cd webdav2localdisk
cmake -B build -S .
cmake --build build --config Release
```

## 运行

```bat
:: 先装好 WinFsp 可再发行包（https://winfsp.dev/）

webdav2localdisk.exe ^
    --url   https://dav.example.com/dav ^
    --user  alice ^
    --pass  hunter2 ^
    --drive W: ^
    --cache "%LOCALAPPDATA%\webdav2localdisk\cache"
```

验证生效：

```bat
:: 期望输出: 3  (即 DRIVE_FIXED)
powershell -NoProfile -Command "Add-Type -TypeDefinition 'using System;using System.Runtime.InteropServices;static class W{[DllImport(\"kernel32\")]public static extern uint GetDriveType(string lpRootPathName);}'; [W]::GetDriveType('W:\')"
```

不用本项目、用 `net use W: \\server\share` 普通映射会得到 `4`（`DRIVE_REMOTE`）；
用了本项目是 `3`（`DRIVE_FIXED`）。

## 原理（一句话版）

整个项目命门是 `fs_driver.cpp` 里的一行：调 `FspFileSystemCreate` 时，把
**`VolumePrefix` 传空字符串 `""`**。WinFsp 用这个前缀判断——走 Multiple UNC
Provider 网络栈（`DeviceType == FILE_DEVICE_NETWORK_FILE_SYSTEM`，
`GetDriveType == DRIVE_REMOTE`）还是当本地普通盘挂（
`DeviceType == FILE_DEVICE_DISK`，`GetDriveType == DRIVE_FIXED`）。其它字段都是
装饰，只有前缀翻得动设备类型。

完整推导、参考资料、逆向笔记见 **[`docs/PRINCIPLE.md`](./docs/PRINCIPLE.md)**。

## 状态

这是早期公开骨架。已完成的勾：

- [x] 配置 `VolumeParams` 强制返回 `DRIVE_FIXED`
- [x] WinFsp 回调分发表
- [x] 命令行解析 + 挂载生命周期（信号驱动卸载）

还没做（欢迎 PR）：

- [ ] 完整的 WebDAV 动词映射（`PROPFIND`/`PUT`/`MOVE`/`COPY`/`MKCOL`）
- [ ] 真正的 `cache.cpp`（目前是桩）
- [ ] 鉴权：Basic + Digest
- [ ] Inno Setup 安装包 + 签名构建
- [ ] CI 矩阵（MSVC / clang-cl / x86 / x64）

## 贡献

PR 欢迎，尤其 WebDAV 协议层 —— 那是项目目前缺口最大的一块。请把 `VolumePrefix=""`
当作神圣不变量：它就是定义本项目的单行代码。

## License

MIT —— 见 [`LICENSE`](./LICENSE)。
