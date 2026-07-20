/*
 * webdav2localdisk — webdav_client.h
 *
 * Shared surfaces for the WebDAV protocol layer.
 */
#ifndef WDL_WEBDAV_CLIENT_H_
#define WDL_WEBDAV_CLIENT_H_

/* WinFsp interface table, filled in by webdav_client.c. */
#include <winfsp/winfsp.h>

#ifdef __cplusplus
extern "C" {
#endif

extern FSP_FILE_SYSTEM_INTERFACE g_kWdlFsInterface;

#ifdef __cplusplus
}
#endif

#endif  // WDL_WEBDAV_CLIENT_H_
