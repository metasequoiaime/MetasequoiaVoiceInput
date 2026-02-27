#include "window_webview2.h"

#include "mvi_utils.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <shellapi.h>
#include <WebView2.h>
#include <winuser.h>
#include <wrl.h>

namespace window_webview2
{
namespace
{
constexpr UINT WM_APP_TRAY_ICON = WM_APP + 120;
constexpr UINT WM_APP_TRAY_SHOW_MENU = WM_APP + 121;
constexpr UINT WM_APP_TRAY_RESIZE_TO_MENU = WM_APP + 122;
constexpr UINT k_tray_icon_id = 1;
constexpr wchar_t k_tray_window_class[] = L"MetasequoiaVoiceInput.TrayWindow";
constexpr wchar_t k_tray_menu_window_class[] = L"MetasequoiaVoiceInput.TrayMenuWindow";
constexpr wchar_t k_tray_tooltip[] = L"MetasequoiaVoiceInput";
constexpr wchar_t k_menu_size_prefix[] = L"__menu_size__:";

using CreateCoreWebView2EnvironmentWithOptionsFn = HRESULT(STDAPICALLTYPE *)(PCWSTR, PCWSTR, ICoreWebView2EnvironmentOptions *, ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler *);

struct TrayUiState
{
    TrayMenuConfig config{};

    HWND tray_window = nullptr;
    HWND tray_menu_window = nullptr;
    NOTIFYICONDATAW notify_icon{};
    bool notify_icon_added = false;
    HICON tray_icon = nullptr;

    HMODULE webview2_loader = nullptr;
    CreateCoreWebView2EnvironmentWithOptionsFn create_environment = nullptr;
    Microsoft::WRL::ComPtr<ICoreWebView2Environment> environment;
    Microsoft::WRL::ComPtr<ICoreWebView2Controller> controller;
    Microsoft::WRL::ComPtr<ICoreWebView2> webview;
    bool webview_creating = false;
    bool webview_ready = false;
    bool pending_show = false;
    POINT pending_show_point{};
    POINT last_anchor_pos{};

    std::wstring tray_menu_html_path;
    std::wstring tray_menu_html_content;
    std::wstring html_base_folder;
    int current_width = 194;
    int current_height = 299;
};

TrayUiState g_state;

std::wstring GetDefaultTrayMenuHtmlPath(const std::wstring &app_name)
{
    std::wstring html_path;
    char *buf = nullptr;
    size_t sz = 0;
    if (_dupenv_s(&buf, &sz, "LOCALAPPDATA") == 0 && buf != nullptr)
    {
        const std::string local_app_data(buf);
        free(buf);
        html_path = mvi_utils::utf8_to_wstring(local_app_data);
        html_path += L"\\";
        html_path += app_name;
        html_path += L"\\html\\tray_menu.html";
    }
    return html_path;
}

std::wstring ReadHtmlUtf8AsWide(const std::wstring &path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open())
    {
        return L"";
    }

    std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    return mvi_utils::utf8_to_wstring(content);
}

std::wstring GetFolderPath(const std::wstring &file_path)
{
    const size_t pos = file_path.find_last_of(L"\\/");
    if (pos == std::wstring::npos)
    {
        return L"";
    }
    return file_path.substr(0, pos);
}

std::wstring BuildHtmlWithBaseTag(const std::wstring &html)
{
    if (g_state.html_base_folder.empty())
    {
        return html;
    }

    std::wstring folder_url = L"https://traymenu/";
    std::wstring suffix = g_state.html_base_folder;
    std::replace(suffix.begin(), suffix.end(), L'\\', L'/');
    folder_url += suffix;
    folder_url += L"/";

    std::wstring base_tag = L"<base href=\"" + folder_url + L"\">";

    const size_t head_pos = html.find(L"<head>");
    if (head_pos != std::wstring::npos)
    {
        std::wstring result = html;
        result.insert(head_pos + 6, base_tag);
        return result;
    }

    const size_t html_pos = html.find(L"<html>");
    if (html_pos != std::wstring::npos)
    {
        std::wstring result = html;
        result.insert(html_pos + 6, L"<head>" + base_tag + L"</head>");
        return result;
    }

    return L"<head>" + base_tag + L"</head>" + html;
}

void ResizeWebViewBounds()
{
    if (g_state.controller == nullptr || g_state.tray_menu_window == nullptr)
    {
        return;
    }

    RECT client_rect{};
    if (GetClientRect(g_state.tray_menu_window, &client_rect))
    {
        g_state.controller->put_Bounds(client_rect);
    }
}

void HideTrayMenuWindow()
{
    if (g_state.controller != nullptr)
    {
        g_state.controller->put_IsVisible(FALSE);
    }
    if (g_state.tray_menu_window != nullptr)
    {
        ShowWindow(g_state.tray_menu_window, SW_HIDE);
    }
}

void ShowTrayMenuWindowAt(POINT anchor)
{
    if (g_state.tray_menu_window == nullptr)
    {
        return;
    }

    g_state.last_anchor_pos = anchor;

    RECT work_area{};
    HMONITOR monitor = MonitorFromPoint(anchor, MONITOR_DEFAULTTONEAREST);
    MONITORINFO monitor_info{};
    monitor_info.cbSize = sizeof(monitor_info);
    if (monitor != nullptr && GetMonitorInfoW(monitor, &monitor_info))
    {
        work_area = monitor_info.rcWork;
    }
    else
    {
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &work_area, 0);
    }

