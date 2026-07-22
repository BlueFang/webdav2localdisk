/*
 * webdav2localdisk — webdav_client.cpp
 *
 * WebDAV protocol implementation behind the WinFsp mount callback table.
 *
 * HTTP transport: WinHttp (no curl / libxml dependency).
 * XML parsing:    minimal SAX-style scanner for DAV:multistatus only.
 *
 * Design: every WinFsp FS is created with a WebDavCtx* via user-context.
 * A single DavRuntime object is embedded there, holding an Http session.
 * All callbacks grab the ctx, extract the runtime, and delegate to it.
 *
 * Thread-safety: WinFsp callbacks are serialised per-volume (the dispatcher
 * is single-threaded by default). We add a simple sync mutex for the
 * directory cache but no global lock beyond that.
 */
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define WIN32_NO_STATUS
#include <windows.h>
#undef WIN32_NO_STATUS
#include <winternl.h>
#include <ntstatus.h>
#ifndef PNTSTATUS
typedef NTSTATUS *PNTSTATUS;
#endif
#include <winhttp.h>
#include <wincrypt.h>
#include <winfsp/winfsp.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

#include "fs.h"
#include "webdav_client.h"

#pragma comment(lib, "winhttp.lib")

namespace wdl {

/* ===================================================================== */
/*  helpers                                                               */
/* ===================================================================== */

static std::string WideToUtf8(const std::wstring &w) {
  if (w.empty()) return {};
  int n = ::WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(),
                                nullptr, 0, nullptr, nullptr);
  if (n <= 0) return {};
  std::string s(n, 0);
  ::WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(),
                        &s[0], n, nullptr, nullptr);
  return s;
}
static std::wstring Utf8ToWide(const std::string &s) {
  if (s.empty()) return {};
  int n = ::MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
  if (n <= 0) return {};
  std::wstring w(n, 0);
  ::MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
  return w;
}
static void UnixToNT(uint64_t *t) {
  if (*t) *t = (*t + 11644473600ULL) * 10000000ULL;
}

/* NTSTATUS from HTTP status + typical error codes. */
static NTSTATUS MapHttp(uint32_t code) {
  if (code >= 200 && code < 300) return STATUS_SUCCESS;
  if (code == 404) return STATUS_OBJECT_NAME_NOT_FOUND;
  if (code == 401 || code == 403) return STATUS_ACCESS_DENIED;
  if (code == 405 || code == 409) return STATUS_OBJECT_NAME_COLLISION;
  if (code == 507) return STATUS_DISK_FULL;
  if (code >= 500) return STATUS_IO_DEVICE_ERROR;
  return STATUS_IO_DEVICE_ERROR;
}

static std::wstring to_path(const wchar_t *p) {
  std::wstring s = p ? p : L"";
  for (auto &c : s) if (c == L'\\') c = L'/';
  if (s.empty() || s.front() != L'/') s.insert(s.begin(), L'/');
  return s;
}

/* ===================================================================== */
/*  Tiny XML scanner — just enough to parse DAV:multistatus                */
/* ===================================================================== */
struct XmlEi {
  std::string  local;       /* <D:response> -> local=="response" */
  bool end    = false;      /* </… */  
};
static XmlEi xml_next(const std::string &body, size_t &pos) {
  while (pos < body.size()) {
    if (body[pos] != '<') { ++pos; continue; }
    size_t beg = pos++; bool c = false;
    if (pos < body.size() && body[pos] == '/') { c = true; ++pos; }
    while (pos < body.size() && body[pos] != '>' && body[pos] != ' ') ++pos;
    std::string tag(body.begin() + beg + (c ? 2 : 1),
                    body.begin() + pos);
    if (!tag.empty() && tag.back() == '/') tag.pop_back();
    if (pos < body.size() && body[pos] != '>') {
      while (pos < body.size() && body[pos] != '>') ++pos;
    }
    if (pos < body.size()) ++pos; /* skip '>' */
    if (tag.empty()) continue;
    /* strip namespace prefix */
    auto col = tag.find(':');
    if (col != std::string::npos) tag = tag.substr(col + 1);
    return {tag, c};
  }
  return {"", false};
}
static std::string xml_inner(const std::string &body, size_t &pos) {
  size_t start = pos;
  while (pos < body.size() && body[pos] != '<') ++pos;
  return std::string(body.begin() + start, body.begin() + pos);
}

