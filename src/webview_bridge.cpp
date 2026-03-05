/* webview_bridge.cpp — WebView2 COM bridge without WRL/ATL.
 * Compiles with MinGW-w64 g++ -std=c++17.
 * vpn.c / vpn.h are NEVER included here.
 */

/* INITGUID must be defined in exactly one translation unit so that DEFINE_GUID
 * macros in WebView2.h (and COM headers) emit the actual GUID values instead of
 * extern declarations only.  MinGW also needs initguid.h to wire this up. */
#define INITGUID
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <initguid.h>   /* must come after INITGUID define, before WebView2.h */
#include <ole2.h>
#include <string>
#include <vector>
#include <cstring>

#include "WebView2.h"          /* downloaded via NuGet during CI */
#include "webview_bridge.h"

/* ------------------------------------------------------------------ */
/* Internal state                                                       */
/* ------------------------------------------------------------------ */
static HWND                       s_hwnd     = nullptr;
static WvMsgCallback              s_cb       = nullptr;
static ICoreWebView2Controller   *s_ctrl     = nullptr;
static ICoreWebView2             *s_wv       = nullptr;
static bool                       s_ready    = false;

static std::wstring               s_pending_html;
static std::vector<std::wstring>  s_pending_msgs;

/* ------------------------------------------------------------------ */
/* Helpers                                                              */
/* ------------------------------------------------------------------ */
static std::wstring utf8_to_ws(const char *utf8, int len = -1)
{
    if (!utf8 || (len == 0)) return {};
    int needed = MultiByteToWideChar(CP_UTF8, 0, utf8, len, nullptr, 0);
    if (needed <= 0) return {};
    std::wstring ws(needed, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8, len, &ws[0], needed);
    /* Remove null terminator MultiByteToWideChar appended when len==-1 */
    if (!ws.empty() && ws.back() == L'\0') ws.pop_back();
    return ws;
}

static std::string ws_to_utf8(const wchar_t *ws)
{
    if (!ws) return {};
    int needed = WideCharToMultiByte(CP_UTF8, 0, ws, -1, nullptr, 0, nullptr, nullptr);
    if (needed <= 0) return {};
    std::string s(needed, '\0');
    WideCharToMultiByte(CP_UTF8, 0, ws, -1, &s[0], needed, nullptr, nullptr);
    if (!s.empty() && s.back() == '\0') s.pop_back();
    return s;
}

static void flush_pending()
{
    if (!s_wv) return;

    if (!s_pending_html.empty()) {
        s_wv->NavigateToString(s_pending_html.c_str());
        s_pending_html.clear();
    }
    for (auto &msg : s_pending_msgs) {
        s_wv->PostWebMessageAsString(msg.c_str());
    }
    s_pending_msgs.clear();
}

/* ------------------------------------------------------------------ */
/* COM callback: message received from JavaScript                       */
/* ------------------------------------------------------------------ */
struct MsgHandler : ICoreWebView2WebMessageReceivedEventHandler
{
    ULONG m_ref = 1;

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppv) override
    {
        if (IsEqualIID(riid, IID_IUnknown) ||
            IsEqualIID(riid, IID_ICoreWebView2WebMessageReceivedEventHandler)) {
            *ppv = this; AddRef(); return S_OK;
        }
        *ppv = nullptr; return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef()  override { return ++m_ref; }
    ULONG STDMETHODCALLTYPE Release() override
    {
        if (--m_ref == 0) { delete this; return 0; }
        return m_ref;
    }

    HRESULT STDMETHODCALLTYPE Invoke(
        ICoreWebView2 * /*sender*/,
        ICoreWebView2WebMessageReceivedEventArgs *args) override
    {
        if (!s_cb) return S_OK;
        LPWSTR rawW = nullptr;
        if (SUCCEEDED(args->TryGetWebMessageAsString(&rawW)) && rawW) {
            std::string utf8 = ws_to_utf8(rawW);
            CoTaskMemFree(rawW);
            if (s_cb) s_cb(utf8.c_str());
        }
        return S_OK;
    }
};