    int x = anchor.x;
    int y = anchor.y - g_state.current_height;
    if (x + g_state.current_width > work_area.right)
    {
        x = work_area.right - g_state.current_width;
    }
    if (x < work_area.left)
    {
        x = work_area.left;
    }
    if (y < work_area.top)
    {
        y = anchor.y;
    }
    if (y + g_state.current_height > work_area.bottom)
    {
        y = work_area.bottom - g_state.current_height;
    }

    SetWindowPos(g_state.tray_menu_window, HWND_TOPMOST, x, y, g_state.current_width, g_state.current_height, SWP_SHOWWINDOW);
    ShowWindow(g_state.tray_menu_window, SW_SHOWNOACTIVATE);
    SetForegroundWindow(g_state.tray_menu_window);

    if (g_state.controller != nullptr)
    {
        ResizeWebViewBounds();
        g_state.controller->put_IsVisible(TRUE);
    }
}

void RequestContainerSize()
{
    if (g_state.webview == nullptr)
    {
        return;
    }

    const wchar_t *script = LR"JS(
(() => {
  const send = () => {
    if (!chrome.webview) return;
    const menu = document.getElementById('menuContainer');
    if (!menu) return;
    const rect = menu.getBoundingClientRect();
    const w = Math.max(1, Math.ceil(rect.width));
    const h = Math.max(1, Math.ceil(rect.height));
    chrome.webview.postMessage(`__menu_size__:${w}:${h}`);
  };
  send();
  requestAnimationFrame(send);
  setTimeout(send, 60);
})();
)JS";
    g_state.webview->ExecuteScript(script, nullptr);
}

bool ParseBodySizeMessage(const std::wstring &message, int &width, int &height)
{
    const std::wstring prefix = k_menu_size_prefix;
    if (message.rfind(prefix, 0) != 0)
    {
        return false;
    }

    const size_t sep = message.find(L':', prefix.size());
    if (sep == std::wstring::npos)
    {
        return false;
    }

    const std::wstring width_text = message.substr(prefix.size(), sep - prefix.size());
    const std::wstring height_text = message.substr(sep + 1);

    try
    {
        width = std::stoi(width_text);
        height = std::stoi(height_text);
        return width > 0 && height > 0;
    }
    catch (...)
    {
        return false;
    }
}

LRESULT CALLBACK TrayMenuWindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message)
    {
    case WM_ACTIVATE:
        if (LOWORD(wParam) == WA_INACTIVE)
        {
            HideTrayMenuWindow();
        }
        return 0;
    case WM_KILLFOCUS:
        HideTrayMenuWindow();
        return 0;
    case WM_SIZE:
        ResizeWebViewBounds();
        return 0;
    case WM_CLOSE:
        HideTrayMenuWindow();
        return 0;
    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE)
        {
            HideTrayMenuWindow();
            return 0;
        }
        break;
    case WM_APP_TRAY_RESIZE_TO_MENU: {
        static float scale = mvi_utils::GetForegroundWindowScale();
        const int width = static_cast<int>(static_cast<short>(LOWORD(lParam))) * scale + 10;
        const int height = static_cast<int>(static_cast<short>(HIWORD(lParam))) * scale;
        if (width > 0 && height > 0)
        {
            g_state.current_width = width;
            g_state.current_height = height;
            if (IsWindowVisible(hwnd))
            {
                ShowTrayMenuWindowAt(g_state.last_anchor_pos);
            }
        }
        return 0;
    }
    default:
        break;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