/* ===================================================================== */
/*  Http                                                                  */
/* ===================================================================== */
class Http {
 public:
  explicit Http(const std::wstring &server_url);
  ~Http();
  bool Open(const std::wstring &user, const std::wstring &pass);
  /* Returns HTTP status code (0 = transport error). */
  uint32_t Send(const std::wstring &rel_path,
                const wchar_t *verb,
                const void *body = nullptr,
                uint32_t body_len = 0,
                const wchar_t *depth = nullptr,
                const wchar_t *dest = nullptr,
                const wchar_t *range = nullptr);
  std::string ReadAll(uint32_t *p_bytes = nullptr);
  bool        ReadBin(void *dst, uint32_t want, uint32_t *got);
  uint32_t    LastCode() const { return last_code_; }

 private:
  HINTERNET session_ = nullptr, connect_ = nullptr;
  HINTERNET req_ = nullptr;
  uint32_t  last_code_ = 0;
  std::wstring host_, root_path_;
  INTERNET_PORT port_ = 443;
  bool https_ = true;
  std::wstring auth_hdr_;
};
Http::Http(const std::wstring &url) {
  URL_COMPONENTS uc{ sizeof(uc) };
  wchar_t ho[256]={0}, pa[1024]={0};
  uc.lpszHostName = ho; uc.dwHostNameLength = 255;
  uc.lpszUrlPath  = pa; uc.dwUrlPathLength  = 1023;
  if (::WinHttpCrackUrl(url.c_str(), 0, 0, &uc)) {
    host_ = ho; port_ = uc.nPort;
    https_ = (uc.nScheme == INTERNET_SCHEME_HTTPS);
    root_path_ = pa;
    if (!root_path_.empty() && root_path_.back() == L'/')
      root_path_.pop_back();
  }
}
Http::~Http() {
  if (req_) ::WinHttpCloseHandle(req_);
  if (connect_) ::WinHttpCloseHandle(connect_);
  if (session_) ::WinHttpCloseHandle(session_);
}
bool Http::Open(const std::wstring &user, const std::wstring &pass) {
  session_ = ::WinHttpOpen(L"webdav2localdisk/0.1",
                           WINHTTP_ACCESS_TYPE_NO_PROXY,
                           WINHTTP_NO_PROXY_NAME,
                           WINHTTP_NO_PROXY_BYPASS, 0);
  if (!session_) return false;
  connect_ = ::WinHttpConnect(session_, host_.c_str(), port_, 0);
  if (!connect_) return false;
  /* pre-build Basic auth header */
  if (!user.empty()) {
    std::string c = WideToUtf8(user) + ":" + WideToUtf8(pass);
    DWORD bsz = ((DWORD)c.size() + 2) * 4 / 3 + 8;
    std::string b64(bsz, 0);
    DWORD n = bsz;
    ::CryptBinaryToStringA((const BYTE*)c.data(), (DWORD)c.size(),
                           CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF,
                           &b64[0], &n);
    b64.resize(n);
    auth_hdr_ = L"Authorization: Basic " + Utf8ToWide(b64);
  }
  return true;
}
uint32_t Http::Send(const std::wstring &rel_path,
                    const wchar_t *verb,
                    const void *body, uint32_t body_len,
                    const wchar_t *depth,
                    const wchar_t *dest,
                    const wchar_t *range) {
  if (req_) { ::WinHttpCloseHandle(req_); req_ = nullptr; }
  last_code_ = 0;
  std::wstring full = root_path_ + rel_path;
  req_ = ::WinHttpOpenRequest(connect_, verb, full.c_str(), nullptr,
                              WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                              https_ ? WINHTTP_FLAG_SECURE : 0);
  if (!req_) return 0;

  /* auth */
  if (!auth_hdr_.empty())
    ::WinHttpAddRequestHeaders(req_, auth_hdr_.c_str(),
        (DWORD)-1, WINHTTP_ADDREQ_FLAG_ADD);
  /* depth */
  if (depth && *depth) {
    std::wstring d = std::wstring(L"Depth: ") + depth;
    ::WinHttpAddRequestHeaders(req_, d.c_str(),
        (DWORD)-1, WINHTTP_ADDREQ_FLAG_ADD);
  }
  /* destination (for MOVE/COPY) */
  if (dest && *dest) {
    std::wstring d = std::wstring(L"Destination: ") + dest;
    ::WinHttpAddRequestHeaders(req_, d.c_str(),
        (DWORD)-1, WINHTTP_ADDREQ_FLAG_ADD);
  }
  /* range */
  if (range && *range) {
    std::wstring r = std::wstring(L"Range: bytes=") + range;
    ::WinHttpAddRequestHeaders(req_, r.c_str(),
        (DWORD)-1, WINHTTP_ADDREQ_FLAG_ADD);
  }
  DWORD fl = WINHTTP_DISABLE_REDIRECTS; /* we handle WebDAV redirects manually */
  ::WinHttpSetOption(req_, WINHTTP_OPTION_DISABLE_FEATURE, &fl, sizeof(fl));

  if (!::WinHttpSendRequest(req_, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                             const_cast<void*>(body), body_len, body_len, 0))
    { ::WinHttpCloseHandle(req_); req_ = nullptr; return 0; }
  if (!::WinHttpReceiveResponse(req_, nullptr))
    { ::WinHttpCloseHandle(req_); req_ = nullptr; return 0; }
  DWORD c = 0, cs = sizeof(c);
  ::WinHttpQueryHeaders(req_, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
      WINHTTP_HEADER_NAME_BY_INDEX, &c, &cs, WINHTTP_NO_HEADER_INDEX);
  last_code_ = c;
  return c;
}
std::string Http::ReadAll(uint32_t *p_bytes) {
  std::string out;
  if (!req_) return out;
  DWORD a = 0;
  while (::WinHttpQueryDataAvailable(req_, &a) && a > 0) {
    size_t off = out.size(); out.resize(off + a);
    DWORD r = 0;
    if (!::WinHttpReadData(req_, &out[off], a, &r)) break;
    out.resize(off + r);
  }
  if (p_bytes) *p_bytes = (uint32_t)out.size();
  return out;
}
bool Http::ReadBin(void *dst, uint32_t want, uint32_t *got) {
  if (!req_) { *got = 0; return false; }
  *got = 0; DWORD a = 0;
  while (*got < want &&
         ::WinHttpQueryDataAvailable(req_, &a) && a > 0) {
    DWORD take = (std::min)((DWORD)(want - *got), a);
    DWORD r = 0;
    if (!::WinHttpReadData(req_, (BYTE*)dst + *got, take, &r)) break;
    *got += r; if (r == 0) break;
  }
  return *got > 0;
}

