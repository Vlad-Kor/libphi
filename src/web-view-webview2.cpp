/*
 * Phi - Microsoft Edge WebView2 backend of PdfvWebView
 * Copyright (C) 2026 Vlad Korsakov <ulqba@student.kit.edu>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * WebView2 renders into a native child window of the GTK toplevel. That window
 * is always drawn above GTK's own rendering, so this backend
 *
 *   - clips a per-view host window to the visible part of the widget and cuts
 *     holes into it where GTK widgets (toasts, overlays) are drawn above it,
 *   - shows a snapshot of the page instead while a dialog covers the view,
 *   - keeps GTK's focus widget and the Win32 keyboard focus in step, and
 *   - routes keyboard shortcuts to GTK the way GTK would propagate them: in
 *     the capture phase before the page, and in the bubble phase for keys the
 *     page did not handle.
 */

#define G_LOG_DOMAIN "phi-web-view"

#include "web-view-backend.h"

#include <gdk/win32/gdkwin32.h>
#include <json-glib/json-glib.h>

#include <windows.h>
#include <commctrl.h>
#include <wrl/client.h>

#include <WebView2.h>

#include <algorithm>
#include <cmath>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using Microsoft::WRL::ComPtr;

/* Utilities --------------------------------------------------------------- */

namespace {

std::wstring to_wide(const gchar *text) {
  if (!text)
    return std::wstring();
  gunichar2 *wide = g_utf8_to_utf16(text, -1, nullptr, nullptr, nullptr);
  std::wstring result(wide ? reinterpret_cast<wchar_t *>(wide) : L"");
  g_free(wide);
  return result;
}

/* Takes ownership of a CoTaskMemAlloc()ed string. */
gchar *take_utf8(LPWSTR wide) {
  if (!wide)
    return nullptr;
  gchar *text = g_utf16_to_utf8(reinterpret_cast<gunichar2 *>(wide), -1,
                                nullptr, nullptr, nullptr);
  CoTaskMemFree(wide);
  return text;
}

LPWSTR dup_co_task(const std::wstring &value) {
  size_t size = (value.size() + 1) * sizeof(wchar_t);
  auto copy = static_cast<LPWSTR>(CoTaskMemAlloc(size));
  if (copy)
    memcpy(copy, value.c_str(), size);
  return copy;
}

GError *error_from_hresult(HRESULT result, const gchar *what) {
  gchar *message = g_win32_error_message(result);
  GError *error = g_error_new(G_IO_ERROR, G_IO_ERROR_FAILED, "%s: %s", what,
                              message);
  g_free(message);
  return error;
}

/* A COM object for one of WebView2's single-method handler interfaces, calling
 * a C++ function. */
template <typename Interface, typename Method = decltype(&Interface::Invoke)>
class Handler;

template <typename Interface, typename Class, typename... Args>
class Handler<Interface, HRESULT (STDMETHODCALLTYPE Class::*)(Args...)> final
    : public Interface {
public:
  Handler(REFIID iid, std::function<HRESULT(Args...)> function)
      : iid_(iid), function_(std::move(function)) {}

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid,
                                           void **object) override {
    if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, iid_)) {
      *object = static_cast<Interface *>(this);
      AddRef();
      return S_OK;
    }
    *object = nullptr;
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
  ULONG STDMETHODCALLTYPE Release() override {
    ULONG references = --references_;
    if (references == 0)
      delete this;
    return references;
  }
  HRESULT STDMETHODCALLTYPE Invoke(Args... args) override {
    return function_(args...);
  }

private:
  ULONG references_ = 1;
  IID iid_;
  std::function<HRESULT(Args...)> function_;
};

/* ComPtr::As() needs __uuidof(), which WebView2.h does not declare for
 * MinGW; its IID_ constants work everywhere. */
template <typename Interface, typename Source>
HRESULT query(const ComPtr<Source> &source, REFIID iid,
              ComPtr<Interface> &target) {
  if (!source)
    return E_POINTER;
  return source->QueryInterface(
      iid, reinterpret_cast<void **>(target.ReleaseAndGetAddressOf()));
}

template <typename Interface, typename Function>
ComPtr<Interface> make_handler(REFIID iid, Function &&function) {
  ComPtr<Interface> handler;
  handler.Attach(new Handler<Interface>(iid, std::forward<Function>(function)));
  return handler;
}

/* A read-only IStream over a GInputStream. It aggregates the free-threaded
 * marshaler, so WebView2 reads it on its own thread instead of the UI one. */
class InputStream final : public IStream {
public:
  static ComPtr<IStream> create(GInputStream *stream, gint64 length) {
    ComPtr<InputStream> wrapper;
    wrapper.Attach(new InputStream(stream, length));
    if (FAILED(CoCreateFreeThreadedMarshaler(wrapper.Get(),
                                             &wrapper->marshaler_)))
      return nullptr;
    return ComPtr<IStream>(wrapper.Get());
  }

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid,
                                           void **object) override {
    if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_IStream) ||
        IsEqualIID(riid, IID_ISequentialStream)) {
      *object = static_cast<IStream *>(this);
      AddRef();
      return S_OK;
    }
    if (IsEqualIID(riid, IID_IMarshal) && marshaler_)
      return marshaler_->QueryInterface(riid, object);
    *object = nullptr;
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override {
    return g_atomic_int_add(&references_, 1) + 1;
  }
  ULONG STDMETHODCALLTYPE Release() override {
    if (!g_atomic_int_dec_and_test(&references_))
      return 1;
    delete this;
    return 0;
  }

  HRESULT STDMETHODCALLTYPE Read(void *buffer, ULONG size,
                                 ULONG *read) override {
    gsize total = 0;
    GError *error = nullptr;
    gboolean ok = g_input_stream_read_all(stream_, buffer, size, &total,
                                          nullptr, &error);
    if (read)
      *read = static_cast<ULONG>(total);
    if (!ok) {
      g_debug("Reading a web resource failed: %s", error->message);
      g_error_free(error);
      return E_FAIL;
    }
    return total < size ? S_FALSE : S_OK;
  }
  HRESULT STDMETHODCALLTYPE Write(const void *, ULONG, ULONG *) override {
    return STG_E_ACCESSDENIED;
  }
  HRESULT STDMETHODCALLTYPE Seek(LARGE_INTEGER move, DWORD origin,
                                 ULARGE_INTEGER *position) override {
    if (!G_IS_SEEKABLE(stream_) ||
        !g_seekable_can_seek(G_SEEKABLE(stream_))) {
      if (origin == STREAM_SEEK_CUR && move.QuadPart == 0 && position)
        position->QuadPart = 0;
      return origin == STREAM_SEEK_CUR && move.QuadPart == 0
                 ? S_OK : STG_E_INVALIDFUNCTION;
    }
    GSeekType type = origin == STREAM_SEEK_SET ? G_SEEK_SET
                     : origin == STREAM_SEEK_END ? G_SEEK_END : G_SEEK_CUR;
    if (!g_seekable_seek(G_SEEKABLE(stream_), move.QuadPart, type, nullptr,
                         nullptr))
      return STG_E_INVALIDFUNCTION;
    if (position)
      position->QuadPart = g_seekable_tell(G_SEEKABLE(stream_));
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE SetSize(ULARGE_INTEGER) override {
    return STG_E_ACCESSDENIED;
  }
  HRESULT STDMETHODCALLTYPE CopyTo(IStream *, ULARGE_INTEGER, ULARGE_INTEGER *,
                                   ULARGE_INTEGER *) override {
    return E_NOTIMPL;
  }
  HRESULT STDMETHODCALLTYPE Commit(DWORD) override { return S_OK; }
  HRESULT STDMETHODCALLTYPE Revert() override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE LockRegion(ULARGE_INTEGER, ULARGE_INTEGER,
                                       DWORD) override {
    return STG_E_INVALIDFUNCTION;
  }
  HRESULT STDMETHODCALLTYPE UnlockRegion(ULARGE_INTEGER, ULARGE_INTEGER,
                                         DWORD) override {
    return STG_E_INVALIDFUNCTION;
  }
  HRESULT STDMETHODCALLTYPE Stat(STATSTG *stat, DWORD) override {
    memset(stat, 0, sizeof *stat);
    stat->type = STGTY_STREAM;
    stat->cbSize.QuadPart = length_ >= 0 ? length_ : 0;
    return length_ >= 0 ? S_OK : STG_E_INVALIDFUNCTION;
  }
  HRESULT STDMETHODCALLTYPE Clone(IStream **) override { return E_NOTIMPL; }

private:
  InputStream(GInputStream *stream, gint64 length)
      : stream_(G_INPUT_STREAM(g_object_ref(stream))), length_(length) {}
  ~InputStream() {
    g_input_stream_close(stream_, nullptr, nullptr);
    g_object_unref(stream_);
  }

  gint references_ = 1;
  GInputStream *stream_;
  gint64 length_;
  ComPtr<IUnknown> marshaler_;
};