bool CreateTrayMenuWindow(HINSTANCE instance)
{
    if (g_state.tray_menu_window != nullptr)
    {
        return true;
    }

    WNDCLASSW wc{};
    wc.lpfnWndProc = TrayMenuWindowProc;
    wc.hInstance = instance;
    wc.lpszClassName = k_tray_menu_window_class;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    RegisterClassW(&wc);

    g_state.tray_menu_window = CreateWindowExW(           //
        WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_TOPMOST, //
        k_tray_menu_window_class,                         //
        L"MetasequoiaVoiceInput.TrayMenu",                //
        WS_POPUP,                                         //
        CW_USEDEFAULT,                                    //
        CW_USEDEFAULT,                                    //
        g_state.current_width,                            //
        g_state.current_height,                           //
        nullptr,                                          //
        nullptr,                                          //
        instance,                                         //
        nullptr);

    if (g_state.tray_menu_window == nullptr)
    {
        printf("[TRAY] Failed to create tray menu window: %lu\n", GetLastError());
        fflush(stdout);
        return false;
    }
    return true;
}

bool EnsureWebView2Loader()
{
    if (g_state.create_environment != nullptr)
    {
        return true;
    }
    if (g_state.webview2_loader == nullptr)
    {
        g_state.webview2_loader = LoadLibraryW(L"WebView2Loader.dll");
    }
    if (g_state.webview2_loader == nullptr)
    {
        printf("[TRAY] WebView2Loader.dll not found.\n");
        fflush(stdout);
        return false;
    }

    auto proc = GetProcAddress(g_state.webview2_loader, "CreateCoreWebView2EnvironmentWithOptions");
    if (proc == nullptr)
    {
        printf("[TRAY] CreateCoreWebView2EnvironmentWithOptions not found in WebView2Loader.dll.\n");
        fflush(stdout);
        return false;
    }

    g_state.create_environment = reinterpret_cast<CreateCoreWebView2EnvironmentWithOptionsFn>(proc);
    return true;
}

