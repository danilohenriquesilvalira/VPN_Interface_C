/* webview_bridge.h — C-compatible header for the WebView2 COM bridge.
 * vpn.c / vpn.h are UNTOUCHED. This header is only included in main.c.
 */
#ifndef WEBVIEW_BRIDGE_H
#define WEBVIEW_BRIDGE_H

#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Callback: called on the UI thread when JS posts a message via
 *   window.chrome.webview.postMessage(jsonString)
 * json_msg is a UTF-8 null-terminated string; do NOT free it. */
typedef void (*WvMsgCallback)(const char *json_msg);

/* Create and attach a WebView2 inside hwnd_parent.
 * cb will be called on WM_APP+10 dispatched from the bridge.
 * Returns 1 on success, 0 on failure (e.g. WebView2Loader.dll missing). */
int  wv_create(HWND hwnd_parent, WvMsgCallback cb);

/* Load html_utf8 (raw HTML string, len bytes) into the WebView.
 * Safe to call before the WebView is fully initialised — queued. */
void wv_navigate_html(const char *html_utf8, int len);

/* Post a JSON string from C to JavaScript:
 *   JS side: window.chrome.webview.addEventListener('message', e => ...)
 *            e.data is the raw string passed here.
 * Safe to call before ready — queued automatically. */
void wv_post_json(const char *json);

/* Resize the WebView2 surface to fill (w × h) pixels. */
void wv_resize(int w, int h);

/* Destroy the WebView2 controller and free resources. */
void wv_destroy(void);

#ifdef __cplusplus
}
#endif

#endif /* WEBVIEW_BRIDGE_H */