/* The custom schemes every view serves. */
class SchemeRegistration final : public ICoreWebView2CustomSchemeRegistration {
public:
  SchemeRegistration(std::wstring name, bool has_authority)
      : name_(std::move(name)), has_authority_(has_authority) {}

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid,
                                           void **object) override {
    if (IsEqualIID(riid, IID_IUnknown) ||
        IsEqualIID(riid, IID_ICoreWebView2CustomSchemeRegistration)) {
      *object = static_cast<ICoreWebView2CustomSchemeRegistration *>(this);
      AddRef();
      return S_OK;
    }
    *object = nullptr;
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
  ULONG STDMETHODCALLTYPE Release() override {
    ULONG references = --references_;
    if (references == 0)
      delete this;
    return references;
  }

  HRESULT STDMETHODCALLTYPE get_SchemeName(LPWSTR *name) override {
    *name = dup_co_task(name_);
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE get_TreatAsSecure(BOOL *value) override {
    *value = TRUE;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE put_TreatAsSecure(BOOL) override {
    return E_NOTIMPL;
  }
  /* Only pages served by the application may request these schemes; remote
   * frames embedded in a note cannot read the vault. */
  HRESULT STDMETHODCALLTYPE GetAllowedOrigins(UINT32 *count,
                                              LPWSTR **origins) override {
    std::vector<std::wstring> allowed;
    for (const gchar *const *scheme = pdfv_web_view_get_uri_schemes();
         *scheme; scheme++) {
      gchar *origin = g_strdup_printf("%s://*", *scheme);
      allowed.push_back(to_wide(origin));
      g_free(origin);
    }
    *count = static_cast<UINT32>(allowed.size());
    *origins = static_cast<LPWSTR *>(CoTaskMemAlloc(
        sizeof(LPWSTR) * std::max<size_t>(allowed.size(), 1)));
    for (size_t i = 0; i < allowed.size(); i++)
      (*origins)[i] = dup_co_task(allowed[i]);
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE SetAllowedOrigins(UINT32, LPCWSTR *) override {
    return E_NOTIMPL;
  }
  HRESULT STDMETHODCALLTYPE get_HasAuthorityComponent(BOOL *value) override {
    *value = has_authority_;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE put_HasAuthorityComponent(BOOL) override {
    return E_NOTIMPL;
  }

private:
  ULONG references_ = 1;
  std::wstring name_;
  BOOL has_authority_;
};

class EnvironmentOptions final : public ICoreWebView2EnvironmentOptions,
                                 public ICoreWebView2EnvironmentOptions4 {
public:
  EnvironmentOptions() {
    for (const gchar *const *scheme = pdfv_web_view_get_uri_schemes();
         *scheme; scheme++) {
      /* app://editor/... has a host; vault:///path does not. Chromium would
       * otherwise read the first path segment of vault:///a/b as a host. */
      ComPtr<ICoreWebView2CustomSchemeRegistration> registration;
      registration.Attach(new SchemeRegistration(
          to_wide(*scheme), g_str_equal(*scheme, "app")));
      schemes_.push_back(registration);
    }
  }

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid,
                                           void **object) override {
    if (IsEqualIID(riid, IID_IUnknown) ||
        IsEqualIID(riid, IID_ICoreWebView2EnvironmentOptions)) {
      *object = static_cast<ICoreWebView2EnvironmentOptions *>(this);
    } else if (IsEqualIID(riid, IID_ICoreWebView2EnvironmentOptions4)) {
      *object = static_cast<ICoreWebView2EnvironmentOptions4 *>(this);
    } else {
      *object = nullptr;
      return E_NOINTERFACE;
    }
    AddRef();
    return S_OK;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
  ULONG STDMETHODCALLTYPE Release() override {
    ULONG references = --references_;
    if (references == 0)
      delete this;
    return references;
  }

  HRESULT STDMETHODCALLTYPE get_AdditionalBrowserArguments(
      LPWSTR *value) override {
    *value = dup_co_task(L"");
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE put_AdditionalBrowserArguments(LPCWSTR) override {
    return E_NOTIMPL;
  }
  HRESULT STDMETHODCALLTYPE get_Language(LPWSTR *value) override {
    *value = dup_co_task(L"");
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE put_Language(LPCWSTR) override {
    return E_NOTIMPL;
  }
  /* The oldest runtime providing every interface used below. Evergreen
   * runtimes update themselves and are far newer in practice. */
  HRESULT STDMETHODCALLTYPE get_TargetCompatibleBrowserVersion(
      LPWSTR *value) override {
    *value = dup_co_task(L"120.0.2210.55");
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE put_TargetCompatibleBrowserVersion(
      LPCWSTR) override {
    return E_NOTIMPL;
  }
  HRESULT STDMETHODCALLTYPE get_AllowSingleSignOnUsingOSPrimaryAccount(
      BOOL *allow) override {
    *allow = FALSE;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE put_AllowSingleSignOnUsingOSPrimaryAccount(
      BOOL) override {
    return E_NOTIMPL;
  }

  HRESULT STDMETHODCALLTYPE GetCustomSchemeRegistrations(
      UINT32 *count,
      ICoreWebView2CustomSchemeRegistration ***registrations) override {
    *count = static_cast<UINT32>(schemes_.size());
    *registrations = static_cast<ICoreWebView2CustomSchemeRegistration **>(
        CoTaskMemAlloc(sizeof(void *) * std::max<size_t>(schemes_.size(), 1)));
    for (size_t i = 0; i < schemes_.size(); i++) {
      (*registrations)[i] = schemes_[i].Get();
      schemes_[i]->AddRef();
    }
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE SetCustomSchemeRegistrations(
      UINT32, ICoreWebView2CustomSchemeRegistration **) override {
    return E_NOTIMPL;
  }

private:
  ULONG references_ = 1;
  std::vector<ComPtr<ICoreWebView2CustomSchemeRegistration>> schemes_;
};

/* One environment, and therefore one browser process, serves every view. */
struct Environment {
  enum class State { NONE, CREATING, READY, FAILED } state = State::NONE;
  ComPtr<ICoreWebView2Environment> environment;
  HRESULT error = S_OK;
  std::vector<std::function<void(ICoreWebView2Environment *, HRESULT)>>
      waiting;
};

Environment &shared_environment() {
  static Environment environment;
  return environment;
}

std::wstring user_data_folder() {
  gchar *folder = g_build_filename(g_get_user_data_dir(), "phi-pdf-viewer",
                                   "WebView2", NULL);
  std::wstring result = to_wide(folder);
  g_free(folder);
  return result;
}

void with_environment(
    std::function<void(ICoreWebView2Environment *, HRESULT)> callback) {
  Environment &shared = shared_environment();
  if (shared.state == Environment::State::READY) {
    callback(shared.environment.Get(), S_OK);
    return;
  }
  if (shared.state == Environment::State::FAILED) {
    callback(nullptr, shared.error);
    return;
  }
  shared.waiting.push_back(std::move(callback));
  if (shared.state == Environment::State::CREATING)
    return;
  shared.state = Environment::State::CREATING;

  /* WebView2 requires a single-threaded apartment on the UI thread. GDK may
   * have initialized one for drag and drop already. */
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  ComPtr<ICoreWebView2EnvironmentOptions> options;
  options.Attach(new EnvironmentOptions());
  auto finish = [](HRESULT result, ICoreWebView2Environment *environment) {
    Environment &shared = shared_environment();
    if (SUCCEEDED(result) && environment) {
      shared.state = Environment::State::READY;
      shared.environment = environment;
    } else {
      shared.state = Environment::State::FAILED;
      shared.error = FAILED(result) ? result : E_FAIL;
    }
    auto waiting = std::move(shared.waiting);
    shared.waiting.clear();
    for (auto &callback : waiting)
      callback(shared.environment.Get(), shared.error);
    return S_OK;
  };
  /* The loader ships next to the executable and has no import library. */
  using CreateEnvironment = decltype(&CreateCoreWebView2EnvironmentWithOptions);
  static HMODULE loader = LoadLibraryW(L"WebView2Loader.dll");
  auto create = loader
      ? reinterpret_cast<CreateEnvironment>(reinterpret_cast<void *>(
            GetProcAddress(loader, "CreateCoreWebView2EnvironmentWithOptions")))
      : nullptr;
  if (!create) {
    finish(HRESULT_FROM_WIN32(GetLastError()), nullptr);
    return;
  }
  HRESULT result = create(
      nullptr, user_data_folder().c_str(), options.Get(),
      make_handler<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
          IID_ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler,
          finish)
          .Get());
  if (FAILED(result))
    finish(result, nullptr);
}

/* Maps WebView2 context menu entries to the portable actions. */
const struct {
  const wchar_t *name;
  PdfvWebContextMenuAction action;
} context_actions[] = {
    {L"back", PDFV_WEB_CONTEXT_MENU_ACTION_GO_BACK},
    {L"forward", PDFV_WEB_CONTEXT_MENU_ACTION_GO_FORWARD},
    {L"reload", PDFV_WEB_CONTEXT_MENU_ACTION_RELOAD},
    {L"cut", PDFV_WEB_CONTEXT_MENU_ACTION_CUT},
    {L"copy", PDFV_WEB_CONTEXT_MENU_ACTION_COPY},
    {L"paste", PDFV_WEB_CONTEXT_MENU_ACTION_PASTE},
    {L"selectAll", PDFV_WEB_CONTEXT_MENU_ACTION_SELECT_ALL},
    {L"emoji", PDFV_WEB_CONTEXT_MENU_ACTION_INSERT_EMOJI},
    {L"copyImage", PDFV_WEB_CONTEXT_MENU_ACTION_COPY_IMAGE_TO_CLIPBOARD},
    {L"copyImageLocation",
     PDFV_WEB_CONTEXT_MENU_ACTION_COPY_IMAGE_URL_TO_CLIPBOARD},
    {L"openImageInNewWindow",
     PDFV_WEB_CONTEXT_MENU_ACTION_OPEN_IMAGE_IN_NEW_WINDOW},
    {L"saveImageAs", PDFV_WEB_CONTEXT_MENU_ACTION_DOWNLOAD_IMAGE_TO_DISK},
};

/* Browser features the application does not offer, as on WebKitGTK. */
const wchar_t *const hidden_context_items[] = {
    L"saveAs", L"print", L"createQrCode", L"share", L"webCapture",
    L"openLinkInNewWindow", L"saveLinkAs", L"copyLinkToHighlight",
};

/* Keys that do not produce text, as GDK names them. */
const struct {
  UINT virtual_key;
  guint keyval;
} special_keys[] = {
    {VK_BACK, GDK_KEY_BackSpace},   {VK_TAB, GDK_KEY_Tab},
    {VK_RETURN, GDK_KEY_Return},    {VK_ESCAPE, GDK_KEY_Escape},
    {VK_SPACE, GDK_KEY_space},      {VK_PRIOR, GDK_KEY_Page_Up},
    {VK_NEXT, GDK_KEY_Page_Down},   {VK_END, GDK_KEY_End},
    {VK_HOME, GDK_KEY_Home},        {VK_LEFT, GDK_KEY_Left},
    {VK_UP, GDK_KEY_Up},            {VK_RIGHT, GDK_KEY_Right},
    {VK_DOWN, GDK_KEY_Down},        {VK_INSERT, GDK_KEY_Insert},
    {VK_DELETE, GDK_KEY_Delete},    {VK_ADD, GDK_KEY_KP_Add},
    {VK_SUBTRACT, GDK_KEY_KP_Subtract}, {VK_MULTIPLY, GDK_KEY_KP_Multiply},
    {VK_DIVIDE, GDK_KEY_KP_Divide}, {VK_DECIMAL, GDK_KEY_KP_Decimal},
};

} // namespace

/* The GObject ------------------------------------------------------------- */

#define PDFV_TYPE_WEBVIEW2_VIEW (pdfv_webview2_view_get_type())
G_DECLARE_FINAL_TYPE(PdfvWebView2View, pdfv_webview2_view, PDFV,
                     WEBVIEW2_VIEW, PdfvWebView)

namespace {
struct Host;
}

struct _PdfvWebView2View {
  PdfvWebView parent_instance;
  std::shared_ptr<Host> *host;
};

G_DEFINE_FINAL_TYPE(PdfvWebView2View, pdfv_webview2_view, PDFV_TYPE_WEB_VIEW)

extern "C" GType pdfv_web_view_backend_get_type(void) {
  return PDFV_TYPE_WEBVIEW2_VIEW;
}

namespace {

const wchar_t host_class_name[] = L"PhiWebViewHost";
const gchar key_channel[] = "\x01key";

const gchar host_script[] =
    "(() => {"
    "  if (window !== window.top || !window.chrome?.webview) return;"
    "  const webview = window.chrome.webview;"
    "  window.phiHost = Object.freeze({"
    "    postMessage(channel, message) {"
    "      webview.postMessage([String(channel), String(message)]);"
    "    },"
    /* Chromium draws Windows scrollbars, not GTK's. */
    "    toolkitScrollbars: false,"
    "  });"
    /* Report shortcuts the page left alone, once every listener had its
     * chance, so GTK can handle them in its bubble phase. */
    "  window.addEventListener('keydown', (event) => {"
    "    if (event.isComposing ||"
    "        ['Control', 'Shift', 'Alt', 'Meta'].includes(event.key) ||"
    "        !(event.ctrlKey || event.altKey ||"
    "        event.metaKey || /^F\\d+$/.test(event.key))) return;"
    "    setTimeout(() => {"
    "      if (event.defaultPrevented) return;"
    "      webview.postMessage(['\\u0001key', JSON.stringify({"
    "        keyCode: event.keyCode, shift: event.shiftKey,"
    "        control: event.ctrlKey, alt: event.altKey,"
    "        meta: event.metaKey })]);"
    "    });"
    "  });"
    "})();";

struct Host {
  PdfvWebView2View *view = nullptr; /* NULL once disposed */
  HWND window = nullptr;            /* child of the GTK toplevel */
  HWND toplevel = nullptr;
  ComPtr<ICoreWebView2Controller> controller;
  ComPtr<ICoreWebView2> webview;
  bool creating = false;
  bool ready = false;
  bool failed = false;
  bool has_native_focus = false;
  bool developer_extras = false;
  bool have_background = false;
  COREWEBVIEW2_COLOR background{255, 255, 255, 255};
  /* Operations waiting for the page; @drop answers them if it never loads. */
  struct Pending {
    std::function<void()> run;
    std::function<void(GError *)> drop;
  };
  std::vector<Pending> pending;
  gulong after_paint_handler = 0;
  GdkFrameClock *frame_clock = nullptr;
  /* Last geometry applied, to avoid redundant window calls. */
  RECT applied_clip{0, 0, 0, 0};
  RECT applied_bounds{0, 0, 0, 0};
  bool applied_visible = false;
  bool applied_region = false;
  double applied_scale = 0;
  /* Snapshot drawn by GTK where widgets cover the page. It is taken once per
   * episode of the page being covered. */
  GdkTexture *snapshot = nullptr;
  bool occluded = false;
  bool snapshot_requested = false;

  ~Host() {
    drop_pending("The web view was destroyed");
    g_clear_object(&snapshot);
  }

  void when_ready(std::function<void()> run,
                  std::function<void(GError *)> drop = nullptr) {
    if (ready) {
      run();
    } else if (failed) {
      if (drop)
        drop(g_error_new_literal(G_IO_ERROR, G_IO_ERROR_NOT_INITIALIZED,
                                 "The web view could not be started"));
    } else {
      pending.push_back(Pending{std::move(run), std::move(drop)});
    }
  }

  void drop_pending(const gchar *reason) {
    auto dropped = std::move(pending);
    pending.clear();
    for (auto &operation : dropped) {
      if (operation.drop)
        operation.drop(g_error_new_literal(G_IO_ERROR, G_IO_ERROR_CANCELLED,
                                           reason));
    }
  }
};

/* Toplevel window integration --------------------------------------------- */

/* Views per toplevel, to notify them about moves and activation. */
GHashTable *toplevel_views; /* HWND -> GPtrArray of Host* */

std::shared_ptr<Host> host_of(PdfvWebView2View *view) {
  return view && view->host ? *view->host : nullptr;
}

gboolean move_focus_idle(gpointer data) {
  auto weak = static_cast<std::weak_ptr<Host> *>(data);
  auto host = weak->lock();
  if (host && host->view && host->controller && host->applied_visible &&
      gtk_widget_has_focus(GTK_WIDGET(host->view)))
    host->controller->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
  delete weak;
  return G_SOURCE_REMOVE;
}

/* Gives the keyboard focus to the page once the current event is handled:
 * a page that handles a shortcut, such as Ctrl+Tab switching tabs, takes
 * the focus back when it is done with the key. */
void move_focus_into(const std::shared_ptr<Host> &host) {
  if (host->controller)
    g_idle_add(move_focus_idle, new std::weak_ptr<Host>(host));
}

/* A hidden window keeps the keyboard focus, so typing would go to a page
 * that is no longer shown, as after switching tabs. Hand the focus back to
 * the toplevel when hiding the page. */
void hide_host(Host *host) {
  if (!host->window)
    return;
  HWND focus = GetFocus();
  if (host->toplevel &&
      (host->has_native_focus ||
       (focus && (focus == host->window || IsChild(host->window, focus)))))
    SetFocus(host->toplevel);
  host->has_native_focus = false;
  if (host->controller)
    host->controller->put_IsVisible(FALSE);
  ShowWindow(host->window, SW_HIDE);
  host->applied_visible = false;
}

gboolean refocus_after_activation(gpointer data) {
  HWND toplevel = static_cast<HWND>(data);
  GPtrArray *hosts = toplevel_views
      ? static_cast<GPtrArray *>(g_hash_table_lookup(toplevel_views, toplevel))
      : nullptr;
  for (guint i = 0; hosts && i < hosts->len; i++) {
    auto host = static_cast<Host *>(g_ptr_array_index(hosts, i));
    if (host->view && gtk_widget_has_focus(GTK_WIDGET(host->view)) &&
        GetForegroundWindow() == toplevel)
      move_focus_into(host_of(host->view));
  }
  return G_SOURCE_REMOVE;
}

LRESULT CALLBACK toplevel_subclass(HWND window, UINT message, WPARAM wparam,
                                   LPARAM lparam, UINT_PTR, DWORD_PTR) {
  switch (message) {
  case WM_KILLFOCUS:
    /* GTK considers its window active while GDK reports keyboard focus.
     * Focus moving into a web view stays within this window. */
    if (wparam && IsChild(window, reinterpret_cast<HWND>(wparam)))
      return 0;
    break;
  case WM_MOVE:
  case WM_WINDOWPOSCHANGED: {
    GPtrArray *hosts = toplevel_views
        ? static_cast<GPtrArray *>(g_hash_table_lookup(toplevel_views, window))
        : nullptr;
    for (guint i = 0; hosts && i < hosts->len; i++) {
      auto host = static_cast<Host *>(g_ptr_array_index(hosts, i));
      if (host->controller)
        host->controller->NotifyParentWindowPositionChanged();
    }
    break;
  }
  case WM_ACTIVATE:
    if (LOWORD(wparam) != WA_INACTIVE) {
      /* Windows gives the keyboard focus back to the toplevel itself;
       * return it to the web view if that is GTK's focus widget. */
      g_idle_add(refocus_after_activation, window);
    } else {
      /* A web view that has the focus loses it without the toplevel
       * hearing of it; tell GDK that the window lost the focus. */
      HWND focus = GetFocus();
      if (focus && IsChild(window, focus))
        DefSubclassProc(window, WM_KILLFOCUS, 0, 0);
    }
    break;
  case WM_NCDESTROY:
    RemoveWindowSubclass(window, toplevel_subclass, 0);
    break;
  }
  return DefSubclassProc(window, message, wparam, lparam);
}

void attach_to_toplevel(Host *host, HWND toplevel) {
  if (!toplevel_views)
    toplevel_views = g_hash_table_new(g_direct_hash, g_direct_equal);
  auto hosts = static_cast<GPtrArray *>(
      g_hash_table_lookup(toplevel_views, toplevel));
  if (!hosts) {
    hosts = g_ptr_array_new();
    g_hash_table_insert(toplevel_views, toplevel, hosts);
    /* The toplevel must not clip its children: GTK keeps drawing below the
     * web views, which WebView2 composites above, so a page with a
     * transparent background shows GTK's background. */
    SetWindowSubclass(toplevel, toplevel_subclass, 0, 0);
  }
  g_ptr_array_add(hosts, host);
  host->toplevel = toplevel;
}

void detach_from_toplevel(Host *host) {
  if (!host->toplevel || !toplevel_views)
    return;
  auto hosts = static_cast<GPtrArray *>(
      g_hash_table_lookup(toplevel_views, host->toplevel));
  if (hosts) {
    g_ptr_array_remove(hosts, host);
    if (hosts->len == 0) {
      g_hash_table_remove(toplevel_views, host->toplevel);
      g_ptr_array_unref(hosts);
      if (IsWindow(host->toplevel))
        RemoveWindowSubclass(host->toplevel, toplevel_subclass, 0);
    }
  }
  host->toplevel = nullptr;
}

/* Hidden owner for host windows of unrealized views, which WebView2 requires
 * to stay parented while a tab moves between windows. */
HWND parking_window() {
  static HWND window;
  if (!window) {
    window = CreateWindowExW(0, host_class_name, L"", WS_POPUP, 0, 0, 0, 0,
                             nullptr, nullptr, GetModuleHandleW(nullptr),
                             nullptr);
  }
  return window;
}

void register_host_class() {
  static bool registered;
  if (registered)
    return;
  WNDCLASSEXW window_class{};
  window_class.cbSize = sizeof window_class;
  window_class.lpfnWndProc = DefWindowProcW;
  window_class.hInstance = GetModuleHandleW(nullptr);
  window_class.lpszClassName = host_class_name;
  window_class.hCursor =
      LoadCursorW(nullptr, reinterpret_cast<LPCWSTR>(IDC_ARROW));
  RegisterClassExW(&window_class);
  registered = true;
}

/* Keyboard shortcuts ------------------------------------------------------ */

struct KeyPress {
  guint keyval = 0;         /* without Shift applied */
  guint shifted_keyval = 0; /* as typed, if Shift produced a symbol */
  GdkModifierType modifiers = static_cast<GdkModifierType>(0);
};

guint keyval_for_character(UINT virtual_key, bool shift) {
  BYTE state[256] = {};
  if (shift)
    state[VK_SHIFT] = 0x80;
  wchar_t buffer[4];
  UINT scan = MapVirtualKeyW(virtual_key, MAPVK_VK_TO_VSC);
  /* Flag 0x4: do not change the keyboard state (dead keys). */
  int length = ToUnicodeEx(virtual_key, scan, state, buffer, 4, 0x4,
                           GetKeyboardLayout(0));
  if (length != 1)
    return 0;
  return gdk_unicode_to_keyval(buffer[0]);
}

KeyPress key_press_for(UINT virtual_key, bool shift, bool control, bool alt,
                       bool meta) {
  KeyPress press;
  for (const auto &special : special_keys) {
    if (special.virtual_key == virtual_key)
      press.keyval = special.keyval;
  }
  if (!press.keyval && virtual_key >= VK_F1 && virtual_key <= VK_F24)
    press.keyval = GDK_KEY_F1 + (virtual_key - VK_F1);
  if (!press.keyval && virtual_key >= VK_NUMPAD0 && virtual_key <= VK_NUMPAD9)
    press.keyval = GDK_KEY_KP_0 + (virtual_key - VK_NUMPAD0);
  if (!press.keyval) {
    press.keyval = gdk_keyval_to_lower(keyval_for_character(virtual_key,
                                                            false));
    if (shift)
      press.shifted_keyval = keyval_for_character(virtual_key, true);
  }
  guint modifiers = 0;
  if (shift)
    modifiers |= GDK_SHIFT_MASK;
  if (control)
    modifiers |= GDK_CONTROL_MASK;
  if (alt)
    modifiers |= GDK_ALT_MASK;
  if (meta)
    modifiers |= GDK_SUPER_MASK;
  press.modifiers = static_cast<GdkModifierType>(modifiers);
  return press;
}

bool trigger_matches(GtkShortcutTrigger *trigger, const KeyPress &press) {
  if (GTK_IS_ALTERNATIVE_TRIGGER(trigger)) {
    GtkAlternativeTrigger *alternative = GTK_ALTERNATIVE_TRIGGER(trigger);
    return trigger_matches(gtk_alternative_trigger_get_first(alternative),
                           press) ||
           trigger_matches(gtk_alternative_trigger_get_second(alternative),
                           press);
  }
  if (!GTK_IS_KEYVAL_TRIGGER(trigger) || !press.keyval)
    return false;
  GtkKeyvalTrigger *keyval_trigger = GTK_KEYVAL_TRIGGER(trigger);
  guint keyval = gdk_keyval_to_lower(
      gtk_keyval_trigger_get_keyval(keyval_trigger));
  GdkModifierType wanted = static_cast<GdkModifierType>(
      gtk_keyval_trigger_get_modifiers(keyval_trigger) &
      gtk_accelerator_get_default_mod_mask());
  if (keyval == press.keyval && wanted == press.modifiers)
    return true;
  /* Shift may be consumed by the symbol it produces, as in <Control>plus. */
  return press.shifted_keyval &&
         keyval == gdk_keyval_to_lower(press.shifted_keyval) &&
         wanted == (press.modifiers & ~GDK_SHIFT_MASK);
}

bool run_controller(GtkShortcutController *controller, GtkWidget *widget,
                    const KeyPress &press) {
  GListModel *shortcuts = G_LIST_MODEL(controller);
  guint n = g_list_model_get_n_items(shortcuts);
  for (guint i = 0; i < n; i++) {
    GtkShortcut *shortcut = GTK_SHORTCUT(g_list_model_get_item(shortcuts, i));
    bool handled = false;
    if (trigger_matches(gtk_shortcut_get_trigger(shortcut), press)) {
      handled = gtk_shortcut_action_activate(
          gtk_shortcut_get_action(shortcut), GTK_SHORTCUT_ACTION_EXCLUSIVE,
          widget, gtk_shortcut_get_arguments(shortcut));
    }
    g_object_unref(shortcut);
    if (handled)
      return true;
  }
  return false;
}

/* Runs the shortcut controllers @phase would reach on the way to @target. The
 * managers' controllers are skipped: they repeat controllers found on the
 * widgets themselves, which the callbacks need as their widget. */
bool run_shortcuts(GtkWidget *target, GtkPropagationPhase phase,
                   const KeyPress &press) {
  if (!press.keyval)
    return false;
  std::vector<GtkWidget *> chain;
  for (GtkWidget *widget = target; widget;
       widget = gtk_widget_get_parent(widget))
    chain.push_back(widget);
  if (phase == GTK_PHASE_CAPTURE)
    std::reverse(chain.begin(), chain.end());

  GtkApplication *application = nullptr;
  GtkRoot *root = gtk_widget_get_root(target);
  if (GTK_IS_WINDOW(root))
    application = gtk_window_get_application(GTK_WINDOW(root));

  for (GtkWidget *widget : chain) {
    GListModel *controllers = gtk_widget_observe_controllers(widget);
    guint n = g_list_model_get_n_items(controllers);
    bool handled = false;
    for (guint i = 0; i < n && !handled; i++) {
      auto controller = static_cast<GtkEventController *>(
          g_list_model_get_item(controllers, i));
      const gchar *name = gtk_event_controller_get_name(controller);
      if (GTK_IS_SHORTCUT_CONTROLLER(controller) &&
          gtk_event_controller_get_propagation_phase(controller) == phase &&
          !(name && g_str_has_prefix(name, "gtk-shortcut-manager"))) {
        handled = run_controller(GTK_SHORTCUT_CONTROLLER(controller), widget,
                                 press);
      }
      g_object_unref(controller);
    }
    g_object_unref(controllers);
    if (handled)
      return true;
  }

  /* Application accelerators, which GTK handles before any widget. */
  if (phase != GTK_PHASE_CAPTURE || !application)
    return false;
  guint keyvals[] = {press.keyval, press.shifted_keyval};
  for (guint k = 0; k < G_N_ELEMENTS(keyvals); k++) {
    if (!keyvals[k])
      continue;
    GdkModifierType modifiers = k == 0
        ? press.modifiers
        : static_cast<GdkModifierType>(press.modifiers & ~GDK_SHIFT_MASK);
    gchar *accel = gtk_accelerator_name(keyvals[k], modifiers);
    gchar **actions = gtk_application_get_actions_for_accel(application,
                                                            accel);
    g_free(accel);
    bool handled = false;
    for (gchar **action = actions; action && *action && !handled; action++) {
      gchar *name = nullptr;
      GVariant *target_value = nullptr;
      if (g_action_parse_detailed_name(*action, &name, &target_value,
                                       nullptr)) {
        handled = gtk_widget_activate_action_variant(target, name,
                                                     target_value);
      }
      g_free(name);
      g_clear_pointer(&target_value, g_variant_unref);
    }
    g_strfreev(actions);
    if (handled)
      return true;
  }
  return false;
}

/* Geometry ---------------------------------------------------------------- */

struct Rect {
  double x = 0, y = 0, width = 0, height = 0;
  bool empty() const { return width <= 0 || height <= 0; }
  Rect intersect(const Rect &other) const {
    double left = MAX(x, other.x), top = MAX(y, other.y);
    double right = MIN(x + width, other.x + other.width);
    double bottom = MIN(y + height, other.y + other.height);
    return Rect{left, top, MAX(right - left, 0), MAX(bottom - top, 0)};
  }
};

bool widget_rect(GtkWidget *widget, GtkWidget *native, Rect &rect) {
  graphene_rect_t bounds;
  if (!gtk_widget_compute_bounds(widget, native, &bounds))
    return false;
  rect = Rect{bounds.origin.x, bounds.origin.y, bounds.size.width,
              bounds.size.height};
  return true;
}

/* Widgets drawn above @view: later siblings of the view and its ancestors
 * that overlap it, such as overlays, toasts and dialogs. */
void collect_occluders(GtkWidget *view, GtkWidget *native, const Rect &area,
                       std::vector<Rect> &occluders, bool &fully_covered) {
  fully_covered = false;
  for (GtkWidget *child = view; child && child != native;
       child = gtk_widget_get_parent(child)) {
    for (GtkWidget *sibling = gtk_widget_get_next_sibling(child); sibling;
         sibling = gtk_widget_get_next_sibling(sibling)) {
      Rect rect;
      if (!gtk_widget_get_mapped(sibling) ||
          gtk_widget_get_opacity(sibling) <= 0.0 ||
          !widget_rect(sibling, native, rect))
        continue;
      Rect overlap = rect.intersect(area);
      if (overlap.empty())
        continue;
      if (overlap.width >= area.width - 1 && overlap.height >= area.height - 1)
        fully_covered = true;
      occluders.push_back(overlap);
    }
  }
}

RECT to_pixels(const Rect &rect, double scale_x, double scale_y) {
  RECT pixels;
  pixels.left = static_cast<LONG>(std::lround(rect.x * scale_x));
  pixels.top = static_cast<LONG>(std::lround(rect.y * scale_y));
  pixels.right =
      static_cast<LONG>(std::lround((rect.x + rect.width) * scale_x));
  pixels.bottom =
      static_cast<LONG>(std::lround((rect.y + rect.height) * scale_y));
  return pixels;
}

bool same_rect(const RECT &a, const RECT &b) {
  return a.left == b.left && a.top == b.top && a.right == b.right &&
         a.bottom == b.bottom;
}

void capture_snapshot(const std::shared_ptr<Host> &host);

void update_geometry(const std::shared_ptr<Host> &host) {
  if (!host->view || !host->controller || !host->window)
    return;
  GtkWidget *widget = GTK_WIDGET(host->view);
  GtkNative *native = gtk_widget_get_native(widget);
  GdkSurface *surface = native ? gtk_native_get_surface(native) : nullptr;
  bool visible = surface && gtk_widget_get_mapped(widget);

  Rect bounds, clip;
  std::vector<Rect> occluders;
  bool covered = false;
  double scale_x = 1, scale_y = 1, surface_x = 0, surface_y = 0;
  if (visible && widget_rect(widget, GTK_WIDGET(native), bounds)) {
    clip = bounds;
    for (GtkWidget *ancestor = gtk_widget_get_parent(widget);
         ancestor && ancestor != GTK_WIDGET(native);
         ancestor = gtk_widget_get_parent(ancestor)) {
      Rect rect;
      if (gtk_widget_get_overflow(ancestor) == GTK_OVERFLOW_HIDDEN &&
          widget_rect(ancestor, GTK_WIDGET(native), rect))
        clip = clip.intersect(rect);
    }
    collect_occluders(widget, GTK_WIDGET(native), clip, occluders, covered);
    gtk_native_get_surface_transform(native, &surface_x, &surface_y);
    RECT client;
    GetClientRect(host->toplevel, &client);
    int surface_width = gdk_surface_get_width(surface);
    int surface_height = gdk_surface_get_height(surface);
    if (surface_width > 0 && surface_height > 0) {
      scale_x = static_cast<double>(client.right) / surface_width;
      scale_y = static_cast<double>(client.bottom) / surface_height;
    }
    visible = !clip.empty();
  } else {
    visible = false;
  }

  bool occluded = visible && !occluders.empty();
  if (occluded && !host->occluded) {
    host->occluded = true;
    capture_snapshot(host);
  } else if (!occluded && host->occluded) {
    host->occluded = false;
    host->snapshot_requested = false;
    if (host->snapshot) {
      g_clear_object(&host->snapshot);
      gtk_widget_queue_draw(widget);
    }
  }
  /* A covered view stays visible until its snapshot can replace it. */
  if (covered && host->snapshot)
    visible = false;

  if (!visible) {
    if (host->applied_visible)
      hide_host(host.get());
    return;
  }

  Rect offset_bounds = bounds, offset_clip = clip;
  offset_bounds.x += surface_x;
  offset_bounds.y += surface_y;
  offset_clip.x += surface_x;
  offset_clip.y += surface_y;
  RECT clip_pixels = to_pixels(offset_clip, scale_x, scale_y);
  RECT bounds_pixels = to_pixels(offset_bounds, scale_x, scale_y);
  /* Controller bounds are relative to the host window at the clip. */
  OffsetRect(&bounds_pixels, -clip_pixels.left, -clip_pixels.top);

  if (!same_rect(clip_pixels, host->applied_clip)) {
    SetWindowPos(host->window, HWND_TOP, clip_pixels.left, clip_pixels.top,
                 clip_pixels.right - clip_pixels.left,
                 clip_pixels.bottom - clip_pixels.top,
                 SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    host->applied_clip = clip_pixels;
  }
  if (!same_rect(bounds_pixels, host->applied_bounds)) {
    host->controller->put_Bounds(bounds_pixels);
    host->applied_bounds = bounds_pixels;
  }
  ComPtr<ICoreWebView2Controller3> controller3;
  if (SUCCEEDED(query(host->controller, IID_ICoreWebView2Controller3,
                      controller3)) &&
      std::fabs(scale_x - host->applied_scale) > 0.001) {
    /* Render CSS pixels at GTK's scale, as WebKitGTK does, rather than at
     * the monitor's, which GTK may round. */
    controller3->put_RasterizationScale(scale_x);
    host->applied_scale = scale_x;
  }

  if (!occluders.empty() || host->applied_region) {
    HRGN region = nullptr;
    if (!occluders.empty()) {
      region = CreateRectRgn(0, 0, clip_pixels.right - clip_pixels.left,
                             clip_pixels.bottom - clip_pixels.top);
      for (const Rect &occluder : occluders) {
        Rect shifted = occluder;
        shifted.x += surface_x;
        shifted.y += surface_y;
        RECT hole = to_pixels(shifted, scale_x, scale_y);
        OffsetRect(&hole, -clip_pixels.left, -clip_pixels.top);
        HRGN hole_region = CreateRectRgnIndirect(&hole);
        CombineRgn(region, region, hole_region, RGN_DIFF);
        DeleteObject(hole_region);
      }
    }
    /* The window owns the region from here on. */
    SetWindowRgn(host->window, region, TRUE);
    host->applied_region = region != nullptr;
  }

  if (!host->applied_visible) {
    ShowWindow(host->window, SW_SHOWNOACTIVATE);
    host->controller->put_IsVisible(TRUE);
    host->applied_visible = true;
    /* A hidden view ignores focus requests, as when GTK focuses the page of
     * a tab that is being switched to. */
    if (gtk_widget_has_focus(widget))
      move_focus_into(host);
  }
}

void capture_snapshot(const std::shared_ptr<Host> &host) {
  if (!host->webview || host->snapshot_requested)
    return;
  ComPtr<IStream> stream;
  if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &stream)))
    return;
  host->snapshot_requested = true;
  auto weak = std::weak_ptr<Host>(host);
  HRESULT result = host->webview->CapturePreview(
      COREWEBVIEW2_CAPTURE_PREVIEW_IMAGE_FORMAT_PNG, stream.Get(),
      make_handler<ICoreWebView2CapturePreviewCompletedHandler>(
          IID_ICoreWebView2CapturePreviewCompletedHandler,
          [weak, stream](HRESULT error) -> HRESULT {
            auto host = weak.lock();
            if (!host || !host->occluded)
              return S_OK;
            HGLOBAL global = nullptr;
            if (SUCCEEDED(error) &&
                SUCCEEDED(GetHGlobalFromStream(stream.Get(), &global))) {
              gsize size = GlobalSize(global);
              gconstpointer data = GlobalLock(global);
              GBytes *bytes = g_bytes_new(data, size);
              GlobalUnlock(global);
              GdkTexture *texture = gdk_texture_new_from_bytes(bytes, nullptr);
              g_bytes_unref(bytes);
              if (texture) {
                g_set_object(&host->snapshot, texture);
                g_object_unref(texture);
              }
            }
            if (host->view) {
              gtk_widget_queue_draw(GTK_WIDGET(host->view));
              update_geometry(host);
            }
            return S_OK;
          })
          .Get());
  if (FAILED(result))
    g_debug("Could not capture the page");
}

void on_after_paint(GdkFrameClock *, gpointer data) {
  auto view = PDFV_WEBVIEW2_VIEW(data);
  update_geometry(host_of(view));
}

/* Scheme requests ---------------------------------------------------------- */

struct SchemeResponse {
  std::weak_ptr<Host> host;
  ComPtr<ICoreWebView2WebResourceRequestedEventArgs> args;
  ComPtr<ICoreWebView2Deferral> deferral;
};

void finish_scheme_request(gpointer data, GInputStream *stream, gint64 length,
                           const gchar *content_type, const GError *error) {
  auto response = static_cast<SchemeResponse *>(data);
  auto host = response->host.lock();
  ComPtr<ICoreWebView2Environment> environment =
      shared_environment().environment;
  if (host && environment) {
    ComPtr<ICoreWebView2WebResourceResponse> reply;
    if (stream) {
      gchar *headers = g_strdup_printf(
          "Content-Type: %s\r\nAccess-Control-Allow-Origin: *\r\n"
          "Cache-Control: no-cache",
          content_type ? content_type : "application/octet-stream");
      ComPtr<IStream> content = InputStream::create(stream, length);
      environment->CreateWebResourceResponse(content.Get(), 200, L"OK",
                                             to_wide(headers).c_str(),
                                             &reply);
      g_free(headers);
    } else {
      gboolean missing = error && g_error_matches(error, G_IO_ERROR,
                                                  G_IO_ERROR_NOT_FOUND);
      g_debug("Web resource failed: %s", error ? error->message : "unknown");
      environment->CreateWebResourceResponse(
          nullptr, missing ? 404 : 403, missing ? L"Not Found" : L"Forbidden",
          L"", &reply);
    }
    if (reply)
      response->args->put_Response(reply.Get());
  }
  response->deferral->Complete();
}

void free_scheme_response(gpointer data) {
  delete static_cast<SchemeResponse *>(data);
}

/* Context menus ------------------------------------------------------------ */

PdfvWebContextMenuAction action_for_item(ICoreWebView2ContextMenuItem *item,
                                         bool developer_extras,
                                         bool *hidden) {
  *hidden = false;
  COREWEBVIEW2_CONTEXT_MENU_ITEM_KIND kind;
  item->get_Kind(&kind);
  if (kind == COREWEBVIEW2_CONTEXT_MENU_ITEM_KIND_SEPARATOR)
    return PDFV_WEB_CONTEXT_MENU_ACTION_NONE;
  LPWSTR raw_name = nullptr;
  item->get_Name(&raw_name);
  std::wstring name = raw_name ? raw_name : L"";
  CoTaskMemFree(raw_name);
  for (const wchar_t *hidden_name : hidden_context_items) {
    if (name == hidden_name)
      *hidden = true;
  }
  if (name == L"inspectElement" && !developer_extras)
    *hidden = true;
  for (const auto &entry : context_actions) {
    if (name == entry.name)
      return entry.action;
  }
  return PDFV_WEB_CONTEXT_MENU_ACTION_OTHER;
}

void release_com(gpointer object) {
  static_cast<IUnknown *>(object)->Release();
}

/* Builds the WebView2 item that shows @item, or returns NULL. */
ComPtr<ICoreWebView2ContextMenuItem> native_item_for(
    PdfvWebContextMenuItem *item, ICoreWebView2Environment9 *environment,
    const std::vector<std::pair<PdfvWebContextMenuAction,
                                ComPtr<ICoreWebView2ContextMenuItem>>> &stock) {
  auto native = static_cast<ICoreWebView2ContextMenuItem *>(
      pdfv_web_context_menu_item_get_native(item));
  if (native)
    return native;
  ComPtr<ICoreWebView2ContextMenuItem> created;
  if (pdfv_web_context_menu_item_is_separator(item)) {
    environment->CreateContextMenuItem(
        L"", nullptr, COREWEBVIEW2_CONTEXT_MENU_ITEM_KIND_SEPARATOR, &created);
    return created;
  }
  GAction *action = pdfv_web_context_menu_item_get_gaction(item);
  if (action) {
    environment->CreateContextMenuItem(
        to_wide(pdfv_web_context_menu_item_get_label(item)).c_str(), nullptr,
        COREWEBVIEW2_CONTEXT_MENU_ITEM_KIND_COMMAND, &created);
    if (!created)
      return nullptr;
    created->put_IsEnabled(g_action_get_enabled(action));
    GVariant *target = pdfv_web_context_menu_item_get_target(item);
    g_object_ref(action);
    if (target)
      g_variant_ref(target);
    /* The handler owns the action and target until the menu is gone. */
    auto hold = std::shared_ptr<void>(nullptr, [action, target](void *) {
      g_object_unref(action);
      if (target)
        g_variant_unref(target);
    });
    EventRegistrationToken token;
    created->add_CustomItemSelected(
        make_handler<ICoreWebView2CustomItemSelectedEventHandler>(
            IID_ICoreWebView2CustomItemSelectedEventHandler,
            [action, target, hold](ICoreWebView2ContextMenuItem *,
                                   IUnknown *) -> HRESULT {
              g_action_activate(action, target);
              return S_OK;
            })
            .Get(),
        &token);
    return created;
  }
  /* WebView2 cannot create its own entries; reuse one it offered. */
  PdfvWebContextMenuAction wanted =
      pdfv_web_context_menu_item_get_stock_action(item);
  for (const auto &entry : stock) {
    if (entry.first == wanted)
      return entry.second;
  }
  return nullptr;
}

void show_context_menu(const std::shared_ptr<Host> &host,
                       ICoreWebView2ContextMenuRequestedEventArgs *args) {
  ComPtr<ICoreWebView2ContextMenuItemCollection> items;
  ComPtr<ICoreWebView2ContextMenuTarget> target;
  ComPtr<ICoreWebView2Environment9> environment;
  if (FAILED(args->get_MenuItems(&items)) ||
      FAILED(args->get_ContextMenuTarget(&target)) ||
      FAILED(query(shared_environment().environment,
                   IID_ICoreWebView2Environment9, environment)))
    return;

  gchar *image_uri = nullptr;
  COREWEBVIEW2_CONTEXT_MENU_TARGET_KIND kind;
  if (SUCCEEDED(target->get_Kind(&kind)) &&
      kind == COREWEBVIEW2_CONTEXT_MENU_TARGET_KIND_IMAGE) {
    /* HasSourceUri is FALSE for images from custom schemes, although
     * SourceUri holds their address. */
    LPWSTR source = nullptr;
    target->get_SourceUri(&source);
    image_uri = take_utf8(source);
    if (image_uri && !*image_uri)
      g_clear_pointer(&image_uri, g_free);
  }
  PdfvWebContextMenu *menu = pdfv_web_context_menu_new(image_uri);
  g_free(image_uri);

  std::vector<std::pair<PdfvWebContextMenuAction,
                        ComPtr<ICoreWebView2ContextMenuItem>>> stock;
  UINT32 count = 0;
  items->get_Count(&count);
  for (UINT32 i = 0; i < count; i++) {
    ComPtr<ICoreWebView2ContextMenuItem> native;
    if (FAILED(items->GetValueAtIndex(i, &native)))
      continue;
    bool hidden = false;
    PdfvWebContextMenuAction action =
        action_for_item(native.Get(), host->developer_extras, &hidden);
    if (action != PDFV_WEB_CONTEXT_MENU_ACTION_NONE &&
        action != PDFV_WEB_CONTEXT_MENU_ACTION_OTHER)
      stock.emplace_back(action, native);
    if (hidden)
      continue;
    native->AddRef();
    pdfv_web_context_menu_append(
        menu, pdfv_web_context_menu_item_new_native(action, native.Get(),
                                                    release_com));
  }

  gboolean suppressed = pdfv_web_view_emit_context_menu(
      PDFV_WEB_VIEW(host->view), menu);
  std::vector<ComPtr<ICoreWebView2ContextMenuItem>> shown;
  if (!suppressed) {
    bool previous_separator = true;
    for (GList *at = pdfv_web_context_menu_get_items(menu); at;
         at = at->next) {
      auto item = static_cast<PdfvWebContextMenuItem *>(at->data);
      ComPtr<ICoreWebView2ContextMenuItem> native =
          native_item_for(item, environment.Get(), stock);
      if (!native)
        continue;
      COREWEBVIEW2_CONTEXT_MENU_ITEM_KIND item_kind;
      native->get_Kind(&item_kind);
      bool separator =
          item_kind == COREWEBVIEW2_CONTEXT_MENU_ITEM_KIND_SEPARATOR;
      if (separator && previous_separator)
        continue;
      previous_separator = separator;
      shown.push_back(native);
    }
    while (!shown.empty()) {
      COREWEBVIEW2_CONTEXT_MENU_ITEM_KIND item_kind;
      shown.back()->get_Kind(&item_kind);
      if (item_kind != COREWEBVIEW2_CONTEXT_MENU_ITEM_KIND_SEPARATOR)
        break;
      shown.pop_back();
    }
  }
  g_object_unref(menu);

  while (count > 0)
    items->RemoveValueAtIndex(--count);
  for (UINT32 i = 0; i < shown.size(); i++)
    items->InsertValueAtIndex(i, shown[i].Get());
  if (shown.empty())
    args->put_Handled(TRUE);
}

/* Messages ----------------------------------------------------------------- */

void handle_unhandled_key(const std::shared_ptr<Host> &host,
                          const gchar *message) {
  JsonParser *parser = json_parser_new();
  if (json_parser_load_from_data(parser, message, -1, nullptr) &&
      JSON_NODE_HOLDS_OBJECT(json_parser_get_root(parser))) {
    JsonObject *key = json_node_get_object(json_parser_get_root(parser));
    KeyPress press = key_press_for(
        static_cast<UINT>(
            json_object_get_int_member_with_default(key, "keyCode", 0)),
        json_object_get_boolean_member_with_default(key, "shift", FALSE),
        json_object_get_boolean_member_with_default(key, "control", FALSE),
        json_object_get_boolean_member_with_default(key, "alt", FALSE),
        json_object_get_boolean_member_with_default(key, "meta", FALSE));
    run_shortcuts(GTK_WIDGET(host->view), GTK_PHASE_BUBBLE, press);
  }
  g_object_unref(parser);
}

void on_web_message(const std::shared_ptr<Host> &host,
                    ICoreWebView2WebMessageReceivedEventArgs *args) {
  LPWSTR raw_source = nullptr;
  args->get_Source(&raw_source);
  gchar *source = take_utf8(raw_source);
  bool trusted = source && g_str_has_prefix(source, "app://");
  g_free(source);
  LPWSTR raw_json = nullptr;
  if (!trusted || FAILED(args->get_WebMessageAsJson(&raw_json)))
    return;
  gchar *json = take_utf8(raw_json);
  JsonParser *parser = json_parser_new();
  if (json && json_parser_load_from_data(parser, json, -1, nullptr) &&
      JSON_NODE_HOLDS_ARRAY(json_parser_get_root(parser))) {
    JsonArray *pair = json_node_get_array(json_parser_get_root(parser));
    if (json_array_get_length(pair) == 2) {
      const gchar *channel = json_array_get_string_element(pair, 0);
      const gchar *message = json_array_get_string_element(pair, 1);
      if (channel && message && g_str_equal(channel, key_channel))
        handle_unhandled_key(host, message);
      else if (channel && message)
        pdfv_web_view_emit_script_message(PDFV_WEB_VIEW(host->view), channel,
                                          message);
    }
  }
  g_object_unref(parser);
  g_free(json);
}

/* Controller setup --------------------------------------------------------- */

void flush_pending(const std::shared_ptr<Host> &host) {
  host->ready = true;
  auto pending = std::move(host->pending);
  host->pending.clear();
  for (auto &operation : pending)
    operation.run();
}

void show_failure(const std::shared_ptr<Host> &host, HRESULT result) {
  host->failed = true;
  host->drop_pending("The web view could not be started");
  gchar *message = g_win32_error_message(result);
  g_warning("Could not start Microsoft Edge WebView2: %s", message);
  g_free(message);
  if (!host->view)
    return;
  GtkWidget *label = gtk_label_new(
      "The Microsoft Edge WebView2 Runtime is required to show this page.\n"
      "Reinstall Phi, or install the runtime from Microsoft.");
  gtk_label_set_justify(GTK_LABEL(label), GTK_JUSTIFY_CENTER);
  gtk_label_set_wrap(GTK_LABEL(label), TRUE);
  gtk_widget_add_css_class(label, "dim-label");
  gtk_widget_set_parent(label, GTK_WIDGET(host->view));
}

void configure_webview(const std::shared_ptr<Host> &host) {
  auto weak = std::weak_ptr<Host>(host);
  ICoreWebView2 *webview = host->webview.Get();
  EventRegistrationToken token;

  ComPtr<ICoreWebView2Settings> settings;
  if (SUCCEEDED(webview->get_Settings(&settings))) {
    settings->put_AreDevToolsEnabled(host->developer_extras);
    settings->put_IsStatusBarEnabled(FALSE);
    settings->put_IsZoomControlEnabled(FALSE);
    settings->put_IsBuiltInErrorPageEnabled(FALSE);
    ComPtr<ICoreWebView2Settings3> settings3;
    if (SUCCEEDED(query(settings, IID_ICoreWebView2Settings3, settings3)))
      settings3->put_AreBrowserAcceleratorKeysEnabled(FALSE);
    ComPtr<ICoreWebView2Settings4> settings4;
    if (SUCCEEDED(query(settings, IID_ICoreWebView2Settings4, settings4))) {
      settings4->put_IsGeneralAutofillEnabled(FALSE);
      settings4->put_IsPasswordAutosaveEnabled(FALSE);
    }
    ComPtr<ICoreWebView2Settings5> settings5;
    if (SUCCEEDED(query(settings, IID_ICoreWebView2Settings5, settings5)))
      settings5->put_IsPinchZoomEnabled(FALSE);
    ComPtr<ICoreWebView2Settings6> settings6;
    if (SUCCEEDED(query(settings, IID_ICoreWebView2Settings6, settings6)))
      settings6->put_IsSwipeNavigationEnabled(FALSE);
  }

  webview->add_WebMessageReceived(
      make_handler<ICoreWebView2WebMessageReceivedEventHandler>(
          IID_ICoreWebView2WebMessageReceivedEventHandler,
          [weak](ICoreWebView2 *,
                 ICoreWebView2WebMessageReceivedEventArgs *args) -> HRESULT {
            auto host = weak.lock();
            if (host && host->view)
              on_web_message(host, args);
            return S_OK;
          })
          .Get(),
      &token);

  auto decide = [weak](ICoreWebView2NavigationStartingEventArgs *args) {
    auto host = weak.lock();
    if (!host || !host->view)
      return S_OK;
    LPWSTR raw_uri = nullptr;
    BOOL user = FALSE;
    args->get_Uri(&raw_uri);
    args->get_IsUserInitiated(&user);
    gchar *uri = take_utf8(raw_uri);
    if (pdfv_web_view_emit_decide_navigation(PDFV_WEB_VIEW(host->view),
                                             uri ? uri : "", user, FALSE))
      args->put_Cancel(TRUE);
    g_free(uri);
    return S_OK;
  };
  webview->add_NavigationStarting(
      make_handler<ICoreWebView2NavigationStartingEventHandler>(
          IID_ICoreWebView2NavigationStartingEventHandler,
          [decide](ICoreWebView2 *,
                   ICoreWebView2NavigationStartingEventArgs *args) {
            return decide(args);
          })
          .Get(),
      &token);
  webview->add_FrameNavigationStarting(
      make_handler<ICoreWebView2NavigationStartingEventHandler>(
          IID_ICoreWebView2NavigationStartingEventHandler,
          [decide](ICoreWebView2 *,
                   ICoreWebView2NavigationStartingEventArgs *args) {
            return decide(args);
          })
          .Get(),
      &token);
  webview->add_NewWindowRequested(
      make_handler<ICoreWebView2NewWindowRequestedEventHandler>(
          IID_ICoreWebView2NewWindowRequestedEventHandler,
          [weak](ICoreWebView2 *,
                 ICoreWebView2NewWindowRequestedEventArgs *args) -> HRESULT {
            args->put_Handled(TRUE);
            auto host = weak.lock();
            if (!host || !host->view)
              return S_OK;
            LPWSTR raw_uri = nullptr;
            BOOL user = FALSE;
            args->get_Uri(&raw_uri);
            args->get_IsUserInitiated(&user);
            gchar *uri = take_utf8(raw_uri);
            pdfv_web_view_emit_decide_navigation(PDFV_WEB_VIEW(host->view),
                                                 uri ? uri : "", user, TRUE);
            g_free(uri);
            return S_OK;
          })
          .Get(),
      &token);
  webview->add_PermissionRequested(
      make_handler<ICoreWebView2PermissionRequestedEventHandler>(
          IID_ICoreWebView2PermissionRequestedEventHandler,
          [](ICoreWebView2 *,
             ICoreWebView2PermissionRequestedEventArgs *args) -> HRESULT {
            args->put_State(COREWEBVIEW2_PERMISSION_STATE_DENY);
            return S_OK;
          })
          .Get(),
      &token);

  ComPtr<ICoreWebView2_4> webview4;
  if (SUCCEEDED(query(host->webview, IID_ICoreWebView2_4, webview4))) {
    webview4->add_DownloadStarting(
        make_handler<ICoreWebView2DownloadStartingEventHandler>(
            IID_ICoreWebView2DownloadStartingEventHandler,
            [](ICoreWebView2 *,
               ICoreWebView2DownloadStartingEventArgs *args) -> HRESULT {
              /* Let the user choose where to save, as a browser would. */
              args->put_Handled(FALSE);
              return S_OK;
            })
            .Get(),
        &token);
  }

  ComPtr<ICoreWebView2_11> webview11;
  if (SUCCEEDED(query(host->webview, IID_ICoreWebView2_11, webview11))) {
    webview11->add_ContextMenuRequested(
        make_handler<ICoreWebView2ContextMenuRequestedEventHandler>(
            IID_ICoreWebView2ContextMenuRequestedEventHandler,
            [weak](ICoreWebView2 *,
                   ICoreWebView2ContextMenuRequestedEventArgs *args)
                -> HRESULT {
              auto host = weak.lock();
              if (host && host->view)
                show_context_menu(host, args);
              return S_OK;
            })
            .Get(),
        &token);
  }

  /* Serve the registered schemes. */
  auto on_resource = [weak](ICoreWebView2 *,
                            ICoreWebView2WebResourceRequestedEventArgs *args)
      -> HRESULT {
    auto host = weak.lock();
    ComPtr<ICoreWebView2WebResourceRequest> request;
    if (!host || !host->view || FAILED(args->get_Request(&request)))
      return S_OK;
    LPWSTR raw_uri = nullptr;
    request->get_Uri(&raw_uri);
    gchar *uri = take_utf8(raw_uri);
    auto response = new SchemeResponse();
    response->host = host;
    response->args = args;
    args->GetDeferral(&response->deferral);
    PdfvWebSchemeRequest *wrapped = pdfv_web_scheme_request_new(
        PDFV_WEB_VIEW(host->view), uri ? uri : "", finish_scheme_request,
        response, free_scheme_response);
    pdfv_web_view_handle_scheme_request(wrapped);
    g_object_unref(wrapped);
    g_free(uri);
    return S_OK;
  };
  webview->add_WebResourceRequested(
      make_handler<ICoreWebView2WebResourceRequestedEventHandler>(
          IID_ICoreWebView2WebResourceRequestedEventHandler, on_resource)
          .Get(),
      &token);
  ComPtr<ICoreWebView2_22> webview22;
  for (const gchar *const *scheme = pdfv_web_view_get_uri_schemes(); *scheme;
       scheme++) {
    gchar *filter = g_strdup_printf("%s:*", *scheme);
    if (SUCCEEDED(query(host->webview, IID_ICoreWebView2_22, webview22))) {
      webview22->AddWebResourceRequestedFilterWithRequestSourceKinds(
          to_wide(filter).c_str(), COREWEBVIEW2_WEB_RESOURCE_CONTEXT_ALL,
          COREWEBVIEW2_WEB_RESOURCE_REQUEST_SOURCE_KINDS_ALL);
    } else {
      webview->AddWebResourceRequestedFilter(
          to_wide(filter).c_str(), COREWEBVIEW2_WEB_RESOURCE_CONTEXT_ALL);
    }
    g_free(filter);
  }

  /* Focus and keyboard. */
  ICoreWebView2Controller *controller = host->controller.Get();
  controller->add_GotFocus(
      make_handler<ICoreWebView2FocusChangedEventHandler>(
          IID_ICoreWebView2FocusChangedEventHandler,
          [weak](ICoreWebView2Controller *, IUnknown *) -> HRESULT {
            auto host = weak.lock();
            if (!host || !host->view)
              return S_OK;
            host->has_native_focus = true;
            if (!gtk_widget_has_focus(GTK_WIDGET(host->view)))
              gtk_widget_grab_focus(GTK_WIDGET(host->view));
            return S_OK;
          })
          .Get(),
      &token);
  controller->add_LostFocus(
      make_handler<ICoreWebView2FocusChangedEventHandler>(
          IID_ICoreWebView2FocusChangedEventHandler,
          [weak](ICoreWebView2Controller *, IUnknown *) -> HRESULT {
            if (auto host = weak.lock())
              host->has_native_focus = false;
            return S_OK;
          })
          .Get(),
      &token);
  controller->add_MoveFocusRequested(
      make_handler<ICoreWebView2MoveFocusRequestedEventHandler>(
          IID_ICoreWebView2MoveFocusRequestedEventHandler,
          [weak](ICoreWebView2Controller *,
                 ICoreWebView2MoveFocusRequestedEventArgs *args) -> HRESULT {
            auto host = weak.lock();
            if (!host || !host->view)
              return S_OK;
            COREWEBVIEW2_MOVE_FOCUS_REASON reason;
            args->get_Reason(&reason);
            GtkWidget *widget = GTK_WIDGET(host->view);
            GtkRoot *root = gtk_widget_get_root(widget);
            SetFocus(host->toplevel);
            if (root) {
              gtk_widget_child_focus(
                  GTK_WIDGET(root),
                  reason == COREWEBVIEW2_MOVE_FOCUS_REASON_PREVIOUS
                      ? GTK_DIR_TAB_BACKWARD : GTK_DIR_TAB_FORWARD);
            }
            args->put_Handled(TRUE);
            return S_OK;
          })
          .Get(),
      &token);
  controller->add_AcceleratorKeyPressed(
      make_handler<ICoreWebView2AcceleratorKeyPressedEventHandler>(
          IID_ICoreWebView2AcceleratorKeyPressedEventHandler,
          [weak](ICoreWebView2Controller *,
                 ICoreWebView2AcceleratorKeyPressedEventArgs *args)
              -> HRESULT {
            auto host = weak.lock();
            COREWEBVIEW2_KEY_EVENT_KIND kind;
            UINT virtual_key = 0;
            if (!host || !host->view || FAILED(args->get_KeyEventKind(&kind)) ||
                (kind != COREWEBVIEW2_KEY_EVENT_KIND_KEY_DOWN &&
                 kind != COREWEBVIEW2_KEY_EVENT_KIND_SYSTEM_KEY_DOWN) ||
                FAILED(args->get_VirtualKey(&virtual_key)))
              return S_OK;
            KeyPress press = key_press_for(
                virtual_key, GetKeyState(VK_SHIFT) < 0,
                GetKeyState(VK_CONTROL) < 0, GetKeyState(VK_MENU) < 0,
                GetKeyState(VK_LWIN) < 0 || GetKeyState(VK_RWIN) < 0);
            if (run_shortcuts(GTK_WIDGET(host->view), GTK_PHASE_CAPTURE,
                              press))
              args->put_Handled(TRUE);
            return S_OK;
          })
          .Get(),
      &token);

  ComPtr<ICoreWebView2Controller2> controller2;
  if (host->have_background &&
      SUCCEEDED(query(host->controller, IID_ICoreWebView2Controller2,
                      controller2)))
    controller2->put_DefaultBackgroundColor(host->background);
  ComPtr<ICoreWebView2Controller3> controller3;
  if (SUCCEEDED(query(host->controller, IID_ICoreWebView2Controller3,
                      controller3))) {
    controller3->put_ShouldDetectMonitorScaleChanges(FALSE);
    controller3->put_BoundsMode(COREWEBVIEW2_BOUNDS_MODE_USE_RAW_PIXELS);
  }

  /* Pages may only run once window.phiHost exists. */
  webview->AddScriptToExecuteOnDocumentCreated(
      to_wide(host_script).c_str(),
      make_handler<
          ICoreWebView2AddScriptToExecuteOnDocumentCreatedCompletedHandler>(
          IID_ICoreWebView2AddScriptToExecuteOnDocumentCreatedCompletedHandler,
          [weak](HRESULT, LPCWSTR) -> HRESULT {
            auto host = weak.lock();
            if (host && host->view) {
              flush_pending(host);
              update_geometry(host);
              if (gtk_widget_has_focus(GTK_WIDGET(host->view)))
                move_focus_into(host);
            }
            return S_OK;
          })
          .Get());
}

void create_controller(const std::shared_ptr<Host> &host) {
  if (host->creating || host->controller || host->failed)
    return;
  host->creating = true;
  auto weak = std::weak_ptr<Host>(host);
  with_environment([weak](ICoreWebView2Environment *environment,
                          HRESULT error) {
    auto host = weak.lock();
    if (!host || !host->view)
      return;
    if (!environment) {
      show_failure(host, error);
      return;
    }
    environment->CreateCoreWebView2Controller(
        host->window,
        make_handler<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
            IID_ICoreWebView2CreateCoreWebView2ControllerCompletedHandler,
            [weak](HRESULT result,
                   ICoreWebView2Controller *controller) -> HRESULT {
              auto host = weak.lock();
              if (!host)
                return S_OK;
              if (!host->view) {
                if (controller)
                  controller->Close();
                return S_OK;
              }
              if (FAILED(result) || !controller) {
                show_failure(host, FAILED(result) ? result : E_FAIL);
                return S_OK;
              }
              host->controller = controller;
              controller->get_CoreWebView2(&host->webview);
              configure_webview(host);
              return S_OK;
            })
            .Get());
  });
}

} // namespace

/* Widget ------------------------------------------------------------------- */

static void pdfv_webview2_view_realize(GtkWidget *widget) {
  GTK_WIDGET_CLASS(pdfv_webview2_view_parent_class)->realize(widget);
  auto host = host_of(PDFV_WEBVIEW2_VIEW(widget));
  GdkSurface *surface =
      gtk_native_get_surface(gtk_widget_get_native(widget));
  HWND toplevel = surface
      ? static_cast<HWND>(gdk_win32_surface_get_handle(surface)) : nullptr;
  if (!toplevel || !host->window)
    return;
  SetParent(host->window, toplevel);
  attach_to_toplevel(host.get(), toplevel);
  host->applied_clip = RECT{0, 0, 0, 0};
  host->applied_visible = false;
  if (host->controller)
    host->controller->NotifyParentWindowPositionChanged();

  host->frame_clock = gtk_widget_get_frame_clock(widget);
  if (host->frame_clock) {
    g_object_ref(host->frame_clock);
    host->after_paint_handler = g_signal_connect(
        host->frame_clock, "after-paint", G_CALLBACK(on_after_paint), widget);
  }
}

static void pdfv_webview2_view_unrealize(GtkWidget *widget) {
  auto host = host_of(PDFV_WEBVIEW2_VIEW(widget));
  if (host->frame_clock) {
    g_signal_handler_disconnect(host->frame_clock, host->after_paint_handler);
    g_clear_object(&host->frame_clock);
    host->after_paint_handler = 0;
  }
  if (host->window) {
    hide_host(host.get());
    SetParent(host->window, parking_window());
  }
  detach_from_toplevel(host.get());
  GTK_WIDGET_CLASS(pdfv_webview2_view_parent_class)->unrealize(widget);
}

static void pdfv_webview2_view_map(GtkWidget *widget) {
  GTK_WIDGET_CLASS(pdfv_webview2_view_parent_class)->map(widget);
  gtk_widget_queue_draw(widget);
}

static void pdfv_webview2_view_unmap(GtkWidget *widget) {
  auto host = host_of(PDFV_WEBVIEW2_VIEW(widget));
  if (host->applied_visible)
    hide_host(host.get());
  GTK_WIDGET_CLASS(pdfv_webview2_view_parent_class)->unmap(widget);
}

static void pdfv_webview2_view_size_allocate(GtkWidget *widget, int width,
                                             int height, int baseline) {
  GtkWidget *child = gtk_widget_get_first_child(widget);
  if (child) {
    gtk_widget_allocate(child, width, height, baseline, nullptr);
  }
  /* The geometry is applied once the frame is painted. */
  gtk_widget_queue_draw(widget);
}

static void pdfv_webview2_view_measure(GtkWidget *widget,
                                       GtkOrientation orientation, int for_size,
                                       int *minimum, int *natural,
                                       int *minimum_baseline,
                                       int *natural_baseline) {
  GtkWidget *child = gtk_widget_get_first_child(widget);
  *minimum = *natural = 0;
  if (child) {
    gtk_widget_measure(child, orientation, for_size, minimum, natural,
                       minimum_baseline, natural_baseline);
  }
}

static void pdfv_webview2_view_snapshot(GtkWidget *widget,
                                        GtkSnapshot *snapshot) {
  auto host = host_of(PDFV_WEBVIEW2_VIEW(widget));
  int width = gtk_widget_get_width(widget);
  int height = gtk_widget_get_height(widget);
  graphene_rect_t bounds = GRAPHENE_RECT_INIT(0, 0, static_cast<float>(width),
                                              static_cast<float>(height));
  if (host->snapshot) {
    gtk_snapshot_append_texture(snapshot, host->snapshot, &bounds);
  } else if (host->have_background && host->background.A) {
    GdkRGBA color = {host->background.R / 255.0f, host->background.G / 255.0f,
                     host->background.B / 255.0f, 1.0f};
    gtk_snapshot_append_color(snapshot, &color, &bounds);
  }
  GTK_WIDGET_CLASS(pdfv_webview2_view_parent_class)->snapshot(widget,
                                                               snapshot);
}

static void on_focus_changed(GtkWidget *widget, GParamSpec *,
                             gpointer) {
  auto host = host_of(PDFV_WEBVIEW2_VIEW(widget));
  if (!host)
    return;
  if (gtk_widget_has_focus(widget)) {
    move_focus_into(host);
  } else if (host->has_native_focus && host->toplevel) {
    /* GTK moved the focus elsewhere; take the keyboard back from the page. */
    SetFocus(host->toplevel);
  }
}

/* PdfvWebView -------------------------------------------------------------- */

static void pdfv_webview2_view_setup(PdfvWebView *view, PdfvWebView *related) {
  (void)related; /* every view shares one browser process */
  auto host = host_of(PDFV_WEBVIEW2_VIEW(view));
  /* Pages load before the view is shown: the Markdown editor stays hidden
   * until its page reports that it is ready. Until the widget is realized,
   * the host window waits in a hidden parking window. */
  register_host_class();
  host->window = CreateWindowExW(
      0, host_class_name, L"", WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
      0, 0, 0, 0, parking_window(), nullptr, GetModuleHandleW(nullptr),
      nullptr);
  if (!host->window) {
    show_failure(host, HRESULT_FROM_WIN32(GetLastError()));
    return;
  }
  create_controller(host);
}

static void pdfv_webview2_view_load_uri(PdfvWebView *view, const gchar *uri) {
  auto host = host_of(PDFV_WEBVIEW2_VIEW(view));
  std::wstring wide = to_wide(uri);
  auto weak = std::weak_ptr<Host>(host);
  host->when_ready([weak, wide]() {
    if (auto host = weak.lock())
      host->webview->Navigate(wide.c_str());
  });
}

static void pdfv_webview2_view_set_background_color(PdfvWebView *view,
                                                    const GdkRGBA *color) {
  auto host = host_of(PDFV_WEBVIEW2_VIEW(view));
  /* WebView2 supports only opaque or fully transparent backgrounds. */
  host->background = COREWEBVIEW2_COLOR{
      static_cast<BYTE>(color->alpha < 0.5f ? 0 : 255),
      static_cast<BYTE>(std::lround(CLAMP(color->red, 0.f, 1.f) * 255)),
      static_cast<BYTE>(std::lround(CLAMP(color->green, 0.f, 1.f) * 255)),
      static_cast<BYTE>(std::lround(CLAMP(color->blue, 0.f, 1.f) * 255))};
  host->have_background = true;
  ComPtr<ICoreWebView2Controller2> controller2;
  if (host->controller &&
      SUCCEEDED(query(host->controller, IID_ICoreWebView2Controller2,
                      controller2)))
    controller2->put_DefaultBackgroundColor(host->background);
  gtk_widget_queue_draw(GTK_WIDGET(view));
}

static void pdfv_webview2_view_set_developer_extras_enabled(
    PdfvWebView *view, gboolean enabled) {
  auto host = host_of(PDFV_WEBVIEW2_VIEW(view));
  host->developer_extras = enabled;
  ComPtr<ICoreWebView2Settings> settings;
  if (host->webview && SUCCEEDED(host->webview->get_Settings(&settings)))
    settings->put_AreDevToolsEnabled(enabled);
}

static void pdfv_webview2_view_run_javascript(PdfvWebView *view,
                                              const gchar *script,
                                              const gchar *source_uri,
                                              GTask *task) {
  (void)source_uri;
  auto host = host_of(PDFV_WEBVIEW2_VIEW(view));
  std::wstring wide = to_wide(script);
  auto weak = std::weak_ptr<Host>(host);
  auto drop = [task](GError *error) {
    g_task_return_error(task, error);
    g_object_unref(task);
  };
  host->when_ready([weak, wide, task]() {
    auto host = weak.lock();
    HRESULT result = host->webview->ExecuteScript(
        wide.c_str(),
        make_handler<ICoreWebView2ExecuteScriptCompletedHandler>(
            IID_ICoreWebView2ExecuteScriptCompletedHandler,
            [task](HRESULT error, LPCWSTR) -> HRESULT {
              if (FAILED(error))
                g_task_return_error(
                    task, error_from_hresult(error, "Running a script failed"));
              else
                g_task_return_boolean(task, TRUE);
              g_object_unref(task);
              return S_OK;
            })
            .Get());
    if (FAILED(result)) {
      g_task_return_error(
          task, error_from_hresult(result, "Running a script failed"));
      g_object_unref(task);
    }
  }, drop);
}

static void pdfv_webview2_view_print_to_pdf(PdfvWebView *view,
                                            const gchar *filename,
                                            GtkPageSetup *page_setup,
                                            GTask *task) {
  auto host = host_of(PDFV_WEBVIEW2_VIEW(view));
  std::wstring path = to_wide(filename);
  double width = gtk_page_setup_get_paper_width(page_setup, GTK_UNIT_INCH);
  double height = gtk_page_setup_get_paper_height(page_setup, GTK_UNIT_INCH);
  double top = gtk_page_setup_get_top_margin(page_setup, GTK_UNIT_INCH);
  double bottom = gtk_page_setup_get_bottom_margin(page_setup, GTK_UNIT_INCH);
  double left = gtk_page_setup_get_left_margin(page_setup, GTK_UNIT_INCH);
  double right = gtk_page_setup_get_right_margin(page_setup, GTK_UNIT_INCH);
  bool landscape = gtk_page_setup_get_orientation(page_setup) ==
                   GTK_PAGE_ORIENTATION_LANDSCAPE;
  auto weak = std::weak_ptr<Host>(host);
  host->when_ready([=]() {
    auto host = weak.lock();
    ComPtr<ICoreWebView2_7> webview7;
    ComPtr<ICoreWebView2Environment6> environment6;
    ComPtr<ICoreWebView2PrintSettings> settings;
    if (FAILED(query(host->webview, IID_ICoreWebView2_7, webview7)) ||
        FAILED(query(shared_environment().environment,
                     IID_ICoreWebView2Environment6, environment6)) ||
        FAILED(environment6->CreatePrintSettings(&settings))) {
      g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                              "This WebView2 Runtime cannot print to PDF");
      g_object_unref(task);
      return;
    }
    settings->put_Orientation(landscape
                                  ? COREWEBVIEW2_PRINT_ORIENTATION_LANDSCAPE
                                  : COREWEBVIEW2_PRINT_ORIENTATION_PORTRAIT);
    settings->put_PageWidth(width);
    settings->put_PageHeight(height);
    settings->put_MarginTop(top);
    settings->put_MarginBottom(bottom);
    settings->put_MarginLeft(left);
    settings->put_MarginRight(right);
    settings->put_ScaleFactor(1.0);
    settings->put_ShouldPrintBackgrounds(TRUE);
    settings->put_ShouldPrintHeaderAndFooter(FALSE);
    HRESULT result = webview7->PrintToPdf(
        path.c_str(), settings.Get(),
        make_handler<ICoreWebView2PrintToPdfCompletedHandler>(
            IID_ICoreWebView2PrintToPdfCompletedHandler,
            [task](HRESULT error, BOOL success) -> HRESULT {
              if (FAILED(error))
                g_task_return_error(
                    task, error_from_hresult(error, "Could not print"));
              else if (!success)
                g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                                        "Could not print the page to PDF");
              else
                g_task_return_boolean(task, TRUE);
              g_object_unref(task);
              return S_OK;
            })
            .Get());
    if (FAILED(result)) {
      g_task_return_error(task, error_from_hresult(result, "Could not print"));
      g_object_unref(task);
    }
  }, [task](GError *error) {
    g_task_return_error(task, error);
    g_object_unref(task);
  });
}

/* GObject ------------------------------------------------------------------ */

static void pdfv_webview2_view_dispose(GObject *object) {
  auto self = PDFV_WEBVIEW2_VIEW(object);
  if (self->host) {
    auto host = *self->host;
    host->view = nullptr;
    /* Pending operations own GTasks; finish them before dropping them. */
    host->drop_pending("The web view was destroyed");
    detach_from_toplevel(host.get());
    if (host->controller)
      host->controller->Close();
    host->controller.Reset();
    host->webview.Reset();
    if (host->window)
      DestroyWindow(host->window);
    host->window = nullptr;
  }
  GtkWidget *child;
  while ((child = gtk_widget_get_first_child(GTK_WIDGET(object))))
    gtk_widget_unparent(child);
  G_OBJECT_CLASS(pdfv_webview2_view_parent_class)->dispose(object);
}

static void pdfv_webview2_view_finalize(GObject *object) {
  auto self = PDFV_WEBVIEW2_VIEW(object);
  delete self->host;
  self->host = nullptr;
  G_OBJECT_CLASS(pdfv_webview2_view_parent_class)->finalize(object);
}

static void pdfv_webview2_view_class_init(PdfvWebView2ViewClass *klass) {
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);
  PdfvWebViewClass *view_class = PDFV_WEB_VIEW_CLASS(klass);
  object_class->dispose = pdfv_webview2_view_dispose;
  object_class->finalize = pdfv_webview2_view_finalize;
  widget_class->realize = pdfv_webview2_view_realize;
  widget_class->unrealize = pdfv_webview2_view_unrealize;
  widget_class->map = pdfv_webview2_view_map;
  widget_class->unmap = pdfv_webview2_view_unmap;
  widget_class->size_allocate = pdfv_webview2_view_size_allocate;
  widget_class->measure = pdfv_webview2_view_measure;
  widget_class->snapshot = pdfv_webview2_view_snapshot;
  view_class->setup = pdfv_webview2_view_setup;
  view_class->load_uri = pdfv_webview2_view_load_uri;
  view_class->set_background_color = pdfv_webview2_view_set_background_color;
  view_class->set_developer_extras_enabled =
      pdfv_webview2_view_set_developer_extras_enabled;
  view_class->run_javascript = pdfv_webview2_view_run_javascript;
  view_class->print_to_pdf = pdfv_webview2_view_print_to_pdf;
}

static void pdfv_webview2_view_init(PdfvWebView2View *self) {
  self->host = new std::shared_ptr<Host>(std::make_shared<Host>());
  (*self->host)->view = self;
  gtk_widget_set_focusable(GTK_WIDGET(self), TRUE);
  g_signal_connect(self, "notify::has-focus", G_CALLBACK(on_focus_changed),
                   nullptr);
}
