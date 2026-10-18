#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"
#include "imgui_freetype.h"
#include "panorama_ui.h"

#include <d3d11.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <vector>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "windowscodecs.lib")

using Microsoft::WRL::ComPtr;

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace
{
UINT resize_width = 0;
UINT resize_height = 0;

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    if (ImGui::GetCurrentContext() && ImGui_ImplWin32_WndProcHandler(window, message, wparam, lparam))
        return 1;
    switch (message)
    {
    case WM_SIZE:
        if (wparam != SIZE_MINIMIZED)
        {
            resize_width = LOWORD(lparam);
            resize_height = HIWORD(lparam);
        }
        return 0;
    case WM_SYSCOMMAND:
        if ((wparam & 0xfff0) == SC_KEYMENU)
            return 0;
        break;
    case WM_ERASEBKGND:
        return 1;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

struct Renderer
{
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<IDXGISwapChain> swap_chain;
    ComPtr<ID3D11RenderTargetView> target;
    ComPtr<ID3D11ShaderResourceView> wallpaper_srv;
    UINT wallpaper_width = 0;
    UINT wallpaper_height = 0;

    bool LoadImageWIC(const wchar_t* path)
    {
        if (!path || !path[0])
            return false;
        ComPtr<IWICImagingFactory> factory;
        if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))))
            return false;
        ComPtr<IWICBitmapDecoder> decoder;
        if (FAILED(factory->CreateDecoderFromFilename(path, nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder)))
            return false;
        ComPtr<IWICBitmapFrameDecode> frame;
        if (FAILED(decoder->GetFrame(0, &frame)))
            return false;
        ComPtr<IWICFormatConverter> converter;
        if (FAILED(factory->CreateFormatConverter(&converter)))
            return false;
        if (FAILED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA,
            WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom)))
            return false;

        UINT w = 0, h = 0;
        if (FAILED(converter->GetSize(&w, &h)) || w == 0 || h == 0)
            return false;

        std::vector<unsigned char> pixels(static_cast<size_t>(w) * h * 4);
        if (FAILED(converter->CopyPixels(nullptr, w * 4, static_cast<UINT>(pixels.size()), pixels.data())))
            return false;

        D3D11_TEXTURE2D_DESC tex_desc = {};
        tex_desc.Width = w;
        tex_desc.Height = h;
        tex_desc.MipLevels = 1;
        tex_desc.ArraySize = 1;
        tex_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        tex_desc.SampleDesc.Count = 1;
        tex_desc.Usage = D3D11_USAGE_IMMUTABLE;
        tex_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

        D3D11_SUBRESOURCE_DATA init_data = {};
        init_data.pSysMem = pixels.data();
        init_data.SysMemPitch = w * 4;

        ComPtr<ID3D11Texture2D> tex;
        if (FAILED(device->CreateTexture2D(&tex_desc, &init_data, &tex)))
            return false;
        if (FAILED(device->CreateShaderResourceView(tex.Get(), nullptr, &wallpaper_srv)))
            return false;

        wallpaper_width = w;
        wallpaper_height = h;
        return true;
    }

    void LoadWindowsWallpaper()
    {
        static constexpr const wchar_t* kWallpapers[] = {
            L"C:\\Windows\\Web\\Wallpaper\\Theme1\\img1.jpg",
            L"C:\\Windows\\Web\\Wallpaper\\Theme1\\img13.jpg",
            L"C:\\Windows\\Web\\Wallpaper\\Spotlight\\img50.jpg"
        };
        for (const wchar_t* path : kWallpapers)
        {
            if (LoadImageWIC(path))
                return;
        }
        wchar_t desktop_wp[MAX_PATH] = {};
        if (SystemParametersInfoW(SPI_GETDESKWALLPAPER, MAX_PATH, desktop_wp, 0))
            LoadImageWIC(desktop_wp);
    }

    bool Create(HWND window)
    {
        DXGI_SWAP_CHAIN_DESC desc = {};
        desc.BufferCount = 2;
        desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.OutputWindow = window;
        desc.SampleDesc.Count = 1;
        desc.Windowed = TRUE;
        desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
        const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
        D3D_FEATURE_LEVEL chosen = {};
        HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
            levels, 2, D3D11_SDK_VERSION, &desc, &swap_chain, &device, &chosen, &context);
        if (FAILED(hr))
            hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0,
                levels, 2, D3D11_SDK_VERSION, &desc, &swap_chain, &device, &chosen, &context);
        if (FAILED(hr) || !CreateTarget())
            return false;
        LoadWindowsWallpaper();
        return true;
    }

    bool CreateTarget()
    {
        ComPtr<ID3D11Texture2D> buffer;
        return SUCCEEDED(swap_chain->GetBuffer(0, IID_PPV_ARGS(&buffer))) &&
            SUCCEEDED(device->CreateRenderTargetView(buffer.Get(), nullptr, &target));
    }

    bool Resize(UINT width, UINT height)
    {
        if (width == 0 || height == 0)
            return true;
        context->OMSetRenderTargets(0, nullptr, nullptr);
        target.Reset();
        return SUCCEEDED(swap_chain->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0)) && CreateTarget();
    }

    void Render()
    {
        const float clear[4] = { 0.12f, 0.15f, 0.20f, 1.0f };
        ID3D11RenderTargetView* raw_target = target.Get();
        context->OMSetRenderTargets(1, &raw_target, nullptr);
        context->ClearRenderTargetView(raw_target, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    }
};

}