/* ===================================================================== */
/*  DirCache — in-memory directory listing                                 */
/* ===================================================================== */
struct DirCache {
  std::mutex m;
  std::map<std::string, std::vector<WebDavFile>> dirs;
  std::map<std::string, WebDavFile> files;  /* ALL known files for GetInfo */
};

/* ===================================================================== */
/*  DavRuntime — attached to FS user-context                               */
/* ===================================================================== */
struct DavRuntime {
  Http        http;
  WebDavCtx  *cfg;
  DirCache    dc;
  std::mutex  mu;   /* serialise dir-cache updates */

  DavRuntime(WebDavCtx *c) : http(c->server_url), cfg(c) {}
};

/* file-system-object to Win32 attr bits */
static inline uint32_t WdlAttrs(WebDavFile &f) {
  uint32_t a = f.attrs;
  if (f.is_dir) a |= FILE_ATTRIBUTE_DIRECTORY;
  else          a |= FILE_ATTRIBUTE_NORMAL;
  return a;
}
static void FillFileInfo(FSP_FSCTL_FILE_INFO *o, WebDavFile &f) {
  memset(o, 0, sizeof(*o));
  o->FileAttributes = WdlAttrs(f);
  o->FileSize        = f.is_dir ? 0 : f.size;
  o->AllocationSize  = (f.size + 4095) & ~4095ULL;
  o->ChangeTime = o->LastAccessTime = o->LastWriteTime = o->CreationTime = f.mtime;
}

