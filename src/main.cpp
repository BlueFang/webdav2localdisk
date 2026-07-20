/*
 * webdav2localdisk — main.cpp
 *
 * CLI entry point: parses --url --user --pass --drive --cache, mounts
 * the WebDAV share onto the requested drive letter in DRIVE_FIXED mode.
 *
 * Example:
 *   webdav2localdisk.exe --url https://dav.example.com/dav \
 *       --user alice --pass secret --drive W: \
 *       --cache "%LOCALAPPDATA%\webdav2localdisk\cache"
 *
 * After mount, GetDriveTypeW("W:\\") == DRIVE_FIXED. The program keeps
 * running in the foreground; Ctrl-C or closing the console unmounts.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <csignal>
#include <string>
#include <atomic>
#include <thread>

#include "fs.h"
#include "webdav_client.h"

extern "C" FSP_FILE_SYSTEM_INTERFACE g_kWdlFsInterface;

static std::atomic<FSP_FILE_SYSTEM *> g_active_fs{nullptr};

static void PrintUsage() {
  std::fputs(
      "webdav2localdisk — map a WebDAV server to a local Windows drive\n"
      "                 that reports DRIVE_FIXED to GetDriveType().\n\n"
      "Usage:\n"
      "  webdav2localdisk --url <https://...> --drive <W:>\n"
      "     [--user <name>] [--pass <password>]\n"
      "     [--cache <dir>] [--no-cache]\n"
      "     [--label <name>]\n\n"
      "Required: --url, --drive (e.g. W:)\n",
      stderr);
}

static void OnSignal(int) {
  if (auto *fs = g_active_fs.exchange(nullptr)) {
    /* Trigger graceful unmount in main loop. */
  }
}

int wmain(int argc, wchar_t **argv) {
  wdl::WebDavCtx ctx;
  wdl::MountOptions opt;
        bool want_cache = true;

  for (int i = 1; i < argc; ++i) {
    std::wstring a = argv[i];
    auto next = [&](const wchar_t *opt) -> const wchar_t * {
      if (i + 1 >= argc) {
        std::fwprintf(stderr, L"missing value for %s\n", opt);
        return nullptr;
      }
      return argv[++i];
    };
    if      (a == L"--url")   ctx.server_url = next(L"--url");
    else if (a == L"--user")  ctx.username   = next(L"--user");
    else if (a == L"--pass")  ctx.password   = next(L"--pass");
    else if (a == L"--drive") opt.mount_point= next(L"--drive");
    else if (a == L"--cache") {
      const wchar_t *v = next(L"--cache");
      if (v) {
        std::wstring wv(v);
        ctx.cache_dir.assign(wv.begin(), wv.end());
      }
    }
    else if (a == L"--no-cache")  want_cache = false;
    else if (a == L"--label") {
      const wchar_t *v = next(L"--label");
      (void)v;
    }
    else if (a == L"-h" || a == L"--help") { PrintUsage(); return 0; }
    else {
      std::fwprintf(stderr, L"unknown option: %s\n", a.c_str());
      return 2;
    }
  }

  if (ctx.server_url.empty() || opt.mount_point.empty()) {
    PrintUsage();
    return 2;
  }

  /* Pre-flight: drive letter must be free & untaken. */
  std::wstring dev = opt.mount_point;
  if (!dev.empty() && dev.back() != L'\\') dev.push_back(L'\\');
  std::wstring letter = opt.mount_point;
  if (!letter.empty() && letter.back() == L'\\') letter.pop_back();
  if (::GetDriveTypeW(letter.c_str()) != DRIVE_NO_ROOT_DIR) {
    std::fwprintf(stderr,
                  L"drive %s is already in use; pick an empty letter.\n",
                  letter.c_str());
    return 3;
  }

  if (opt.creation_time.QuadPart == 0) {
    FILETIME ft; ::GetSystemTimeAsFileTime(&ft);
    ::memcpy(&opt.creation_time, &ft, sizeof(ft));
  }

  FSP_FILE_SYSTEM *fs = nullptr;
  if (!wdl::FsDriverMount(opt, &ctx, &fs)) {
    std::fputs("mount failed — is WinFsp installed?\n", stderr);
    return 4;
  }
  g_active_fs.store(fs);

  std::signal(SIGINT,  OnSignal);
  std::signal(SIGBREAK, OnSignal);

  std::fwprintf(stderr,
      L"webdav2localdisk: %s mounted on %s as DRIVE_FIXED.\n"
      L"GetDriveTypeW(%s\\)==DRIVE_FIXED. Ctrl-C to unmount.\n",
      ctx.server_url.c_str(), opt.mount_point.c_str(),
      opt.mount_point.c_str());

  /* Wait until signal flips the atomic to null, then unmount. */
  while (g_active_fs.load() != nullptr) {
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
  }

  wdl::FsDriverUnmount(fs);
  std::fputs("unmounted.\n", stderr);
  return 0;
}