int main()
{
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)))
        return 1;
    ImGui_ImplWin32_EnableDpiAwareness();
    const float dpi = ImGui_ImplWin32_GetDpiScaleForMonitor(MonitorFromPoint(POINT{ 0, 0 }, MONITOR_DEFAULTTOPRIMARY));
    WNDCLASSEXW window_class = { sizeof(WNDCLASSEXW), CS_CLASSDC, WindowProc, 0, 0, GetModuleHandleW(nullptr), nullptr, nullptr, nullptr, nullptr, L"Panorama ImGui Preview", nullptr };
    RegisterClassExW(&window_class);
    HWND window = CreateWindowW(window_class.lpszClassName, L"Panorama UI by kdt255", WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, static_cast<int>(1320 * dpi), static_cast<int>(820 * dpi),
        nullptr, nullptr, window_class.hInstance, nullptr);
    Renderer renderer;
    if (!window || !renderer.Create(window))
    {
        if (window)
            DestroyWindow(window);
        UnregisterClassW(window_class.lpszClassName, window_class.hInstance);
        CoUninitialize();
        return 1;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = "panorama_ui.ini";
    io.Fonts->SetFontLoader(ImGuiFreeType::GetFontLoader());
    panorama::Fonts fonts;
    if (!panorama::LoadEmbeddedFonts(*io.Fonts, fonts))
    {
        ImGui::DestroyContext();
        DestroyWindow(window);
        UnregisterClassW(window_class.lpszClassName, window_class.hInstance);
        CoUninitialize();
        return 1;
    }
    io.FontDefault = fonts.noto_regular;
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 0.0f;
    style.FrameRounding = 2.0f;
    style.ScaleAllSizes(dpi);
    style.FontScaleDpi = dpi;
    ImGui_ImplWin32_Init(window);
    ImGui_ImplDX11_Init(renderer.device.Get(), renderer.context.Get());

    panorama::State state;
    ShowWindow(window, SW_SHOWDEFAULT);
    UpdateWindow(window);

    bool running = true;
    while (running)
    {
        MSG message;
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&message);
            DispatchMessageW(&message);
            if (message.message == WM_QUIT)
                running = false;
        }
        if (!running)
            break;
        if (resize_width && resize_height)
        {
            if (!renderer.Resize(resize_width, resize_height))
                break;
            resize_width = resize_height = 0;
        }
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        if (renderer.wallpaper_srv && io.DisplaySize.x > 0.0f && io.DisplaySize.y > 0.0f)
        {
            const float win_aspect = io.DisplaySize.x / io.DisplaySize.y;
            const float img_aspect = static_cast<float>(renderer.wallpaper_width) /
                static_cast<float>(renderer.wallpaper_height);
            ImVec2 uv0(0.0f, 0.0f);
            ImVec2 uv1(1.0f, 1.0f);
            if (win_aspect > img_aspect)
            {
                const float v_span = img_aspect / win_aspect;
                uv0.y = (1.0f - v_span) * 0.5f;
                uv1.y = uv0.y + v_span;
            }
            else
            {
                const float u_span = win_aspect / img_aspect;
                uv0.x = (1.0f - u_span) * 0.5f;
                uv1.x = uv0.x + u_span;
            }
            ImGui::GetBackgroundDrawList()->AddImage(
                reinterpret_cast<ImTextureID>(renderer.wallpaper_srv.Get()),
                { 0.0f, 0.0f }, io.DisplaySize, uv0, uv1);
        }
        panorama::Render(state, fonts);
        if (!state.visible)
            break;
        ImGui::Render();
        renderer.Render();
        const HRESULT result = renderer.swap_chain->Present(1, 0);
        if (result == DXGI_STATUS_OCCLUDED)
            Sleep(10);
        else if (FAILED(result))
            break;
    }

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    renderer.wallpaper_srv.Reset();
    renderer.target.Reset();
    renderer.swap_chain.Reset();
    renderer.context.Reset();
    renderer.device.Reset();
    DestroyWindow(window);
    UnregisterClassW(window_class.lpszClassName, window_class.hInstance);
    CoUninitialize();
    return 0;
}