/* parse a PROPFIND multistatus response into WebDavFile entries.
 * updates dc.files + dc.dirs with fresh data. */
static void ParsePropfind(const std::string &xml, const std::string &parent,
                          DirCache &dc) {
  std::vector<WebDavFile> dl;
  std::string cur_href, cur_disp;
  WebDavFile cur{}; enum { OUT, IN_RESPONSE, IN_HREF, IN_DISPLAYNAME,
    IN_SIZE, IN_MODIFIED, IN_RESOURCETYPE, IN_COLLECTION } st = OUT;
  size_t p = 0; XmlEi ei;
  while (true) {
    ei = xml_next(xml, p);
    if (ei.local.empty()) break;
    std::string inner = xml_inner(xml, p);
    if (!ei.end) {
      if (ei.local == "response")                { st = IN_RESPONSE; cur=WebDavFile{}; cur_href.clear(); }
      else if (st == IN_RESPONSE && ei.local == "href")      st = IN_HREF;
      else if (st == IN_RESPONSE && ei.local == "displayname") st = IN_DISPLAYNAME;
      else if (st == IN_RESPONSE && ei.local == "getcontentlength") st = IN_SIZE;
      else if (st == IN_RESPONSE && ei.local == "getlastmodified") st = IN_MODIFIED;
      else if (st == IN_RESPONSE && ei.local == "resourcetype")   st = IN_RESOURCETYPE;
      else if (st == IN_RESOURCETYPE && ei.local == "collection") st = IN_COLLECTION;
    } else {
      if (ei.local == "response") {
        if (!cur_href.empty() && cur_href != parent) {
          cur.name = cur_href;
          if (!cur.name.empty() && cur.name.front() == '/')
            cur.name.erase(0, 1);
          if (!cur.name.empty() && cur.name.back() == '/')
            cur.name.pop_back();
          dc.files[cur.name] = cur;
          /* only direct children go into the dir listing */
          std::string leaf = cur.name;
          auto slash = leaf.rfind('/');
          if (slash != std::string::npos) leaf = leaf.substr(slash + 1);
          if (!leaf.empty()) dl.push_back(cur);
        }
        st = OUT;
      } else if (ei.local == "collection")  { cur.is_dir = true;  st = IN_RESOURCETYPE; }
      else if (ei.local == "resourcetype")  { st = IN_RESPONSE; }
      else if (st == IN_HREF)            { cur_href = inner; st = IN_RESPONSE; }
      else if (st == IN_DISPLAYNAME)     { cur_disp = inner; st = IN_RESPONSE; }
      else if (st == IN_SIZE)            { cur.size = strtoull(inner.c_str(), nullptr, 10); st = IN_RESPONSE; }
      else if (st == IN_MODIFIED)        {
        /* crude HTTP-date parser; enough for practical PROPFIND. */
        struct tm tm{}; struct { const char *a,*b; int v; } mn[]={
          {"jan",nullptr,0},{"feb",nullptr,1},{"mar",nullptr,2},{"apr",nullptr,3},
          {"may",nullptr,4},{"jun",nullptr,5},{"jul",nullptr,6},{"aug",nullptr,7},
          {"sep",nullptr,8},{"oct",nullptr,9},{"nov",nullptr,10},{"dec",nullptr,11}};
        auto lo = inner; for (auto &ch:lo) ch=(char)tolower((unsigned char)ch);
        for (auto &m:mn) if (lo.find(m.a)!=std::string::npos) {tm.tm_mon=m.v; break;}
        sscanf(inner.c_str(), "%*3s %d %d %d", &tm.tm_mday, &tm.tm_year, &tm.tm_hour);
        tm.tm_year -= 1900;
        time_t t = _mkgmtime(&tm);
        cur.mtime = t > 0 ? (uint64_t)t : 0;
        UnixToNT(&cur.mtime);
        st = IN_RESPONSE;
      }
    }
  }
  dc.dirs[parent] = dl;
}