void CreateTrayMenuWebViewIfNeeded()
{
    if (g_state.webview_ready || g_state.webview_creating || g_state.tray_menu_window == nullptr)
    {
        return;
    }
    if (!EnsureWebView2Loader())
    {
        return;
    }

    g_state.webview_creating = true;
    const HRESULT hr = g_state.create_environment(                                            //
        nullptr,                                                                              //
        nullptr,                                                                              //
        nullptr,                                                                              //
        Microsoft::WRL::Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>( //
            [](HRESULT result, ICoreWebView2Environment *environment) -> HRESULT {
                g_state.webview_creating = false;
                if (FAILED(result) || environment == nullptr)
                {
                    printf("[TRAY] Failed to create WebView2 environment: 0x%08lx\n", static_cast<unsigned long>(result));
                    fflush(stdout);
                    return S_OK;
                }

                g_state.environment = environment;
                return g_state.environment->CreateCoreWebView2Controller( //
                    g_state.tray_menu_window,                             //
                    Microsoft::WRL::Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>([](HRESULT controller_result, ICoreWebView2Controller *controller) -> HRESULT {
                        if (FAILED(controller_result) || controller == nullptr)
                        {
                            printf("[TRAY] Failed to create WebView2 controller: 0x%08lx\n", static_cast<unsigned long>(controller_result));
                            fflush(stdout);
                            return S_OK;
                        }

                        g_state.controller = controller;
                        g_state.controller->get_CoreWebView2(&g_state.webview);
                        if (g_state.webview == nullptr)
                        {
                            printf("[TRAY] Failed to get CoreWebView2 instance.\n");
                            fflush(stdout);
                            return S_OK;
                        }

                        Microsoft::WRL::ComPtr<ICoreWebView2Controller2> webviewController2MenuWnd;
                        // Set transparent background
                        if (SUCCEEDED(controller->QueryInterface(IID_PPV_ARGS(&webviewController2MenuWnd))))
                        {
                            COREWEBVIEW2_COLOR backgroundColor = {0, 0, 0, 0};
                            webviewController2MenuWnd->put_DefaultBackgroundColor(backgroundColor);
                        }

                        Microsoft::WRL::ComPtr<ICoreWebView2Settings> settings;
                        if (SUCCEEDED(g_state.webview->get_Settings(&settings)) && settings != nullptr)
                        {
                            settings->put_IsScriptEnabled(TRUE);
                            settings->put_IsWebMessageEnabled(TRUE);
                            settings->put_AreDefaultContextMenusEnabled(FALSE);
                            settings->put_AreDevToolsEnabled(FALSE);
                            settings->put_IsStatusBarEnabled(FALSE);
                        }

                        Microsoft::WRL::ComPtr<ICoreWebView2_3> webview3;
                        if (SUCCEEDED(g_state.webview->QueryInterface(IID_PPV_ARGS(&webview3))) && !g_state.html_base_folder.empty())
                        {
                            webview3->SetVirtualHostNameToFolderMapping(L"traymenu", g_state.html_base_folder.c_str(), COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_DENY_CORS);
                        }

                        g_state.webview->add_WebMessageReceived(                                   //
                            Microsoft::WRL::Callback<ICoreWebView2WebMessageReceivedEventHandler>( //
                                [](ICoreWebView2 *, ICoreWebView2WebMessageReceivedEventArgs *args) -> HRESULT {
                                    LPWSTR raw_message = nullptr;
                                    if (args == nullptr || FAILED(args->TryGetWebMessageAsString(&raw_message)) || raw_message == nullptr)
                                    {
                                        return S_OK;
                                    }

                                    const std::wstring message(raw_message);
                                    CoTaskMemFree(raw_message);

                                    int width = 0;
                                    int height = 0;
                                    if (ParseBodySizeMessage(message, width, height))
                                    {
                                        const LPARAM size_lparam = MAKELPARAM(width, height);
                                        PostMessageW(g_state.tray_menu_window, WM_APP_TRAY_RESIZE_TO_MENU, 0, size_lparam);
                                        return S_OK;
                                    }

                                    if (message == L"hide")
                                    {
                                        HideTrayMenuWindow();
                                    }
                                    else if (message == L"exit")
                                    {
                                        PostThreadMessage(g_state.config.main_thread_id, g_state.config.msg_exit, 0, 0);
                                    }
                                    else if (message == L"toggle_record")
                                    {
                                        PostThreadMessage(g_state.config.main_thread_id, g_state.config.msg_toggle_record, 0, 0);
                                    }
                                    return S_OK;
                                })
                                .Get(),
                            nullptr);

                        g_state.webview->add_NavigationCompleted( //
                            Microsoft::WRL::Callback<ICoreWebView2NavigationCompletedEventHandler>([](ICoreWebView2 *, ICoreWebView2NavigationCompletedEventArgs *args) -> HRESULT {
                                BOOL success = FALSE;
                                if (args != nullptr)
                                {
                                    args->get_IsSuccess(&success);
                                }
                                if (success)
                                {
                                    RequestContainerSize();
                                }
                                return S_OK;
                            }).Get(),
                            nullptr);

                        g_state.controller->put_IsVisible(FALSE);
                        ResizeWebViewBounds();

                        const std::wstring html = BuildHtmlWithBaseTag(g_state.tray_menu_html_content);
                        g_state.webview->NavigateToString(html.c_str());
                        g_state.webview_ready = true;

                        if (g_state.pending_show)
                        {
                            g_state.pending_show = false;
                            ShowTrayMenuWindowAt(g_state.pending_show_point);
                        }
                        return S_OK;
                    }).Get());
            })
            .Get());

    if (FAILED(hr))
    {
        g_state.webview_creating = false;
        printf("[TRAY] Failed to start WebView2 creation: 0x%08lx\n", static_cast<unsigned long>(hr));
        fflush(stdout);
    }
}

void RequestShowTrayMenu()
{
    POINT cursor{};
    GetCursorPos(&cursor);

    if (g_state.webview_ready)
    {
        ShowTrayMenuWindowAt(cursor);
        return;
    }
    g_state.pending_show = true;
    g_state.pending_show_point = cursor;
    CreateTrayMenuWebViewIfNeeded();
}

LRESULT CALLBACK TrayWindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message)
    {
    case WM_APP_TRAY_ICON:
        if (LOWORD(lParam) == WM_RBUTTONUP || LOWORD(lParam) == WM_CONTEXTMENU)
        {
            PostMessageW(hwnd, WM_APP_TRAY_SHOW_MENU, 0, 0);
            return 0;
        }
        if (LOWORD(lParam) == WM_LBUTTONDBLCLK && g_state.config.msg_toggle_record != 0)
        {
            PostThreadMessage(g_state.config.main_thread_id, g_state.config.msg_toggle_record, 0, 0);
            return 0;
        }
        break;
    case WM_APP_TRAY_SHOW_MENU:
        RequestShowTrayMenu();
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}
} // namespace