/* ------------------------------------------------------------------ */
/* COM callback: controller created                                     */
/* ------------------------------------------------------------------ */
struct CtrlHandler : ICoreWebView2CreateCoreWebView2ControllerCompletedHandler
{
    ULONG m_ref = 1;

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppv) override
    {
        if (IsEqualIID(riid, IID_IUnknown) ||
            IsEqualIID(riid, IID_ICoreWebView2CreateCoreWebView2ControllerCompletedHandler)) {
            *ppv = this; AddRef(); return S_OK;
        }
        *ppv = nullptr; return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef()  override { return ++m_ref; }
    ULONG STDMETHODCALLTYPE Release() override
    {
        if (--m_ref == 0) { delete this; return 0; }
        return m_ref;
    }

    HRESULT STDMETHODCALLTYPE Invoke(
        HRESULT hr,
        ICoreWebView2Controller *ctrl) override
    {
        if (FAILED(hr) || !ctrl) return S_OK;

        s_ctrl = ctrl;
        s_ctrl->AddRef();

        /* Stretch to fill parent */
        RECT rc; GetClientRect(s_hwnd, &rc);
        s_ctrl->put_Bounds(rc);
        s_ctrl->put_IsVisible(TRUE);

        ICoreWebView2 *wv2 = nullptr;
        if (FAILED(s_ctrl->get_CoreWebView2(&wv2)) || !wv2) return S_OK;
        s_wv = wv2;   /* already AddRef'd by getter */

        /* Disable context menu + dev tools for release feel */
        ICoreWebView2Settings *settings = nullptr;
        if (SUCCEEDED(s_wv->get_Settings(&settings)) && settings) {
            settings->put_AreDefaultContextMenusEnabled(FALSE);
            settings->put_AreDevToolsEnabled(FALSE);
            settings->put_IsStatusBarEnabled(FALSE);
            settings->Release();
        }

        /* Register message handler */
        EventRegistrationToken tok{};
        s_wv->add_WebMessageReceived(new MsgHandler(), &tok);

        s_ready = true;
        flush_pending();
        return S_OK;
    }
};

/* ------------------------------------------------------------------ */
/* COM callback: environment created                                    */
/* ------------------------------------------------------------------ */
struct EnvHandler : ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler
{
    ULONG m_ref = 1;

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppv) override
    {
        if (IsEqualIID(riid, IID_IUnknown) ||
            IsEqualIID(riid, IID_ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler)) {
            *ppv = this; AddRef(); return S_OK;
        }
        *ppv = nullptr; return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef()  override { return ++m_ref; }
    ULONG STDMETHODCALLTYPE Release() override
    {
        if (--m_ref == 0) { delete this; return 0; }
        return m_ref;
    }

    HRESULT STDMETHODCALLTYPE Invoke(
        HRESULT hr,
        ICoreWebView2Environment *env) override
    {
        if (FAILED(hr) || !env) return S_OK;
        env->CreateCoreWebView2Controller(s_hwnd, new CtrlHandler());
        return S_OK;
    }
};

/* ------------------------------------------------------------------ */
/* Public API (extern "C")                                             */
/* ------------------------------------------------------------------ */
extern "C" {

int wv_create(HWND hwnd_parent, WvMsgCallback cb)
{
    s_hwnd  = hwnd_parent;
    s_cb    = cb;
    s_ready = false;

    OleInitialize(nullptr);

    /* userData dir: %APPDATA%\RLS_VPN\WebView2 — use GetEnvironmentVariable
     * to avoid the deprecated CSIDL_ API (and shlobj.h requirement). */
    wchar_t userDataW[MAX_PATH] = L".";
    {
        wchar_t appData[MAX_PATH] = {};
        if (GetEnvironmentVariableW(L"APPDATA", appData, MAX_PATH) > 0) {
            /* appData + \RLS_VPN\WebView2, stay within MAX_PATH */
            int rem = MAX_PATH - (int)wcslen(appData) - 1;
            if (rem > 20) {
                wcscpy(userDataW, appData);
                wcscat(userDataW, L"\\RLS_VPN\\WebView2");
            }
        }
    }

    HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(
        nullptr,      /* browser executable folder — auto-detect */
        userDataW,
        nullptr,      /* no extra options */
        new EnvHandler());

    return SUCCEEDED(hr) ? 1 : 0;
}

void wv_navigate_html(const char *html_utf8, int len)
{
    std::wstring ws = utf8_to_ws(html_utf8, len);
    if (s_ready && s_wv) {
        s_wv->NavigateToString(ws.c_str());
    } else {
        s_pending_html = std::move(ws);
    }
}

void wv_post_json(const char *json)
{
    if (!json) return;
    std::wstring ws = utf8_to_ws(json);
    if (s_ready && s_wv) {
        s_wv->PostWebMessageAsString(ws.c_str());
    } else {
        s_pending_msgs.push_back(std::move(ws));
    }
}

void wv_resize(int w, int h)
{
    if (!s_ctrl) return;
    RECT rc = {0, 0, (LONG)w, (LONG)h};
    s_ctrl->put_Bounds(rc);
}

void wv_destroy(void)
{
    if (s_wv)   { s_wv->Release();   s_wv   = nullptr; }
    if (s_ctrl) { s_ctrl->Release(); s_ctrl = nullptr; }
    s_ready = false;
    OleUninitialize();
}

} /* extern "C" */