/* ===================================================================== */
/*  FileHandle — open file context (cast from PVOID)                      */
/* ===================================================================== */
struct FileHandle {
  std::string path;  /* POSIX relative */
  bool dir = false;
  std::vector<WebDavFile> children; /* if dir */
  uint64_t size = 0;
  /* pending write buffer — flushed to server on close */
  std::string pending;
  bool dirty = false;
};

/* ===================================================================== */
/*  Internal helpers (operate on DavRuntime)                              */
/* ===================================================================== */
static DavRuntime* R(FSP_FILE_SYSTEM *fs) {
  return static_cast<DavRuntime*>(FspFileSystemGetUserContext(fs));
}

/* Fetch the PROPFIND for <rel> and cache files. Returns STATUS. */
static NTSTATUS dav_refresh(DavRuntime *r, const std::string &rel) {
  std::wstring path = Utf8ToWide(rel);
  uint32_t code = r->http.Send(path, L"PROPFIND", nullptr, 0, L"1");
  if (code < 200 || code > 207) return MapHttp(code);
  std::string xml = r->http.ReadAll();
  ParsePropfind(xml, rel, r->dc);
  return STATUS_SUCCESS;
}

/* Return cached WebDavFile* or nullptr. Auto-fetches missing dirs. */
static NTSTATUS dav_get_info(DavRuntime *r, const std::string &rel,
                             WebDavFile **out) {
  /* check cache */
  auto it = r->dc.files.find(rel);
  if (it == r->dc.files.end()) {
    /* PROPFIND depth:0 for a single file */
    std::wstring path = Utf8ToWide(rel);
    uint32_t code = r->http.Send(path, L"PROPFIND", nullptr, 0, L"0");
    if (code < 200 || code > 207) return MapHttp(code);
    std::string xml = r->http.ReadAll();
    /* grab parent dir */
    std::string parent = rel;
    auto sl = parent.rfind('/');
    if (sl != std::string::npos) parent = parent.substr(0, sl);
    else parent.clear();
    ParsePropfind(xml, parent, r->dc);
    it = r->dc.files.find(rel);
    if (it == r->dc.files.end()) return STATUS_OBJECT_NAME_NOT_FOUND;
  }
  *out = &it->second;
  return STATUS_SUCCESS;
}