bool InitializeTrayUi(HINSTANCE instance, const TrayMenuConfig &config)
{
    g_state = {};
    g_state.config = config;
    float scale = mvi_utils::GetForegroundWindowScale();
    g_state.current_width = config.default_width * scale;
    g_state.current_height = config.default_height * scale;

    g_state.tray_menu_html_path = config.tray_menu_html_path.empty() ? GetDefaultTrayMenuHtmlPath(config.app_name) : config.tray_menu_html_path;
    if (g_state.tray_menu_html_path.empty())
    {
        printf("[TRAY] tray_menu.html path is empty.\n");
        fflush(stdout);
        return false;
    }

    const DWORD attrs = GetFileAttributesW(g_state.tray_menu_html_path.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0)
    {
        printf("[TRAY] tray_menu.html not found: %ls\n", g_state.tray_menu_html_path.c_str());
        fflush(stdout);
        return false;
    }

    g_state.tray_menu_html_content = ReadHtmlUtf8AsWide(g_state.tray_menu_html_path);
    if (g_state.tray_menu_html_content.empty())
    {
        printf("[TRAY] Failed to read tray_menu.html content: %ls\n", g_state.tray_menu_html_path.c_str());
        fflush(stdout);
        return false;
    }
    g_state.html_base_folder = GetFolderPath(g_state.tray_menu_html_path);

    WNDCLASSW wc{};
    wc.lpfnWndProc = TrayWindowProc;
    wc.hInstance = instance;
    wc.lpszClassName = k_tray_window_class;
    RegisterClassW(&wc);

    g_state.tray_window = CreateWindowExW(0, k_tray_window_class, L"MetasequoiaVoiceInput.TrayHost", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, instance, nullptr);
    if (g_state.tray_window == nullptr)
    {
        printf("[TRAY] Failed to create tray host window: %lu\n", GetLastError());
        fflush(stdout);
        return false;
    }
    if (!CreateTrayMenuWindow(instance))
    {
        return false;
    }

    g_state.tray_icon = static_cast<HICON>(LoadImageW(nullptr, IDI_APPLICATION, IMAGE_ICON, 0, 0, LR_SHARED));
    if (g_state.tray_icon == nullptr)
    {
        g_state.tray_icon = LoadIconW(nullptr, IDI_APPLICATION);
    }

    g_state.notify_icon = {};
    g_state.notify_icon.cbSize = sizeof(NOTIFYICONDATAW);
    g_state.notify_icon.hWnd = g_state.tray_window;
    g_state.notify_icon.uID = k_tray_icon_id;
    g_state.notify_icon.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    g_state.notify_icon.uCallbackMessage = WM_APP_TRAY_ICON;
    g_state.notify_icon.hIcon = g_state.tray_icon;
    wcscpy_s(g_state.notify_icon.szTip, k_tray_tooltip);

    if (!Shell_NotifyIconW(NIM_ADD, &g_state.notify_icon))
    {
        printf("[TRAY] Failed to add tray icon.\n");
        fflush(stdout);
        return false;
    }

    g_state.notify_icon_added = true;
    g_state.notify_icon.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &g_state.notify_icon);
    printf("[TRAY] Tray icon initialized.\n");
    fflush(stdout);
    return true;
}

void ShutdownTrayUi(HINSTANCE instance)
{
    HideTrayMenuWindow();

    if (g_state.notify_icon_added)
    {
        Shell_NotifyIconW(NIM_DELETE, &g_state.notify_icon);
        g_state.notify_icon_added = false;
    }

    g_state.webview = nullptr;
    g_state.controller = nullptr;
    g_state.environment = nullptr;

    if (g_state.tray_menu_window != nullptr)
    {
        DestroyWindow(g_state.tray_menu_window);
        g_state.tray_menu_window = nullptr;
    }
    if (g_state.tray_window != nullptr)
    {
        DestroyWindow(g_state.tray_window);
        g_state.tray_window = nullptr;
    }
    if (g_state.webview2_loader != nullptr)
    {
        FreeLibrary(g_state.webview2_loader);
        g_state.webview2_loader = nullptr;
        g_state.create_environment = nullptr;
    }

    UnregisterClassW(k_tray_menu_window_class, instance);
    UnregisterClassW(k_tray_window_class, instance);
}
} // namespace window_webview2
