/*
 * webdav2localdisk — webdav_client.h
 *
 * Shared surfaces for the WebDAV protocol layer.
 *
 * fs_driver.cpp calls DavRuntimeCreate/Destroy to manage the runtime
 * that backs the WinFsp callbacks. It never sees the internal struct.
 */
#ifndef WDL_WEBDAV_CLIENT_H_
#define WDL_WEBDAV_CLIENT_H_

#include <string>
#include <winfsp/winfsp.h>

namespace wdl {

struct WebDavCtx;               /* fwd from fs.h */
struct DavRuntime;              /* opaque — defined in webdav_client.cpp */

DavRuntime *DavRuntimeCreate(WebDavCtx *cfg);
void        DavRuntimeDestroy(DavRuntime *rt);

}  // namespace wdl

#ifdef __cplusplus
extern "C" {
#endif
extern FSP_FILE_SYSTEM_INTERFACE g_kWdlFsInterface;
#ifdef __cplusplus
}
#endif

#endif  // WDL_WEBDAV_CLIENT_H_