/* ===================================================================== */
/*  Callbacks                                                             */
/* ===================================================================== */
static NTSTATUS TpCreate(FSP_FILE_SYSTEM *fs, FSP_FSCTL_TRANSACT_REQ *, PWSTR name,
                          UINT32 create_opts, UINT32, UINT32 attrs,
                          PSECURITY_DESCRIPTOR, UINT64,
                          PVOID *out_ctx, FSP_FSCTL_FILE_INFO *out_info) {
  auto *r = R(fs);
  std::string rel = WideToUtf8(to_path(name));
  auto *fh = new FileHandle; fh->path = rel;
  if (create_opts & FILE_DIRECTORY_FILE) {
    fh->dir = true;
    /* MKCOL */
    std::wstring wp = Utf8ToWide(rel);
    uint32_t code = r->http.Send(wp, L"MKCOL");
    if (code < 200 || code >= 300 && code != 405) {
      delete fh; return MapHttp(code);
    }
    r->dc.files[rel] = {rel, true, 0, 0, attrs};
    *out_ctx = fh;
    FillFileInfo(out_info, r->dc.files[rel]);
    return STATUS_SUCCESS;
  }
  /* create regular file — PUT empty body */
  std::wstring wp = Utf8ToWide(rel);
  uint32_t code = r->http.Send(wp, L"PUT", "", 0);
  if (code >= 200 && code < 300) {
    r->dc.files[rel] = {rel, false, 0, 0, attrs};
    *out_ctx = fh;
    FillFileInfo(out_info, r->dc.files[rel]);
    return STATUS_SUCCESS;
  }
  delete fh; return MapHttp(code);
}
static NTSTATUS TpOpen(FSP_FILE_SYSTEM *fs, FSP_FSCTL_TRANSACT_REQ *, PWSTR name,
                        UINT32, PVOID *out_ctx) {
  auto *r = R(fs);
  std::string rel = WideToUtf8(to_path(name));
  WebDavFile *info = nullptr;
  NTSTATUS st = dav_get_info(r, rel, &info);
  if (st != STATUS_SUCCESS) return st;
  auto *fh = new FileHandle; fh->path = rel;
  fh->dir = info->is_dir; fh->size = info->size;
  *out_ctx = fh; return STATUS_SUCCESS;
}
static NTSTATUS TpRead(FSP_FILE_SYSTEM *fs, PVOID ctx,
                        PVOID buf, UINT64 off, ULONG len,
                        PULONG p_transferred) {
  auto *r = R(fs); auto *fh = (FileHandle*)ctx;
  if (!fh->pending.empty()) {
    /* serve from local buffer */
    ULONG n = std::min<ULONG>(len, (ULONG)fh->pending.size() - (ULONG)off);
    if (off > fh->pending.size()) n = 0;
    if (n) memcpy(buf, fh->pending.data() + off, n);
    *p_transferred = n; return n > 0 ? STATUS_SUCCESS : STATUS_END_OF_FILE;
  }
  std::wstring path = Utf8ToWide(fh->path);
  char range[64]; snprintf(range, sizeof(range), "%llu-%llu",
                           (unsigned long long)off,
                           (unsigned long long)(off + len - 1));
  uint32_t code = r->http.Send(path, L"GET", nullptr, 0,
                               nullptr, nullptr, Utf8ToWide(range).c_str());
  if (code != 200 && code != 206) {
    *p_transferred = 0; return MapHttp(code);
  }
  uint32_t got = 0;
  r->http.ReadBin(buf, len, &got);
  *p_transferred = got;
  return got > 0 ? STATUS_SUCCESS : STATUS_END_OF_FILE;
}
static NTSTATUS TpWrite(FSP_FILE_SYSTEM *fs, PVOID ctx,
                         PVOID buf, UINT64 off, ULONG len,
                         PULONG p_transferred, FSP_FSCTL_FILE_INFO *out_info) {
  auto *r = R(fs); auto *fh = (FileHandle*)ctx;
  /* buffer write; flush on close */
  size_t need = (size_t)off + len;
  if (fh->pending.size() < need) fh->pending.resize(need, 0);
  memcpy(fh->pending.data() + off, buf, len);
  fh->dirty = true;
  fh->size = std::max(fh->size, (uint64_t)need);
  *p_transferred = len;
  WebDavFile tmp{fh->path, false, fh->size, 0};
  FillFileInfo(out_info, tmp);
  return STATUS_SUCCESS;
}
static NTSTATUS TpFlush(FSP_FILE_SYSTEM *fs, PVOID ctx, FSP_FSCTL_FILE_INFO *) {
  auto *r = R(fs); auto *fh = (FileHandle*)ctx;
  if (!fh->dirty) return STATUS_SUCCESS;
  std::wstring path = Utf8ToWide(fh->path);
  uint32_t code = r->http.Send(path, L"PUT",
                               fh->pending.data(), (uint32_t)fh->pending.size());
  if (code >= 200 && code < 300) { fh->dirty = false; return STATUS_SUCCESS; }
  return MapHttp(code);
}
static void TpClose(FSP_FILE_SYSTEM *fs, FSP_FSCTL_TRANSACT_REQ *, PVOID ctx) {
  auto *r = R(fs); auto *fh = (FileHandle*)ctx;
  if (fh->dirty) TpFlush(fs, ctx, nullptr);
  delete fh;
}
static NTSTATUS TpGetInfo(FSP_FILE_SYSTEM *fs, PVOID ctx, FSP_FSCTL_FILE_INFO *out) {
  auto *r = R(fs); auto *fh = (FileHandle*)ctx;
  WebDavFile *pf = nullptr;
  NTSTATUS st = dav_get_info(r, fh->path, &pf);
  if (st == STATUS_SUCCESS) FillFileInfo(out, *pf);
  else {
    WebDavFile tmp{fh->path, fh->dir, fh->size, 0};
    FillFileInfo(out, tmp);
  }
  return STATUS_SUCCESS;
}
static NTSTATUS TpSetInfo(FSP_FILE_SYSTEM *fs, PVOID ctx, UINT32 icls,
                           PVOID buf, FSP_FSCTL_FILE_INFO *out_info) {
  auto *r = R(fs); auto *fh = (FileHandle*)ctx;
  if (icls == FileRenameInformation) {
    PFILE_RENAME_INFORMATION ri = (PFILE_RENAME_INFORMATION)buf;
    std::wstring new_name(ri->FileName, ri->FileNameLength / sizeof(WCHAR));
    std::string new_rel = WideToUtf8(to_path(new_name.c_str()));
    std::wstring old_path = Utf8ToWide(fh->path);
    std::wstring dest = r->cfg->server_url + L"/" + Utf8ToWide(new_rel);
    uint32_t code = r->http.Send(old_path, L"MOVE", nullptr, 0,
                                 nullptr, dest.c_str());
    if (code >= 200 && code < 300) {
      /* update cache */
      r->dc.files[new_rel] = r->dc.files[fh->path];
      r->dc.dirs.erase(fh->path);
      fh->path = new_rel;
      return TpGetInfo(fs, ctx, out_info);
    }
    return MapHttp(code);
  }
  if (icls == FileDispositionInformation) {
    std::wstring path = Utf8ToWide(fh->path);
    uint32_t code = r->http.Send(path, L"DELETE");
    return code >= 200 && code < 300 ? STATUS_SUCCESS : MapHttp(code);
  }
  return STATUS_NOT_IMPLEMENTED;
}
static NTSTATUS TpEnumDir(FSP_FILE_SYSTEM *fs, FSP_FSCTL_TRANSACT_REQ *,
                           PCWSTR pat, PVOID *marker, UINT64 *next) {
  auto *r = R(fs); auto *fh = (FileHandle*)(*marker ? *marker : nullptr);
  if (!fh) return STATUS_INVALID_PARAMETER;
  std::string rel = fh->path;
  /* PROPFIND this dir if not cached */
  { std::lock_guard<std::mutex> lk(r->mu);
    if (r->dc.dirs.find(rel) == r->dc.dirs.end()) dav_refresh(r, rel);
  }
  auto &vv = r->dc.dirs[rel];
  size_t idx = reinterpret_cast<size_t>(*next);
  for (; idx < vv.size(); ++idx) {
    auto &wf = vv[idx];
    std::string entry = wf.name;
    auto sl = entry.rfind('/');
    if (sl != std::string::npos) entry = entry.substr(sl + 1);
    if (entry.empty()) continue;
    UINT8 buf[256] = {0};
    FSP_FSCTL_DIR_INFO *di = (FSP_FSCTL_DIR_INFO*)buf;
    int n = ::WideCharToMultiByte(CP_UTF8, 0,
               Utf8ToWide(entry).c_str(), -1,
               di->FileNameBuf, sizeof(buf) - sizeof(FSP_FSCTL_DIR_INFO),
               nullptr, nullptr);
    di->Size = (UINT16)(sizeof(FSP_FSCTL_DIR_INFO) + n - 1);
    di->FileInfo.FileAttributes = WdlAttrs(wf);
    di->FileInfo.FileSize       = wf.is_dir ? 0 : wf.size;
    di->FileInfo.AllocationSize = (di->FileInfo.FileSize + 4095) & ~4095ULL;
    di->FileInfo.ChangeTime = di->FileInfo.LastAccessTime =
    di->FileInfo.LastWriteTime = di->FileInfo.CreationTime = wf.mtime;
    if (FspFileSystemAddDirInfo(buf, fs, marker, next)) {
      *next = reinterpret_cast<UINT64>(
                reinterpret_cast<void*>(idx + 1));
      return STATUS_SUCCESS;
    }
  }
  return STATUS_NO_MORE_FILES;
}
static NTSTATUS TpGetVolumeInfo(FSP_FILE_SYSTEM *fs,
                                 FSP_FSCTL_VOLUME_INFO *info) {
  auto *r = R(fs);
  info->TotalSize = r->cfg->total;
  info->FreeSize  = r->cfg->free;
  auto &lbl = r->cfg->label;
  info->VolumeLabelLength = (UINT16)(lbl.size() * sizeof(WCHAR));
  memcpy(info->VolumeLabel, lbl.c_str(),
         std::min<size_t>(lbl.size() * sizeof(WCHAR), sizeof(info->VolumeLabel)));
  return STATUS_SUCCESS;
}

/* ===================================================================== */
/*  Public factory                                                        */
/* ===================================================================== */

DavRuntime *DavRuntimeCreate(WebDavCtx *cfg) {
  auto *rt = new DavRuntime(cfg);
  if (!rt->http.Open(cfg->username, cfg->password)) { delete rt; return nullptr; }
  return rt;
}
void DavRuntimeDestroy(DavRuntime *rt) { delete rt; }

/* ===================================================================== */
/*  Table                                                                 */
/* ===================================================================== */
extern "C" FSP_FILE_SYSTEM_INTERFACE g_kWdlFsInterface = {
  /* GetVolumeInfo   */ TpGetVolumeInfo,
  /* SetVolumeLabel  */ nullptr,
  /* GetSecurity     */ nullptr,
  /* SetSecurity     */ nullptr,
  /* Create          */ TpCreate,
  /* Open            */ TpOpen,
  /* Overwrite       */ nullptr,
  /* Cleanup         */ nullptr,
  /* Close           */ TpClose,
  /* Read            */ TpRead,
  /* Write           */ TpWrite,
  /* Flush           */ TpFlush,
  /* GetFileInfo     */ TpGetInfo,
  /* SetBasicInfo    */ TpSetInfo,
  /* SetFileSize     */ nullptr,
  /* Rename          */ nullptr,
  /* GetSecurity     */ nullptr,
  /* SetSecurity     */ nullptr,
  /* CreateEx        */ nullptr,
  /* OverwriteEx     */ nullptr,
  /* GetDirInfoByName*/ nullptr,
  /* ResolveReparse  */ nullptr,
  /* GetReparse      */ nullptr,
  /* SetReparse      */ nullptr,
  /* DeleteReparse   */ nullptr,
  /* EnumDirectory   */ TpEnumDir,
  /* StreamInfo      */ nullptr,
  /* GetDirInfoByName2*/ nullptr,
  /* Control         */ nullptr,
};

}  // namespace wdl
