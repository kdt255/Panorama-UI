#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include "panorama_ui.h"
#include "imgui_impl_dx11.h"
#include "imgui_internal.h"

#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#ifdef small
#undef small
#endif

#include <algorithm>
#include <array>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>

#pragma comment(lib, "d3dcompiler.lib")

namespace panorama
{
namespace
{
using Microsoft::WRL::ComPtr;

constexpr ImU32 kWhite = IM_COL32(244, 246, 248, 255);
constexpr ImU32 kMuted = IM_COL32(215, 216, 217, 255);
constexpr ImU32 kDivider = IM_COL32(238, 239, 240, 48);

struct BlurResources
{
    ID3D11Device* device = nullptr;
    UINT width = 0;
    UINT height = 0;
    UINT blur_width = 0;
    UINT blur_height = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;

    ComPtr<ID3D11VertexShader> fullscreen_vs;
    ComPtr<ID3D11PixelShader> copy_ps;
    ComPtr<ID3D11PixelShader> blur_ps;
    ComPtr<ID3D11Buffer> blur_cb;
    ComPtr<ID3D11SamplerState> clamp_sampler;
    ComPtr<ID3D11BlendState> opaque_blend;
    ComPtr<ID3D11DepthStencilState> disabled_dss;
    ComPtr<ID3D11RasterizerState> no_scissor_rs;
    ComPtr<ID3D11RasterizerState> scissor_rs;

    ComPtr<ID3D11Texture2D> copy_tex;
    ComPtr<ID3D11ShaderResourceView> copy_srv;
    ComPtr<ID3D11Texture2D> blur_tex_a;
    ComPtr<ID3D11RenderTargetView> blur_rtv_a;
    ComPtr<ID3D11ShaderResourceView> blur_srv_a;
    ComPtr<ID3D11Texture2D> blur_tex_b;
    ComPtr<ID3D11RenderTargetView> blur_rtv_b;
    ComPtr<ID3D11ShaderResourceView> blur_srv_b;

    bool EnsurePipeline(ID3D11Device* dev)
    {
        if (device == dev && fullscreen_vs && copy_ps && blur_ps && blur_cb &&
            clamp_sampler && opaque_blend && disabled_dss && no_scissor_rs && scissor_rs)
            return true;

        *this = BlurResources{};
        device = dev;

        static constexpr const char kVS[] = R"(
            struct VSOut {
                float4 pos : SV_POSITION;
                float2 uv  : TEXCOORD0;
            };
            VSOut main(uint id : SV_VertexID) {
                VSOut o;
                o.uv = float2((id << 1) & 2, id & 2);
                o.pos = float4(o.uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
                return o;
            }
        )";

        static constexpr const char kCopyPS[] = R"(
            Texture2D tex : register(t0);
            SamplerState samp : register(s0);
            float4 main(float4 pos : SV_POSITION, float2 uv : TEXCOORD0) : SV_Target {
                return float4(tex.Sample(samp, uv).rgb, 1.0);
            }
        )";

        static constexpr const char kBlurPS[] = R"(
            cbuffer BlurCB : register(b0) {
                float2 step_uv;
                float2 pad;
            };
            Texture2D tex : register(t0);
            SamplerState samp : register(s0);
            float4 main(float4 pos : SV_POSITION, float2 uv : TEXCOORD0) : SV_Target {
                float3 col = tex.Sample(samp, uv).rgb * 0.19648255;
                col += tex.Sample(samp, uv + step_uv * 1.4117647).rgb * 0.29690696;
                col += tex.Sample(samp, uv - step_uv * 1.4117647).rgb * 0.29690696;
                col += tex.Sample(samp, uv + step_uv * 3.2941176).rgb * 0.09447039;
                col += tex.Sample(samp, uv - step_uv * 3.2941176).rgb * 0.09447039;
                col += tex.Sample(samp, uv + step_uv * 5.1764706).rgb * 0.01038136;
                col += tex.Sample(samp, uv - step_uv * 5.1764706).rgb * 0.01038136;
                return float4(col, 1.0);
            }
        )";

        ComPtr<ID3DBlob> blob;
        if (FAILED(D3DCompile(kVS, sizeof(kVS) - 1, nullptr, nullptr, nullptr, "main", "vs_4_0", 0, 0, &blob, nullptr)))
            return false;
        if (FAILED(dev->CreateVertexShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &fullscreen_vs)))
            return false;

        blob.Reset();
        if (FAILED(D3DCompile(kCopyPS, sizeof(kCopyPS) - 1, nullptr, nullptr, nullptr, "main", "ps_4_0", 0, 0, &blob, nullptr)))
            return false;
        if (FAILED(dev->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &copy_ps)))
            return false;

        blob.Reset();
        if (FAILED(D3DCompile(kBlurPS, sizeof(kBlurPS) - 1, nullptr, nullptr, nullptr, "main", "ps_4_0", 0, 0, &blob, nullptr)))
            return false;
        if (FAILED(dev->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &blur_ps)))
            return false;

        D3D11_BUFFER_DESC cb_desc = {};
        cb_desc.ByteWidth = 16;
        cb_desc.Usage = D3D11_USAGE_DYNAMIC;
        cb_desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        cb_desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(dev->CreateBuffer(&cb_desc, nullptr, &blur_cb)))
            return false;

        D3D11_SAMPLER_DESC samp_desc = {};
        samp_desc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        samp_desc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
        samp_desc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
        samp_desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        samp_desc.ComparisonFunc = D3D11_COMPARISON_ALWAYS;
        samp_desc.MaxLOD = D3D11_FLOAT32_MAX;
        if (FAILED(dev->CreateSamplerState(&samp_desc, &clamp_sampler)))
            return false;

        D3D11_BLEND_DESC blend_desc = {};
        blend_desc.RenderTarget[0].BlendEnable = FALSE;
        blend_desc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        if (FAILED(dev->CreateBlendState(&blend_desc, &opaque_blend)))
            return false;

        D3D11_DEPTH_STENCIL_DESC dss_desc = {};
        dss_desc.DepthEnable = FALSE;
        dss_desc.StencilEnable = FALSE;
        if (FAILED(dev->CreateDepthStencilState(&dss_desc, &disabled_dss)))
            return false;

        D3D11_RASTERIZER_DESC rs_desc = {};
        rs_desc.FillMode = D3D11_FILL_SOLID;
        rs_desc.CullMode = D3D11_CULL_NONE;
        rs_desc.ScissorEnable = FALSE;
        rs_desc.DepthClipEnable = TRUE;
        if (FAILED(dev->CreateRasterizerState(&rs_desc, &no_scissor_rs)))
            return false;

        rs_desc.ScissorEnable = TRUE;
        if (FAILED(dev->CreateRasterizerState(&rs_desc, &scissor_rs)))
            return false;

        return true;
    }

    bool EnsureTextures(ID3D11Device* dev, UINT w, UINT h, DXGI_FORMAT fmt)
    {
        if (w == 0 || h == 0)
            return false;
        if (width == w && height == h && format == fmt && copy_srv && blur_srv_a && blur_srv_b)
            return true;

        copy_tex.Reset();
        copy_srv.Reset();
        blur_tex_a.Reset();
        blur_rtv_a.Reset();
        blur_srv_a.Reset();
        blur_tex_b.Reset();
        blur_rtv_b.Reset();
        blur_srv_b.Reset();

        width = w;
        height = h;
        blur_width = std::max(1u, w / 2);
        blur_height = std::max(1u, h / 2);
        format = fmt;

        D3D11_TEXTURE2D_DESC copy_desc = {};
        copy_desc.Width = w;
        copy_desc.Height = h;
        copy_desc.MipLevels = 1;
        copy_desc.ArraySize = 1;
        copy_desc.Format = fmt;
        copy_desc.SampleDesc.Count = 1;
        copy_desc.Usage = D3D11_USAGE_DEFAULT;
        copy_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(dev->CreateTexture2D(&copy_desc, nullptr, &copy_tex)) ||
            FAILED(dev->CreateShaderResourceView(copy_tex.Get(), nullptr, &copy_srv)))
            return false;

        D3D11_TEXTURE2D_DESC blur_desc = copy_desc;
        blur_desc.Width = blur_width;
        blur_desc.Height = blur_height;
        blur_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        blur_desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(dev->CreateTexture2D(&blur_desc, nullptr, &blur_tex_a)) ||
            FAILED(dev->CreateRenderTargetView(blur_tex_a.Get(), nullptr, &blur_rtv_a)) ||
            FAILED(dev->CreateShaderResourceView(blur_tex_a.Get(), nullptr, &blur_srv_a)))
            return false;

        if (FAILED(dev->CreateTexture2D(&blur_desc, nullptr, &blur_tex_b)) ||
            FAILED(dev->CreateRenderTargetView(blur_tex_b.Get(), nullptr, &blur_rtv_b)) ||
            FAILED(dev->CreateShaderResourceView(blur_tex_b.Get(), nullptr, &blur_srv_b)))
            return false;

        return true;
    }
};

BlurResources g_blur;

void DrawBlurCallback(const ImDrawList*, const ImDrawCmd* cmd)
{
    ImGui_ImplDX11_RenderState* rs = ImGui_ImplDX11_GetRenderState();
    if (!rs || !rs->Device || !rs->DeviceContext)
        return;

    ID3D11Device* dev = rs->Device;
    ID3D11DeviceContext* ctx = rs->DeviceContext;

    ComPtr<ID3D11RenderTargetView> main_rtv;
    ComPtr<ID3D11DepthStencilView> main_dsv;
    ctx->OMGetRenderTargets(1, &main_rtv, &main_dsv);
    if (!main_rtv)
        return;

    ComPtr<ID3D11Resource> bb_res;
    main_rtv->GetResource(&bb_res);
    ComPtr<ID3D11Texture2D> bb_tex;
    if (!bb_res || FAILED(bb_res.As(&bb_tex)))
        return;

    D3D11_TEXTURE2D_DESC bb_desc = {};
    bb_tex->GetDesc(&bb_desc);
    if (!g_blur.EnsurePipeline(dev) || !g_blur.EnsureTextures(dev, bb_desc.Width, bb_desc.Height, bb_desc.Format))
        return;

    if (bb_desc.SampleDesc.Count > 1)
        ctx->ResolveSubresource(g_blur.copy_tex.Get(), 0, bb_tex.Get(), 0, bb_desc.Format);
    else
        ctx->CopyResource(g_blur.copy_tex.Get(), bb_tex.Get());

    ID3D11ShaderResourceView* null_srv = nullptr;
    const float blend_factor[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    ctx->IASetInputLayout(nullptr);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(g_blur.fullscreen_vs.Get(), nullptr, 0);
    ID3D11SamplerState* sampler = g_blur.clamp_sampler.Get();
    ctx->PSSetSamplers(0, 1, &sampler);
    ctx->OMSetBlendState(g_blur.opaque_blend.Get(), blend_factor, 0xffffffff);
    ctx->OMSetDepthStencilState(g_blur.disabled_dss.Get(), 0);
    ctx->RSSetState(g_blur.no_scissor_rs.Get());

    D3D11_VIEWPORT blur_vp = {};
    blur_vp.Width = static_cast<float>(g_blur.blur_width);
    blur_vp.Height = static_cast<float>(g_blur.blur_height);
    blur_vp.MaxDepth = 1.0f;
    ctx->RSSetViewports(1, &blur_vp);

    // Downsample copy into blur_rtv_a
    ID3D11RenderTargetView* rtv_a = g_blur.blur_rtv_a.Get();
    ID3D11RenderTargetView* rtv_b = g_blur.blur_rtv_b.Get();
    ID3D11ShaderResourceView* srv_copy = g_blur.copy_srv.Get();
    ID3D11ShaderResourceView* srv_a = g_blur.blur_srv_a.Get();
    ID3D11ShaderResourceView* srv_b = g_blur.blur_srv_b.Get();

    ctx->OMSetRenderTargets(1, &rtv_a, nullptr);
    ctx->PSSetShader(g_blur.copy_ps.Get(), nullptr, 0);
    ctx->PSSetShaderResources(0, 1, &srv_copy);
    ctx->Draw(3, 0);
    ctx->PSSetShaderResources(0, 1, &null_srv);

    // Multi-pass separable Gaussian blur
    ctx->PSSetShader(g_blur.blur_ps.Get(), nullptr, 0);
    ID3D11Buffer* cb = g_blur.blur_cb.Get();
    ctx->PSSetConstantBuffers(0, 1, &cb);

    const auto set_step = [&](float sx, float sy) {
        D3D11_MAPPED_SUBRESOURCE mapped = {};
        if (SUCCEEDED(ctx->Map(cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
        {
            float* data = static_cast<float*>(mapped.pData);
            data[0] = sx;
            data[1] = sy;
            data[2] = 0.0f;
            data[3] = 0.0f;
            ctx->Unmap(cb, 0);
        }
    };

    const float inv_w = 1.0f / static_cast<float>(g_blur.blur_width);
    const float inv_h = 1.0f / static_cast<float>(g_blur.blur_height);
    static constexpr float kRadii[] = { 1.0f, 1.6f, 2.2f, 2.8f };
    for (float radius : kRadii)
    {
        set_step(radius * inv_w, 0.0f);
        ctx->OMSetRenderTargets(1, &rtv_b, nullptr);
        ctx->PSSetShaderResources(0, 1, &srv_a);
        ctx->Draw(3, 0);
        ctx->PSSetShaderResources(0, 1, &null_srv);

        set_step(0.0f, radius * inv_h);
        ctx->OMSetRenderTargets(1, &rtv_a, nullptr);
        ctx->PSSetShaderResources(0, 1, &srv_b);
        ctx->Draw(3, 0);
        ctx->PSSetShaderResources(0, 1, &null_srv);
    }

    // Composite blurred texture back onto main render target within menu clip rect
    ID3D11RenderTargetView* restore_rtv = main_rtv.Get();
    ctx->OMSetRenderTargets(1, &restore_rtv, main_dsv.Get());

    D3D11_VIEWPORT main_vp = {};
    main_vp.Width = static_cast<float>(g_blur.width);
    main_vp.Height = static_cast<float>(g_blur.height);
    main_vp.MaxDepth = 1.0f;
    ctx->RSSetViewports(1, &main_vp);
    ctx->RSSetState(g_blur.scissor_rs.Get());

    const ImDrawData* draw_data = ImGui::GetDrawData();
    const ImVec2 clip_off = draw_data ? draw_data->DisplayPos : ImVec2(0.0f, 0.0f);
    const ImVec2 clip_scale = draw_data ? draw_data->FramebufferScale : ImVec2(1.0f, 1.0f);
    D3D11_RECT scissor = {
        static_cast<LONG>((cmd->ClipRect.x - clip_off.x) * clip_scale.x),
        static_cast<LONG>((cmd->ClipRect.y - clip_off.y) * clip_scale.y),
        static_cast<LONG>((cmd->ClipRect.z - clip_off.x) * clip_scale.x),
        static_cast<LONG>((cmd->ClipRect.w - clip_off.y) * clip_scale.y)
    };
    ctx->RSSetScissorRects(1, &scissor);

    ctx->PSSetShader(g_blur.copy_ps.Get(), nullptr, 0);
    ctx->PSSetShaderResources(0, 1, &srv_a);
    ctx->Draw(3, 0);
    ctx->PSSetShaderResources(0, 1, &null_srv);
}

struct TabSpec
{
    const char* label;
    float width;
};

constexpr std::array<TabSpec, 6> kTabs = {{
    { "Ragebot", 105.0f }, { "Legitbot", 112.0f }, { "Visuals", 102.0f },
    { "Misc", 82.0f }, { "Config", 98.0f }, { "Preview", 104.0f }
}};

constexpr std::array<TabSpec, 3> kRagebotSections = {{
    { "Weapon", 80.0f }, { "Anti Aim", 88.0f }, { "Misc", 62.0f }
}};

constexpr std::array<TabSpec, 4> kVisualsSections = {{
    { "Enemy", 72.0f }, { "Team", 62.0f }, { "Local", 66.0f }, { "Other", 68.0f }
}};

constexpr std::array<TabSpec, 2> kMiscSections = {{
    { "Movement", 94.0f }, { "Other", 68.0f }
}};

constexpr std::array<TabSpec, 6> kPreviewSections = {{
    { "Combos", 82.0f }, { "Sliders", 78.0f }, { "Slider Check", 114.0f },
    { "Checkboxes", 108.0f }, { "Colors", 74.0f }, { "Buttons", 82.0f }
}};

constexpr float kTabsLeft = 12.0f;

float g_scale = 1.0f;

inline float S(float value)
{
    return value * g_scale;
}

inline ImVec2 S(float x, float y)
{
    return { x * g_scale, y * g_scale };
}

ImVec2 Add(ImVec2 point, float x, float y)
{
    return { point.x + x, point.y + y };
}

float Smooth(float current, float target, float speed, float delta_time)
{
    const float dt = std::clamp(delta_time, 0.0f, 1.0f / 15.0f);
    const float value = target + (current - target) * std::exp(-speed * dt);
    return std::abs(value - target) < 0.002f ? target : value;
}

float EaseOutCubic(float progress)
{
    const float remaining = 1.0f - std::clamp(progress, 0.0f, 1.0f);
    return 1.0f - remaining * remaining * remaining;
}

void SelectPage(State& state, SettingsTab tab, int section)
{
    if (state.tab == tab && state.section == section)
        return;
    const float current_offset = -static_cast<float>(state.page_direction) *
        (1.0f - EaseOutCubic(state.page_transition));
    state.previous_tab = state.tab;
    state.previous_section = state.section;
    state.previous_offset = current_offset;
    state.page_direction = tab != state.tab
        ? (static_cast<int>(tab) > static_cast<int>(state.tab) ? -1 : 1)
        : (section > state.section ? -1 : 1);
    state.tab = tab;
    state.section = section;
    state.page_transition = 0.0f;
}

bool TabButton(const char* id, const char* label, ImFont* font, ImVec2 position, float width, float height, bool selected, bool small)
{
    ImGui::SetCursorScreenPos(position);
    const bool clicked = ImGui::InvisibleButton(id, { width, height });
    const bool hovered = ImGui::IsItemHovered() || ImGui::IsItemFocused();
    const ImGuiID item_id = ImGui::GetItemID();
    ImGuiStorage* storage = ImGui::GetStateStorage();
    const ImGuiID active_key = ImHashStr("active_underline", 0, item_id);
    const ImGuiID hover_key = ImHashStr("hover_underline", 0, item_id);
    const float active_target = selected ? 1.0f : 0.0f;
    const float hover_target = hovered && !selected ? 1.0f : 0.0f;
    const float active = Smooth(storage->GetFloat(active_key, active_target), active_target, 13.0f, ImGui::GetIO().DeltaTime);
    const float hover = Smooth(storage->GetFloat(hover_key, hover_target), hover_target, 19.0f, ImGui::GetIO().DeltaTime);
    storage->SetFloat(active_key, active);
    storage->SetFloat(hover_key, hover);

    ImDrawList* draw = ImGui::GetWindowDrawList();
    const float font_size = S(small ? 14.0f : 18.0f);
    const ImVec2 text_size = font->CalcTextSizeA(font_size, FLT_MAX, 0.0f, label);
    const float text_x = std::floor(position.x + (width - text_size.x) * 0.5f + 0.5f);
    const float text_y = std::floor(position.y + (height - text_size.y) * 0.5f + 0.5f);
    const float emphasis = std::max(active, hover * 0.55f);
    const int text_channel = static_cast<int>(215.0f + 29.0f * emphasis);
    draw->AddText(font, font_size, { text_x, text_y }, IM_COL32(text_channel, text_channel, text_channel, 255), label);

    const float underline_width = std::floor(std::min(width - S(16.0f), text_size.x + S(20.0f)) + 0.5f);
    const float underline_x = std::floor(position.x + (width - underline_width) * 0.5f + 0.5f);
    draw->PushClipRect(position, Add(position, width, height), true);
    if (hover > 0.0f && active < 1.0f)
    {
        const float preview_y = position.y + height - S(2.0f);
        draw->AddRectFilled({ underline_x, preview_y }, { underline_x + underline_width, preview_y + 1.0f },
            IM_COL32(255, 255, 255, static_cast<int>(125.0f * hover * (1.0f - active))));
    }
    if (active > 0.0f)
    {
        const float underline_y = position.y + height - S(4.0f) + (1.0f - active) * S(8.0f);
        draw->AddRectFilled({ underline_x, underline_y },
            { underline_x + underline_width, underline_y + S(4.0f) },
            IM_COL32(244, 246, 248, static_cast<int>(255.0f * active)));
    }
    draw->PopClipRect();
    return clicked;
}

bool BeginTable(const char* id, float width)
{
    if (!ImGui::BeginTable(id, 2, ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp,
        { width, 0.0f }))
        return false;
    ImGui::TableSetupColumn("Setting", ImGuiTableColumnFlags_WidthStretch, 0.56f);
    ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch, 0.44f);
    return true;
}

void RowLabel(const char* label)
{
    ImGui::TableNextRow(0, S(48.0f));
    ImGui::TableSetColumnIndex(0);
    ImGui::PushFont(ImGui::GetFont(), S(25.0f));
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    ImGui::PopFont();
    ImGui::TableSetColumnIndex(1);
    ImGui::SetNextItemWidth(-1.0f);
}

void FormatDropdownLabel(const char* source, char (&destination)[128])
{
    bool word_start = true;
    size_t i = 0;
    for (; i + 1 < sizeof(destination) && source[i] != '\0'; ++i)
    {
        const unsigned char c = static_cast<unsigned char>(source[i]);
        if (std::isalpha(c))
        {
            destination[i] = static_cast<char>(word_start ? std::toupper(c) : std::tolower(c));
            word_start = false;
        }
        else
        {
            destination[i] = static_cast<char>(c);
            word_start = !std::isdigit(c);
        }
    }
    destination[i] = '\0';
}

enum class ComboAppearance { Filled, TextOnly };

void DrawHorizontalFade(ImDrawList* draw, const ImRect& rect, float opacity)
{
    const ImU32 left = ImGui::GetColorU32({ 0.04f, 0.05f, 0.06f, opacity });
    const ImU32 right = ImGui::GetColorU32({ 0.04f, 0.05f, 0.06f, 0.0f });
    draw->AddRectFilledMultiColor(rect.Min, rect.Max, left, right, right, left);
}

bool PanoramaCombo(const Fonts& fonts, const char* id, int* selected,
    const char* const* items, int count, ComboAppearance appearance)
{
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    if (window->SkipItems || selected == nullptr || items == nullptr || count <= 0)
        return false;

    const ImGuiID item_id = window->GetID(id);
    ImGuiStorage* storage = ImGui::GetStateStorage();
    const float dt = std::clamp(ImGui::GetIO().DeltaTime, 0.0f, 1.0f / 15.0f);
    const ImGuiID open_key = ImHashStr("combo_open", 0, item_id);
    const ImGuiID closing_key = ImHashStr("combo_closing", 0, item_id);
    const ImGuiID close_from_key = ImHashStr("combo_close_from", 0, item_id);
    const ImGuiID close_time_key = ImHashStr("combo_close_time", 0, item_id);
    const ImGuiID selection_key = ImHashStr("combo_selection", 0, item_id);
    const ImGuiID visual_selection_key = ImHashStr("combo_popup_visual_selection", 0, item_id);
    const ImGuiID previous_key = ImHashStr("combo_previous", 0, item_id);
    const ImGuiID transition_key = ImHashStr("combo_transition", 0, item_id);
    const int current_selection = std::clamp(*selected, 0, count - 1);
    int shown_selection = std::clamp(storage->GetInt(selection_key, current_selection), 0, count - 1);
    float text_transition = storage->GetFloat(transition_key, 1.0f);
    if (shown_selection != current_selection && storage->GetFloat(open_key, 0.0f) <= 0.0f &&
        !ImGui::IsPopupOpen(item_id, ImGuiPopupFlags_None))
    {
        storage->SetInt(previous_key, shown_selection);
        shown_selection = current_selection;
        text_transition = 0.0f;
    }
    storage->SetInt(selection_key, shown_selection);
    const float available_width = ImGui::CalcItemWidth();
    const bool text_only = appearance == ComboAppearance::TextOnly;
    const char* preview = items[shown_selection];
    const float font_size = S(17.0f);
    const ImVec2 text_size = fonts.stratum_bold->CalcTextSizeA(font_size, FLT_MAX, 0.0f, preview);
    const float target_width = text_only
        ? std::min(available_width, std::ceil(text_size.x + S(46.0f)))
        : available_width;
    const ImGuiID width_key = ImHashStr("combo_width", 0, item_id);
    const float width = Smooth(storage->GetFloat(width_key, target_width), target_width, 22.0f, dt);
    storage->SetFloat(width_key, width);
    ImVec2 position = window->DC.CursorPos;
    if (text_only)
    {
        position.x += available_width - width;
        ImGui::SetCursorScreenPos(position);
    }
    const ImRect frame(position, Add(position, width, S(40.0f)));
    ImGui::ItemSize(frame);
    if (!ImGui::ItemAdd(frame, item_id))
        return false;

    bool hovered = false;
    bool held = false;
    const bool pressed = ImGui::ButtonBehavior(frame, item_id, &hovered, &held);
    bool popup_open = ImGui::IsPopupOpen(item_id, ImGuiPopupFlags_None);
    bool closing = storage->GetInt(closing_key, 0) != 0;
    if (pressed && popup_open)
    {
        ImGuiContext& context = *ImGui::GetCurrentContext();
        for (int level = context.OpenPopupStack.Size - 1; level >= 0; --level)
        {
            if (context.OpenPopupStack[level].PopupId == item_id)
            {
                ImGui::ClosePopupToLevel(level, true);
                popup_open = false;
                closing = false;
                break;
            }
        }
    }
    else if (pressed)
    {
        ImGui::OpenPopupEx(item_id, ImGuiPopupFlags_None);
        popup_open = true;
    }
    if (!popup_open)
        closing = false;
    storage->SetInt(closing_key, closing ? 1 : 0);

    const ImGuiID hover_key = ImHashStr("combo_hover", 0, item_id);
    const ImGuiID press_key = ImHashStr("combo_press", 0, item_id);
    const float hover_target = hovered || popup_open ? 1.0f : 0.0f;
    const float hover = Smooth(storage->GetFloat(hover_key, hover_target), hover_target,
        16.0f, dt);
    storage->SetFloat(hover_key, hover);
    const float press = Smooth(storage->GetFloat(press_key, 0.0f), held ? 1.0f : 0.0f, 22.0f, dt);
    storage->SetFloat(press_key, press);
    float open_progress = 0.0f;
    if (closing)
    {
        const float close_time = std::min(1.0f, storage->GetFloat(close_time_key, 0.0f) + dt / 0.18f);
        storage->SetFloat(close_time_key, close_time);
        const float close_eased = close_time * close_time * (3.0f - 2.0f * close_time);
        open_progress = storage->GetFloat(close_from_key, 1.0f) * (1.0f - close_eased);
    }
    else
    {
        open_progress = std::clamp(storage->GetFloat(open_key, 0.0f) +
            dt / (popup_open ? 0.19f : -0.15f), 0.0f, 1.0f);
    }
    storage->SetFloat(open_key, open_progress);
    if (closing && open_progress <= 0.0f)
    {
        ImGuiContext& context = *ImGui::GetCurrentContext();
        for (int level = context.OpenPopupStack.Size - 1; level >= 0; --level)
        {
            if (context.OpenPopupStack[level].PopupId == item_id)
            {
                ImGui::ClosePopupToLevel(level, true);
                break;
            }
        }
        popup_open = false;
        closing = false;
        storage->SetInt(closing_key, 0);
    }
    const float open_eased = EaseOutCubic(open_progress);
    const float popup_alpha = open_progress;

    text_transition = std::min(1.0f, text_transition + dt / 0.18f);
    storage->SetFloat(transition_key, text_transition);
    const float text_eased = EaseOutCubic(text_transition);

    ImDrawList* draw = window->DrawList;
    if (!text_only)
        draw->AddRectFilled(frame.Min, frame.Max,
            ImGui::GetColorU32({ 0.11f, 0.12f, 0.13f, 0.28f }), 2.0f);
    if (hover > 0.0f)
    {
        if (text_only)
            draw->AddRectFilled(frame.Min, frame.Max,
                ImGui::GetColorU32({ 0.09f, 0.10f, 0.11f, 0.08f * hover }), 2.0f);
        DrawHorizontalFade(draw, frame, (text_only ? 0.33f : 0.16f) * hover + 0.08f * press);
    }

    const ImVec2 text_pos = { frame.Min.x + S(text_only ? 12.0f : 14.0f),
        std::floor(frame.Min.y + (frame.GetHeight() - text_size.y) * 0.5f + 0.5f) };
    draw->PushClipRect(Add(frame.Min, S(text_only ? 11.0f : 13.0f), 0.0f),
        Add(frame.Max, S(text_only ? -28.0f : -43.0f), 0.0f), true);
    if (text_transition < 1.0f)
    {
        const int previous_selection = std::clamp(storage->GetInt(previous_key, current_selection), 0, count - 1);
        draw->AddText(fonts.stratum_bold, font_size, Add(text_pos, 0.0f, -S(5.0f) * text_eased),
            ImGui::GetColorU32({ 0.95f, 0.96f, 0.97f, 1.0f - text_eased }), items[previous_selection]);
    }
    draw->AddText(fonts.stratum_bold, font_size, Add(text_pos, 0.0f, S(5.0f) * (1.0f - text_eased)),
        ImGui::GetColorU32({ 0.95f, 0.96f, 0.97f, text_eased }), preview);
    draw->PopClipRect();

    const float arrow_x = std::floor(frame.Max.x - S(text_only ? 20.0f : 24.0f) + 0.5f);
    const float arrow_y = std::floor(frame.Min.y + frame.GetHeight() * 0.5f + 0.5f);
    const ImU32 arrow_color = ImGui::GetColorU32({ 0.97f, 0.98f, 1.0f, 1.0f });
    const float arrow_angle = IM_PI * open_eased;
    const float arrow_cos = std::cos(arrow_angle);
    const float arrow_sin = std::sin(arrow_angle);
    const auto arrow_point = [&](float x, float y) -> ImVec2 {
        return { arrow_x + x * arrow_cos - y * arrow_sin,
            arrow_y + x * arrow_sin + y * arrow_cos };
    };
    const ImVec2 arrow_points[3] = {
        arrow_point(-S(5.5f), -S(2.75f)),
        arrow_point(0.0f, S(2.75f)),
        arrow_point(S(5.5f), -S(2.75f))
    };
    draw->AddPolyline(arrow_points, 3, arrow_color, ImDrawFlags_None, std::max(1.5f, S(2.0f)));
    ImGui::RenderNavCursor(frame, item_id);

    if (!popup_open && open_progress <= 0.0f)
        return false;

    const float item_height = S(40.0f);
    const float popup_font_size = S(20.0f);
    float popup_width = width;
    if (text_only)
    {
        float longest_label = 0.0f;
        for (int i = 0; i < count; ++i)
        {
            char display_label[128];
            FormatDropdownLabel(items[i], display_label);
            longest_label = std::max(longest_label,
                fonts.noto_regular->CalcTextSizeA(popup_font_size, FLT_MAX, 0.0f, display_label).x);
        }
        popup_width = std::max(width, std::ceil(longest_label + S(44.0f)));
    }
    const float popup_height = S(16.0f) + item_height * static_cast<float>(std::min(count, 6));
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    float popup_y = frame.Max.y;
    if (popup_y + popup_height > viewport->WorkPos.y + viewport->WorkSize.y &&
        frame.Min.y - popup_height >= viewport->WorkPos.y)
        popup_y = frame.Min.y - popup_height;
    const float popup_x = text_only ? frame.Max.x - popup_width : frame.Min.x;
    if (!popup_open)
    {
        if ((window->Flags & ImGuiWindowFlags_NoInputs) ||
            ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel))
            return false;
        const float ghost_width = storage->GetFloat(ImHashStr("combo_popup_width", 0, item_id), popup_width);
        const float ghost_height = storage->GetFloat(ImHashStr("combo_popup_height", 0, item_id), popup_height);
        const float ghost_offset = storage->GetFloat(ImHashStr("combo_popup_offset", 0, item_id), popup_y - frame.Max.y);
        const int visual_selection = std::clamp(storage->GetInt(visual_selection_key, current_selection), 0, count - 1);
        const ImVec2 ghost_min = { text_only ? frame.Max.x - ghost_width : frame.Min.x,
            frame.Max.y + ghost_offset };
        const ImVec2 ghost_max = Add(ghost_min, ghost_width, ghost_height);
        ImDrawList* closing_draw = ImGui::GetForegroundDrawList();
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * popup_alpha);
        closing_draw->AddRectFilled(ghost_min, ghost_max,
            ImGui::GetColorU32(IM_COL32(44, 43, 43, 251)), 3.0f);
        closing_draw->PushClipRect(ghost_min, ghost_max, true);
        for (int i = 0; i < std::min(count, 6); ++i)
        {
            const ImVec2 row_min = Add(ghost_min, 0.0f, S(8.0f) + item_height * i);
            const ImVec2 row_max = Add(row_min, ghost_width, item_height);
            const ImGuiID row_id = ImHashData(&i, sizeof(i), item_id);
            const ImGuiID row_hover_key = ImHashStr("combo_row_hover", 0, row_id);
            const ImGuiID row_press_key = ImHashStr("combo_row_press", 0, row_id);
            const ImGuiID row_selected_key = ImHashStr("combo_row_selected", 0, row_id);
            const float row_hover = Smooth(storage->GetFloat(row_hover_key, 0.0f), 0.0f, 13.0f, dt);
            const float row_press = Smooth(storage->GetFloat(row_press_key, 0.0f), 0.0f, 18.0f, dt);
            const float row_selected = Smooth(storage->GetFloat(row_selected_key, i == visual_selection ? 1.0f : 0.0f),
                i == visual_selection ? 1.0f : 0.0f, 16.0f, dt);
            storage->SetFloat(row_hover_key, row_hover);
            storage->SetFloat(row_press_key, row_press);
            storage->SetFloat(row_selected_key, row_selected);
            const float row_opacity = 0.46f * row_hover + 0.12f * row_press +
                0.18f * row_selected * (1.0f - row_hover);
            if (row_opacity > 0.0f)
                DrawHorizontalFade(closing_draw, ImRect(row_min, row_max), row_opacity);
            char display_label[128];
            FormatDropdownLabel(items[i], display_label);
            const ImVec2 row_text_size = fonts.noto_regular->CalcTextSizeA(popup_font_size, FLT_MAX, 0.0f, display_label);
            const ImVec2 row_text_pos = { row_min.x + S(23.0f) + S(3.0f) * row_hover,
                std::floor(row_min.y + (item_height - row_text_size.y) * 0.5f + 0.5f) };
            closing_draw->PushClipRect(Add(row_min, S(15.0f), 0.0f), Add(row_max, -S(10.0f), 0.0f), true);
            closing_draw->AddText(fonts.noto_regular, popup_font_size, row_text_pos,
                ImGui::GetColorU32({ 0.84f, 0.85f, 0.86f, 1.0f - 0.08f * row_selected }), display_label);
            closing_draw->PopClipRect();
        }
        closing_draw->PopClipRect();
        ImGui::PopStyleVar();
        return false;
    }
    storage->SetFloat(ImHashStr("combo_popup_width", 0, item_id), popup_width);
    storage->SetFloat(ImHashStr("combo_popup_height", 0, item_id), popup_height);
    storage->SetFloat(ImHashStr("combo_popup_offset", 0, item_id), popup_y - frame.Max.y);
    if (!closing)
        storage->SetInt(visual_selection_key, current_selection);
    ImGui::SetNextWindowPos({ popup_x, popup_y });
    ImGui::SetNextWindowSize({ popup_width, popup_height });
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * popup_alpha);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, { 0.0f, S(8.0f) });
    ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, 3.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, { 0.0f, 0.0f });
    ImGui::PushStyleColor(ImGuiCol_PopupBg, IM_COL32(44, 43, 43, 251));
    bool changed = false;
    int chosen_index = -1;
    if (ImGui::BeginPopupEx(item_id, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoSavedSettings |
        (closing ? ImGuiWindowFlags_NoInputs : ImGuiWindowFlags_None)))
    {
        ImGui::PushFont(fonts.noto_regular, popup_font_size);
        for (int i = 0; i < count; ++i)
        {
            char display_label[128];
            FormatDropdownLabel(items[i], display_label);
            ImGui::PushID(i);
            const bool chosen = ImGui::InvisibleButton("##option", { popup_width, item_height });
            const ImVec2 row_min = ImGui::GetItemRectMin();
            const ImVec2 row_max = ImGui::GetItemRectMax();
            const bool row_hovered = !closing && (ImGui::IsItemHovered() || ImGui::IsItemFocused());
            const ImGuiID row_key = ImHashStr("combo_row_hover", 0, ImHashData(&i, sizeof(i), item_id));
            const ImGuiID row_press_key = ImHashStr("combo_row_press", 0, ImHashData(&i, sizeof(i), item_id));
            const ImGuiID row_selected_key = ImHashStr("combo_row_selected", 0, ImHashData(&i, sizeof(i), item_id));
            const float row_hover = Smooth(storage->GetFloat(row_key, 0.0f), row_hovered ? 1.0f : 0.0f, 19.0f, dt);
            storage->SetFloat(row_key, row_hover);
            const float row_press = Smooth(storage->GetFloat(row_press_key, 0.0f),
                ImGui::IsItemActive() ? 1.0f : 0.0f, 24.0f, dt);
            storage->SetFloat(row_press_key, row_press);
            const int visual_selection = storage->GetInt(visual_selection_key, current_selection);
            const float row_selected = Smooth(storage->GetFloat(row_selected_key, i == visual_selection ? 1.0f : 0.0f),
                i == visual_selection ? 1.0f : 0.0f, 16.0f, dt);
            storage->SetFloat(row_selected_key, row_selected);
            const float row_opacity = 0.46f * row_hover + 0.12f * row_press +
                0.18f * row_selected * (1.0f - row_hover);
            if (row_opacity > 0.0f)
                DrawHorizontalFade(ImGui::GetWindowDrawList(), ImRect(row_min, row_max), row_opacity);
            const ImVec2 row_text_size = fonts.noto_regular->CalcTextSizeA(popup_font_size, FLT_MAX, 0.0f, display_label);
            const ImVec2 row_text_pos = { row_min.x + S(23.0f) + S(3.0f) * row_hover,
                std::floor(row_min.y + (item_height - row_text_size.y) * 0.5f + 0.5f) };
            ImGui::GetWindowDrawList()->PushClipRect(Add(row_min, S(15.0f), 0.0f),
                Add(row_max, -S(10.0f), 0.0f), true);
            ImGui::GetWindowDrawList()->AddText(fonts.noto_regular, popup_font_size, row_text_pos,
                ImGui::GetColorU32({ 0.84f, 0.85f, 0.86f, 1.0f - 0.08f * row_selected }), display_label);
            ImGui::GetWindowDrawList()->PopClipRect();
            if (!closing && i == *selected)
                ImGui::SetItemDefaultFocus();
            if (!closing && chosen)
                chosen_index = i;
            ImGui::PopID();
        }
        if (chosen_index >= 0)
        {
            changed = *selected != chosen_index;
            *selected = chosen_index;
            if (changed)
                ImGui::MarkItemEdited(item_id);
            closing = true;
            storage->SetInt(closing_key, 1);
            storage->SetFloat(close_from_key, open_progress);
            storage->SetFloat(close_time_key, 0.0f);
        }
        ImGui::PopFont();
        ImGui::EndPopup();
    }
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(5);
    return changed;
}

void RowBool(const Fonts& fonts, const char* label, const char* id, bool& value)
{
    RowLabel(label);
    int selected = value ? 0 : 1;
    static constexpr const char* items[] = { "YES", "NO" };
    if (PanoramaCombo(fonts, id, &selected, items, 2, ComboAppearance::TextOnly))
        value = selected == 0;
}

void RowCombo(const Fonts& fonts, const char* label, const char* id, int& value,
    const char* const* items, int count)
{
    RowLabel(label);
    PanoramaCombo(fonts, id, &value, items, count, ComboAppearance::TextOnly);
}

void RowBoolFilled(const Fonts& fonts, const char* label, const char* id, bool& value)
{
    RowLabel(label);
    int selected = value ? 0 : 1;
    static constexpr const char* items[] = { "YES", "NO" };
    if (PanoramaCombo(fonts, id, &selected, items, 2, ComboAppearance::Filled))
        value = selected == 0;
}

void RowComboFilled(const Fonts& fonts, const char* label, const char* id, int& value,
    const char* const* items, int count)
{
    RowLabel(label);
    PanoramaCombo(fonts, id, &value, items, count, ComboAppearance::Filled);
}

bool PanoramaCheckbox(const char* id, ImVec2 pos, bool* value)
{
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    if (window->SkipItems || value == nullptr)
        return false;

    const float box_size = S(22.0f);
    const ImGuiID item_id = window->GetID(id);
    const ImRect rect(pos, Add(pos, box_size, box_size));
    ImGui::ItemSize(rect);
    if (!ImGui::ItemAdd(rect, item_id))
        return false;

    bool hovered = false;
    bool held = false;
    const bool pressed = ImGui::ButtonBehavior(rect, item_id, &hovered, &held);
    if (pressed)
    {
        *value = !*value;
        ImGui::MarkItemEdited(item_id);
    }

    ImGuiStorage* storage = ImGui::GetStateStorage();
    const float dt = std::clamp(ImGui::GetIO().DeltaTime, 0.0f, 1.0f / 15.0f);
    const ImGuiID check_key = ImHashStr("chk_anim", 0, item_id);
    const ImGuiID hover_key = ImHashStr("chk_hover", 0, item_id);
    const float check_target = *value ? 1.0f : 0.0f;
    const float hover_target = hovered ? 1.0f : 0.0f;
    const float checked = Smooth(storage->GetFloat(check_key, check_target), check_target, 20.0f, dt);
    const float hover = Smooth(storage->GetFloat(hover_key, hover_target), hover_target, 18.0f, dt);
    storage->SetFloat(check_key, checked);
    storage->SetFloat(hover_key, hover);

    ImDrawList* draw = window->DrawList;
    draw->AddRectFilled(rect.Min, rect.Max,
        ImGui::GetColorU32({ 0.12f, 0.13f, 0.15f, (0.32f + 0.16f * hover) * (1.0f - checked) }), 2.5f);
    if (checked < 1.0f)
        draw->AddRect(rect.Min, rect.Max,
            ImGui::GetColorU32({ 0.88f, 0.90f, 0.92f, (0.42f + 0.28f * hover) * (1.0f - checked) }), 2.5f, 0, 1.2f);

    if (checked > 0.0f)
    {
        draw->AddRectFilled(rect.Min, rect.Max,
            ImGui::GetColorU32({ 0.98f, 0.98f, 0.99f, checked }), 2.5f);
        const ImVec2 check_pts[3] = {
            { rect.Min.x + S(4.5f), rect.Min.y + S(11.2f) },
            { rect.Min.x + S(9.2f), rect.Min.y + S(15.8f) },
            { rect.Min.x + S(17.5f), rect.Min.y + S(6.5f) }
        };
        draw->AddPolyline(check_pts, 3,
            ImGui::GetColorU32({ 0.30f, 0.28f, 0.26f, checked }),
            ImDrawFlags_None, std::max(1.6f, S(2.2f)));
    }
    ImGui::RenderNavCursor(rect, item_id);
    return pressed;
}

void RowCheckbox(const Fonts& fonts, const char* label, const char* id, bool& value)
{
    (void)fonts;
    RowLabel(label);
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    if (window->SkipItems)
        return;

    const ImVec2 start = window->DC.CursorPos;
    const float available = ImGui::CalcItemWidth();
    const float box_size = S(22.0f);
    const float height = S(40.0f);
    const ImVec2 check_pos = {
        std::floor(start.x + available - box_size + 0.5f),
        std::floor(start.y + (height - box_size) * 0.5f + 0.5f)
    };
    ImGui::SetCursorScreenPos(check_pos);
    PanoramaCheckbox(id, check_pos, &value);
    ImGui::SetCursorScreenPos({ start.x, start.y + height });
    ImGui::Dummy({ 0.0f, 0.0f });
}

bool PanoramaSliderScalar(const Fonts& fonts, const char* id, ImGuiDataType data_type,
    void* value, const void* min_value, const void* max_value, const char* format, bool* enabled = nullptr)
{
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    if (window->SkipItems)
        return false;

    ImGuiContext& context = *ImGui::GetCurrentContext();
    const ImVec2 start = window->DC.CursorPos;
    const float available = ImGui::CalcItemWidth();
    const float check_slot = S(34.0f);
    const float value_width = S(80.0f);
    const float gap = S(20.0f);
    const float height = S(40.0f);
    const float box_size = S(22.0f);
    const float rail_width = std::max(S(80.0f), available - check_slot - value_width - gap);

    bool check_changed = false;
    if (enabled != nullptr)
    {
        ImGui::PushID(id);
        const ImVec2 check_pos = {
            std::floor(start.x + 0.5f),
            std::floor(start.y + (height - box_size) * 0.5f + 0.5f)
        };
        check_changed = PanoramaCheckbox("##check", check_pos, enabled);
        ImGui::PopID();
    }

    const ImVec2 rail_start = { start.x + check_slot, start.y };
    ImGui::SetCursorScreenPos(rail_start);
    const ImGuiID slider_id = window->GetID(id);
    const ImRect hit_box(rail_start, Add(rail_start, rail_width, height));
    ImGui::ItemSize(hit_box, ImGui::GetStyle().FramePadding.y);

    bool changed = false;
    if (ImGui::ItemAdd(hit_box, slider_id))
    {
        ImGuiStorage* storage = ImGui::GetStateStorage();
        const ImGuiID drag_x_key = ImHashStr("slider_drag_x", 0, slider_id);
        const ImGuiID drag_w_key = ImHashStr("slider_drag_w", 0, slider_id);
        const bool hovered = ImGui::ItemHoverable(hit_box, slider_id, context.LastItemData.ItemFlags);
        const bool clicked = hovered && ImGui::IsMouseClicked(0, ImGuiInputFlags_None, slider_id);
        if (clicked || context.NavActivateId == slider_id)
        {
            if (clicked)
                ImGui::SetKeyOwner(ImGuiKey_MouseLeft, slider_id);
            ImGui::SetActiveID(slider_id, window);
            ImGui::SetFocusID(slider_id, window);
            ImGui::FocusWindow(window);
            context.ActiveIdUsingNavDirMask |= (1 << ImGuiDir_Left) | (1 << ImGuiDir_Right);
            storage->SetFloat(drag_x_key, hit_box.Min.x);
            storage->SetFloat(drag_w_key, hit_box.GetWidth());
        }

        ImRect behavior_box = hit_box;
        if (context.ActiveId == slider_id)
        {
            const float drag_x = storage->GetFloat(drag_x_key, hit_box.Min.x);
            const float drag_w = storage->GetFloat(drag_w_key, hit_box.GetWidth());
            behavior_box = ImRect({ drag_x, hit_box.Min.y }, { drag_x + drag_w, hit_box.Max.y });
        }

        ImRect grab_box;
        changed = ImGui::SliderBehavior(behavior_box, slider_id, data_type, value,
            min_value, max_value, format, ImGuiSliderFlags_AlwaysClamp, &grab_box);
        if (changed)
            ImGui::MarkItemEdited(slider_id);

        const ImGuiID hover_key = ImHashStr("slider_hover", 0, slider_id);
        const ImGuiID active_key = ImHashStr("slider_active", 0, slider_id);
        const float dt = ImGui::GetIO().DeltaTime;
        const float hover_target = hovered || context.ActiveId == slider_id ? 1.0f : 0.0f;
        const float hover = Smooth(storage->GetFloat(hover_key, hover_target), hover_target, 17.0f, dt);
        const float active = Smooth(storage->GetFloat(active_key, 0.0f),
            context.ActiveId == slider_id ? 1.0f : 0.0f, 21.0f, dt);
        storage->SetFloat(hover_key, hover);
        storage->SetFloat(active_key, active);

        ImDrawList* draw = window->DrawList;
        const float rail_y = std::floor(rail_start.y + height * 0.5f + 0.5f);
        const float rail_half_h = S(4.0f);
        const ImVec2 rail_min = { std::floor(rail_start.x + 0.5f), rail_y - rail_half_h };
        const ImVec2 rail_max = { std::floor(rail_start.x + rail_width + 0.5f), rail_y + rail_half_h };
        draw->AddRectFilled(rail_min, rail_max,
            ImGui::GetColorU32({ 0.79f, 0.81f, 0.84f, 0.30f + 0.12f * hover }), 1.0f);

        const float behavior_w = std::max(1.0f, behavior_box.GetWidth());
        const float grab_rel = (((grab_box.Min.x + grab_box.Max.x) * 0.5f) - behavior_box.Min.x) / behavior_w;
        const float grab_center_x = std::floor(rail_min.x + std::clamp(grab_rel, 0.0f, 1.0f) * rail_width + 0.5f);
        const float fill_end_x = std::clamp(grab_center_x, rail_min.x, rail_max.x);
        if (fill_end_x > rail_min.x)
            draw->AddRectFilled(rail_min, { fill_end_x, rail_max.y },
                ImGui::GetColorU32({ 0.90f, 0.90f, 0.91f, 0.40f + 0.06f * hover }),
                1.0f, ImDrawFlags_RoundCornersLeft);
        const float grab_width = S(16.0f + 2.0f * active);
        const float grab_height = S(10.0f + 2.0f * active);
        draw->AddRectFilled({ grab_center_x - grab_width * 0.5f, rail_y - grab_height * 0.5f },
            { grab_center_x + grab_width * 0.5f, rail_y + grab_height * 0.5f },
            ImGui::GetColorU32({ 0.85f + 0.10f * hover, 0.86f + 0.10f * hover,
                0.88f + 0.09f * hover, 0.95f }), 2.0f);
        ImGui::RenderNavCursor(hit_box, slider_id);
    }

    ImGui::SetCursorScreenPos({ rail_start.x + rail_width + gap, rail_start.y + S(2.0f) });
    ImGui::PushID(id);
    ImGui::PushFont(fonts.stratum_bold, S(18.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, S(10.0f, 8.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 2.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, { 0.15f, 0.16f, 0.17f, 0.07f });
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, { 0.15f, 0.16f, 0.17f, 0.22f });
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, { 0.13f, 0.14f, 0.15f, 0.34f });
    ImGui::PushStyleColor(ImGuiCol_Border, { 0.83f, 0.84f, 0.86f, 0.19f });
    ImGui::PushStyleColor(ImGuiCol_Text, { 0.86f, 0.87f, 0.89f, 1.0f });
    ImGui::SetNextItemWidth(value_width);
    const bool input_changed = ImGui::InputScalar("##value", data_type, value,
        nullptr, nullptr, format);
    if (input_changed)
    {
        if (data_type == ImGuiDataType_Float)
            *static_cast<float*>(value) = std::clamp(*static_cast<float*>(value),
                *static_cast<const float*>(min_value), *static_cast<const float*>(max_value));
        else
            *static_cast<int*>(value) = std::clamp(*static_cast<int*>(value),
                *static_cast<const int*>(min_value), *static_cast<const int*>(max_value));
    }
    ImGui::PopStyleColor(5);
    ImGui::PopStyleVar(3);
    ImGui::PopFont();
    ImGui::PopID();
    return check_changed || changed || input_changed;
}

void RowSlider(const Fonts& fonts, const char* label, const char* id,
    float& value, float min_value, float max_value, const char* format)
{
    RowLabel(label);
    PanoramaSliderScalar(fonts, id, ImGuiDataType_Float, &value, &min_value, &max_value, format, nullptr);
}

void RowSliderInt(const Fonts& fonts, const char* label, const char* id,
    int& value, int min_value, int max_value, const char* format)
{
    RowLabel(label);
    PanoramaSliderScalar(fonts, id, ImGuiDataType_S32, &value, &min_value, &max_value, format, nullptr);
}

void RowSliderCheck(const Fonts& fonts, const char* label, const char* id,
    bool& enabled, float& value, float min_value, float max_value, const char* format)
{
    RowLabel(label);
    PanoramaSliderScalar(fonts, id, ImGuiDataType_Float, &value, &min_value, &max_value, format, &enabled);
}

void RowSliderIntCheck(const Fonts& fonts, const char* label, const char* id,
    bool& enabled, int& value, int min_value, int max_value, const char* format)
{
    RowLabel(label);
    PanoramaSliderScalar(fonts, id, ImGuiDataType_S32, &value, &min_value, &max_value, format, &enabled);
}

void RowColorPreview(const Fonts& fonts, const char* label, const char* id, const ColorRGBA& color)
{
    RowLabel(label);
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    if (window->SkipItems)
        return;

    const ImVec2 start = window->DC.CursorPos;
    const float available = ImGui::CalcItemWidth();
    const float check_slot = S(34.0f);
    const float value_width = S(80.0f);
    const float gap = S(20.0f);
    const float height = S(40.0f);
    const float rail_width = std::max(S(80.0f), available - check_slot - value_width - gap);

    const ImRect total_rect(start, Add(start, available, height));
    ImGui::ItemSize(total_rect, ImGui::GetStyle().FramePadding.y);
    if (!ImGui::ItemAdd(total_rect, window->GetID(id)))
        return;

    const int r = std::clamp(color.r, 0, 255);
    const int g = std::clamp(color.g, 0, 255);
    const int b = std::clamp(color.b, 0, 255);
    const int a = color.alpha_enabled ? std::clamp(color.a, 0, 255) : 255;

    ImDrawList* draw = window->DrawList;
    const float bar_height = S(24.0f);
    const ImVec2 bar_min = {
        std::floor(start.x + check_slot + 0.5f),
        std::floor(start.y + (height - bar_height) * 0.5f + 0.5f)
    };
    const ImVec2 bar_max = Add(bar_min, rail_width, bar_height);

    // Checkerboard background to visualize alpha transparency
    draw->PushClipRect(bar_min, bar_max, true);
    const float tile = std::max(3.0f, S(6.0f));
    for (float y = bar_min.y; y < bar_max.y; y += tile)
    {
        for (float x = bar_min.x; x < bar_max.x; x += tile)
        {
            const bool dark = ((static_cast<int>((x - bar_min.x) / tile) +
                static_cast<int>((y - bar_min.y) / tile)) & 1) != 0;
            draw->AddRectFilled({ x, y }, { std::min(x + tile, bar_max.x), std::min(y + tile, bar_max.y) },
                dark ? IM_COL32(105, 108, 114, 255) : IM_COL32(162, 166, 172, 255));
        }
    }
    // Left segment solid RGB, right segment RGBA with alpha
    const float solid_split = std::floor(bar_min.x + rail_width * 0.28f + 0.5f);
    draw->AddRectFilled(bar_min, { solid_split, bar_max.y }, IM_COL32(r, g, b, 255));
    draw->AddRectFilled({ solid_split, bar_min.y }, bar_max, IM_COL32(r, g, b, a));
    draw->PopClipRect();

    draw->AddRect(bar_min, bar_max, IM_COL32(238, 240, 244, 72), 2.0f);

    // Hex readout box on the right aligned with slider input boxes
    const float hex_h = S(36.0f);
    const ImVec2 hex_min = { start.x + check_slot + rail_width + gap, start.y + S(2.0f) };
    const ImVec2 hex_max = Add(hex_min, value_width, hex_h);
    draw->AddRectFilled(hex_min, hex_max, ImGui::GetColorU32({ 0.15f, 0.16f, 0.17f, 0.12f }), 2.0f);
    draw->AddRect(hex_min, hex_max, ImGui::GetColorU32({ 0.83f, 0.84f, 0.86f, 0.19f }), 2.0f);

    char hex_text[16];
    std::snprintf(hex_text, sizeof(hex_text), "#%02X%02X%02X", r, g, b);
    const float hex_font_size = S(15.0f);
    const ImVec2 hex_size = fonts.stratum_bold->CalcTextSizeA(hex_font_size, FLT_MAX, 0.0f, hex_text);
    const ImVec2 hex_pos = {
        std::floor(hex_min.x + (value_width - hex_size.x) * 0.5f + 0.5f),
        std::floor(hex_min.y + (hex_h - hex_size.y) * 0.5f + 0.5f)
    };
    draw->AddText(fonts.stratum_bold, hex_font_size, hex_pos,
        ImGui::GetColorU32({ 0.88f, 0.89f, 0.91f, 1.0f }), hex_text);
}

void RenderColorRows(const Fonts& fonts, const char* id_prefix, ColorRGBA& color)
{
    ImGui::PushID(id_prefix);
    RowColorPreview(fonts, "Preview Color", "##preview", color);
    RowSliderInt(fonts, "Red", "##r", color.r, 0, 255, "%d");
    RowSliderInt(fonts, "Green", "##g", color.g, 0, 255, "%d");
    RowSliderInt(fonts, "Blue", "##b", color.b, 0, 255, "%d");
    RowSliderIntCheck(fonts, "Alpha", "##a", color.alpha_enabled, color.a, 0, 255, "%d");
    ImGui::PopID();
}

void SectionTitle(const Fonts& fonts, const char* name, float left)
{
    ImGui::SetCursorPosX(left);
    ImGui::PushFont(fonts.stratum_bold, S(21.0f));
    ImGui::TextUnformatted(name);
    ImGui::PopFont();
    ImGui::Dummy({ 0.0f, S(8.0f) });
    ImGui::SetCursorPosX(left);
}

void RenderRagebot(State& state, const Fonts& fonts, float left, float width)
{
    static constexpr const char* target_selections[] = { "NEAREST", "CROSSHAIR", "LOWEST HP", "HIGHEST DAMAGE" };
    static constexpr const char* pitches[] = { "NONE", "DOWN", "UP", "ZERO" };
    static constexpr const char* yaw_bases[] = { "LOCAL VIEW", "AT TARGETS" };
    static constexpr const char* yaws[] = { "NONE", "BACKWARD", "SPIN" };
    static constexpr const char* auto_stops[] = { "OFF", "EARLY", "FULL STOP" };

    switch (static_cast<RagebotSection>(state.section))
    {
    case RagebotSection::Weapon:
        SectionTitle(fonts, "Weapon", left);
        if (BeginTable("##rage_weapon", width))
        {
            RowBool(fonts, "Enable Ragebot", "##rage_enabled", state.rage_enabled);
            RowCombo(fonts, "Target Selection", "##rage_target", state.rage_target_selection, target_selections, IM_ARRAYSIZE(target_selections));
            RowSlider(fonts, "Field of View", "##rage_fov", state.rage_fov, 0.0f, 180.0f, "%.0f deg");
            RowSliderInt(fonts, "Hitchance", "##rage_hc", state.rage_hitchance, 0, 100, "%d%%");
            RowSliderInt(fonts, "Minimum Damage", "##rage_mindmg", state.rage_min_damage, 1, 100, "%d hp");
            RowSliderIntCheck(fonts, "Damage Override", "##rage_dmg_ovr", state.rage_min_damage_override, state.rage_override_damage, 1, 100, "%d hp");
            RowBool(fonts, "Auto Fire", "##rage_autofire", state.rage_auto_fire);
            RowBool(fonts, "Auto Scope", "##rage_autoscope", state.rage_auto_scope);
            ImGui::EndTable();
        }
        break;
    case RagebotSection::AntiAim:
        SectionTitle(fonts, "Anti Aim", left);
        if (BeginTable("##rage_antiaim", width))
        {
            RowBool(fonts, "Enable Anti Aim", "##aa_enabled", state.aa_enabled);
            RowCombo(fonts, "Pitch", "##aa_pitch", state.aa_pitch, pitches, IM_ARRAYSIZE(pitches));
            RowCombo(fonts, "Yaw Base", "##aa_yawbase", state.aa_yaw_base, yaw_bases, IM_ARRAYSIZE(yaw_bases));
            RowCombo(fonts, "Yaw", "##aa_yaw", state.aa_yaw, yaws, IM_ARRAYSIZE(yaws));
            RowSlider(fonts, "Yaw Offset", "##aa_yawoffset", state.aa_yaw_offset, -180.0f, 180.0f, "%.0f deg");
            RowSliderCheck(fonts, "Jitter Range", "##aa_jitter", state.aa_jitter_enabled, state.aa_jitter_range, 0.0f, 90.0f, "%.0f deg");
            ImGui::EndTable();
        }
        break;
    case RagebotSection::Misc:
        SectionTitle(fonts, "Misc", left);
        if (BeginTable("##rage_misc", width))
        {
            RowBool(fonts, "Remove Recoil", "##rage_norecoil", state.rage_remove_recoil);
            RowCombo(fonts, "Auto Stop", "##rage_autostop", state.rage_auto_stop, auto_stops, IM_ARRAYSIZE(auto_stops));
            RowBool(fonts, "Double Tap", "##rage_dt", state.rage_double_tap);
            RowBool(fonts, "Hide Shots", "##rage_hs", state.rage_hide_shots);
            ImGui::EndTable();
        }
        break;
    }
}

void RenderLegitbot(State& state, const Fonts& fonts, float left, float width)
{
    static constexpr const char* hitboxes[] = { "HEAD", "NECK", "CHEST", "NEAREST" };
    SectionTitle(fonts, "Legitbot", left);
    if (BeginTable("##legitbot", width))
    {
        RowBool(fonts, "Enable Legitbot", "##legit_enabled", state.legit_enabled);
        RowCombo(fonts, "Aimbot Hitbox", "##legit_hitbox", state.legit_hitbox, hitboxes, IM_ARRAYSIZE(hitboxes));
        RowSlider(fonts, "Field of View", "##legit_fov", state.legit_fov, 0.5f, 30.0f, "%.1f deg");
        RowSlider(fonts, "Smoothing", "##legit_smooth", state.legit_smooth, 1.0f, 20.0f, "%.1f");
        RowSliderInt(fonts, "Reaction Time", "##legit_reaction", state.legit_reaction_time, 0, 300, "%d ms");
        RowSliderCheck(fonts, "Recoil Control System", "##legit_rcs", state.legit_rcs, state.legit_rcs_scale, 0.0f, 1.0f, "%.1f");
        RowBool(fonts, "Triggerbot", "##legit_trigger", state.legit_triggerbot);
        ImGui::EndTable();
    }
}

void RenderVisuals(State& state, const Fonts& fonts, float left, float width)
{
    static constexpr const char* weapon_modes[] = { "OFF", "TEXT", "ICON", "BOTH" };
    static constexpr const char* chams_types[] = { "OFF", "FLAT", "TEXTURED", "METALLIC" };
    static constexpr const char* local_chams_types[] = { "OFF", "FLAT", "TEXTURED", "WIREFRAME" };

    switch (static_cast<VisualsSection>(state.section))
    {
    case VisualsSection::Enemy:
        SectionTitle(fonts, "Enemy", left);
        if (BeginTable("##vis_enemy", width))
        {
            RowBool(fonts, "Bounding Box", "##vis_enemy_box", state.vis_enemy_box);
            RowBool(fonts, "Player Name", "##vis_enemy_name", state.vis_enemy_name);
            RowBool(fonts, "Health Bar", "##vis_enemy_health", state.vis_enemy_health);
            RowCombo(fonts, "Weapon Display", "##vis_enemy_weapon", state.vis_enemy_weapon, weapon_modes, IM_ARRAYSIZE(weapon_modes));
            RowSliderCheck(fonts, "Outline", "##vis_enemy_outline", state.vis_enemy_outline, state.vis_enemy_outline_width, 0.1f, 2.0f, "%.1f");
            RowBool(fonts, "Skeleton", "##vis_enemy_skeleton", state.vis_enemy_skeleton);
            RowBool(fonts, "Glow", "##vis_enemy_glow", state.vis_enemy_glow);
            RowCombo(fonts, "Chams", "##vis_enemy_chams", state.vis_enemy_chams, chams_types, IM_ARRAYSIZE(chams_types));
            RenderColorRows(fonts, "##enemy_col", state.vis_enemy_color);
            ImGui::EndTable();
        }
        break;
    case VisualsSection::Team:
        SectionTitle(fonts, "Team", left);
        if (BeginTable("##vis_team", width))
        {
            RowBool(fonts, "Enable Team Visuals", "##vis_team_enabled", state.vis_team_enabled);
            RowBool(fonts, "Bounding Box", "##vis_team_box", state.vis_team_box);
            RowBool(fonts, "Player Name", "##vis_team_name", state.vis_team_name);
            RowBool(fonts, "Health Bar", "##vis_team_health", state.vis_team_health);
            RowSliderCheck(fonts, "Outline", "##vis_team_outline", state.vis_team_outline, state.vis_team_outline_width, 0.1f, 2.0f, "%.1f");
            RowCombo(fonts, "Chams", "##vis_team_chams", state.vis_team_chams, chams_types, IM_ARRAYSIZE(chams_types));
            RenderColorRows(fonts, "##team_col", state.vis_team_color);
            ImGui::EndTable();
        }
        break;
    case VisualsSection::Local:
        SectionTitle(fonts, "Local", left);
        if (BeginTable("##vis_local", width))
        {
            RowSliderCheck(fonts, "Third Person", "##vis_thirdperson", state.vis_thirdperson, state.vis_thirdperson_dist, 50.0f, 200.0f, "%.0f");
            RowSlider(fonts, "Viewmodel FOV", "##vis_vm_fov", state.vis_viewmodel_fov, 54.0f, 120.0f, "%.0f deg");
            RowSlider(fonts, "Override FOV", "##vis_fov", state.vis_override_fov, 60.0f, 140.0f, "%.0f deg");
            RowCombo(fonts, "Local Chams", "##vis_local_chams", state.vis_local_chams, local_chams_types, IM_ARRAYSIZE(local_chams_types));
            RenderColorRows(fonts, "##local_col", state.vis_local_color);
            ImGui::EndTable();
        }
        break;
    case VisualsSection::Other:
        SectionTitle(fonts, "Other", left);
        if (BeginTable("##vis_other", width))
        {
            RowBool(fonts, "Dropped Weapons", "##vis_dropped", state.vis_dropped_weapons);
            RowBool(fonts, "Grenade Prediction", "##vis_grenade", state.vis_grenade_prediction);
            RowBool(fonts, "Bomb Timer", "##vis_bomb", state.vis_bomb_timer);
            RowSliderIntCheck(fonts, "Nightmode", "##vis_nightmode", state.vis_nightmode, state.vis_brightness, 10, 100, "%d%%");
            RowBool(fonts, "Remove Flashbang", "##vis_noflash", state.vis_remove_flash);
            RowBool(fonts, "Remove Smoke", "##vis_nosmoke", state.vis_remove_smoke);
            RenderColorRows(fonts, "##world_col", state.vis_world_color);
            ImGui::EndTable();
        }
        break;
    }
}

void RenderMisc(State& state, const Fonts& fonts, float left, float width)
{
    static constexpr const char* strafe_modes[] = { "OFF", "DIRECTIONAL", "VIEW ANGLES" };
    static constexpr const char* hitsounds[] = { "OFF", "BELL", "COD", "BEEP" };

    switch (static_cast<MiscSection>(state.section))
    {
    case MiscSection::Movement:
        SectionTitle(fonts, "Movement", left);
        if (BeginTable("##misc_movement", width))
        {
            RowBool(fonts, "Bunny Hop", "##misc_bhop", state.misc_bhop);
            RowCombo(fonts, "Auto Strafe", "##misc_strafe", state.misc_autostrafe, strafe_modes, IM_ARRAYSIZE(strafe_modes));
            RowBool(fonts, "Fast Stop", "##misc_faststop", state.misc_fast_stop);
            RowBool(fonts, "Infinite Duck", "##misc_infduck", state.misc_infinite_duck);
            RowBool(fonts, "Edge Jump", "##misc_edgejump", state.misc_edge_jump);
            ImGui::EndTable();
        }
        break;
    case MiscSection::Other:
        SectionTitle(fonts, "Other", left);
        if (BeginTable("##misc_other", width))
        {
            RowCombo(fonts, "Hit Sound", "##misc_hitsound", state.misc_hitsound, hitsounds, IM_ARRAYSIZE(hitsounds));
            RowBool(fonts, "Hit Marker", "##misc_hitmarker", state.misc_hitmarker);
            RowBool(fonts, "Spectator List", "##misc_speclist", state.misc_spectator_list);
            RowBool(fonts, "Reveal Ranks", "##misc_ranks", state.misc_reveal_ranks);
            RowBool(fonts, "Clantag Changer", "##misc_clantag", state.misc_clantag);
            ImGui::EndTable();
        }
        break;
    }
}

bool PanoramaButton(const Fonts& fonts, const char* id, const char* label, ImVec2 size)
{
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    if (window->SkipItems)
        return false;

    const ImGuiID item_id = window->GetID(id);
    const ImVec2 pos = window->DC.CursorPos;
    const ImRect frame(pos, Add(pos, size.x, size.y));
    ImGui::ItemSize(frame);
    if (!ImGui::ItemAdd(frame, item_id))
        return false;

    bool hovered = false;
    bool held = false;
    const bool pressed = ImGui::ButtonBehavior(frame, item_id, &hovered, &held);

    ImGuiStorage* storage = ImGui::GetStateStorage();
    const float dt = std::clamp(ImGui::GetIO().DeltaTime, 0.0f, 1.0f / 15.0f);
    const ImGuiID hover_key = ImHashStr("btn_hover", 0, item_id);
    const ImGuiID press_key = ImHashStr("btn_press", 0, item_id);
    const float hover = Smooth(storage->GetFloat(hover_key, 0.0f), hovered ? 1.0f : 0.0f, 18.0f, dt);
    const float press = Smooth(storage->GetFloat(press_key, 0.0f), held ? 1.0f : 0.0f, 24.0f, dt);
    storage->SetFloat(hover_key, hover);
    storage->SetFloat(press_key, press);

    ImDrawList* draw = window->DrawList;
    draw->AddRectFilled(frame.Min, frame.Max,
        ImGui::GetColorU32({ 0.11f, 0.12f, 0.13f, 0.28f + 0.14f * hover + 0.10f * press }), 2.0f);
    if (hover > 0.0f)
        DrawHorizontalFade(draw, frame, 0.22f * hover + 0.10f * press);
    draw->AddRect(frame.Min, frame.Max,
        ImGui::GetColorU32({ 0.88f, 0.90f, 0.92f, 0.16f + 0.28f * hover }), 2.0f);

    const float font_size = S(17.0f);
    const ImVec2 text_size = fonts.stratum_bold->CalcTextSizeA(font_size, FLT_MAX, 0.0f, label);
    const ImVec2 text_pos = {
        std::floor(frame.Min.x + (size.x - text_size.x) * 0.5f + 0.5f),
        std::floor(frame.Min.y + (size.y - text_size.y) * 0.5f + 0.5f)
    };
    draw->AddText(fonts.stratum_bold, font_size, text_pos,
        ImGui::GetColorU32({ 0.88f + 0.09f * hover, 0.89f + 0.09f * hover, 0.91f + 0.08f * hover, 1.0f }),
        label);
    ImGui::RenderNavCursor(frame, item_id);
    return pressed;
}

void RowInputText(const Fonts& fonts, const char* label, const char* id, const char* hint,
    char* buffer, size_t buffer_size)
{
    RowLabel(label);
    ImGui::PushID(id);
    ImGui::PushFont(fonts.stratum_bold, S(17.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, S(12.0f, 10.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 2.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, { 0.11f, 0.12f, 0.13f, 0.28f });
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, { 0.13f, 0.14f, 0.15f, 0.38f });
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, { 0.14f, 0.15f, 0.16f, 0.46f });
    ImGui::PushStyleColor(ImGuiCol_Border, { 0.83f, 0.84f, 0.86f, 0.19f });
    ImGui::PushStyleColor(ImGuiCol_Text, { 0.95f, 0.96f, 0.97f, 1.0f });
    ImGui::InputTextWithHint("##input", hint, buffer, buffer_size);
    ImGui::PopStyleColor(5);
    ImGui::PopStyleVar(3);
    ImGui::PopFont();
    ImGui::PopID();
}

void RenderConfig(State& state, const Fonts& fonts, float left, float width)
{
    state.cfg_count = std::clamp(state.cfg_count, 1, 16);
    state.cfg_slot = std::clamp(state.cfg_slot, 0, state.cfg_count - 1);
    const char* items[16] = {};
    for (int i = 0; i < state.cfg_count; ++i)
        items[i] = state.cfg_names[i];

    SectionTitle(fonts, "Config", left);
    if (BeginTable("##config", width))
    {
        RowCombo(fonts, "Selected Config", "##cfg_slot", state.cfg_slot, items, state.cfg_count);
        RowInputText(fonts, "Config Name", "##cfg_name", "Enter config name...", state.cfg_name, sizeof(state.cfg_name));
        ImGui::EndTable();
    }

    ImGui::Dummy({ 0.0f, S(10.0f) });
    ImGui::SetCursorPosX(left);
    constexpr int btn_count = 5;
    const float btn_gap = S(12.0f);
    const float btn_width = std::floor((width - btn_gap * (btn_count - 1)) / static_cast<float>(btn_count));
    const float btn_height = S(42.0f);

    if (PanoramaButton(fonts, "##cfg_add", "ADD", { btn_width, btn_height }))
    {
        if (state.cfg_count < 16)
        {
            char new_label[64] = {};
            if (state.cfg_name[0] != '\0')
            {
                size_t i = 0;
                for (; i + 1 < sizeof(new_label) && state.cfg_name[i] != '\0'; ++i)
                    new_label[i] = static_cast<char>(std::toupper(static_cast<unsigned char>(state.cfg_name[i])));
                new_label[i] = '\0';
            }
            else
            {
                std::snprintf(new_label, sizeof(new_label), "CONFIG %d", state.cfg_count + 1);
            }
            std::snprintf(state.cfg_names[state.cfg_count], sizeof(state.cfg_names[0]), "%s", new_label);
            state.cfg_slot = state.cfg_count;
            ++state.cfg_count;
            state.cfg_name[0] = '\0';
        }
    }
    ImGui::SameLine(0.0f, btn_gap);
    if (PanoramaButton(fonts, "##cfg_load", "LOAD", { btn_width, btn_height }))
    {
        // Trigger config load for state.cfg_slot
    }
    ImGui::SameLine(0.0f, btn_gap);
    if (PanoramaButton(fonts, "##cfg_save", "SAVE", { btn_width, btn_height }))
    {
        if (state.cfg_name[0] != '\0')
        {
            size_t i = 0;
            for (; i + 1 < sizeof(state.cfg_names[0]) && state.cfg_name[i] != '\0'; ++i)
                state.cfg_names[state.cfg_slot][i] = static_cast<char>(std::toupper(static_cast<unsigned char>(state.cfg_name[i])));
            state.cfg_names[state.cfg_slot][i] = '\0';
            state.cfg_name[0] = '\0';
        }
    }
    ImGui::SameLine(0.0f, btn_gap);
    if (PanoramaButton(fonts, "##cfg_delete", "DELETE", { btn_width, btn_height }))
    {
        if (state.cfg_count > 1)
        {
            for (int i = state.cfg_slot; i + 1 < state.cfg_count; ++i)
                std::memcpy(state.cfg_names[i], state.cfg_names[i + 1], sizeof(state.cfg_names[0]));
            --state.cfg_count;
            state.cfg_slot = std::clamp(state.cfg_slot, 0, state.cfg_count - 1);
        }
    }
    ImGui::SameLine(0.0f, btn_gap);
    if (PanoramaButton(fonts, "##cfg_reset", "RESET", { btn_width, btn_height }))
    {
        const SettingsTab tab = state.tab;
        const int section = state.section;
        const int slot = state.cfg_slot;
        const int count = state.cfg_count;
        char names[16][64];
        std::memcpy(names, state.cfg_names, sizeof(names));
        state = State{};
        state.tab = tab;
        state.section = section;
        state.cfg_slot = slot;
        state.cfg_count = count;
        std::memcpy(state.cfg_names, names, sizeof(names));
    }

    ImGui::Dummy({ 0.0f, S(24.0f) });
    SectionTitle(fonts, "General", left);
    if (BeginTable("##config_general", width))
    {
        RowSlider(fonts, "Menu Scale", "##cfg_scale", state.cfg_menu_scale, 0.75f, 1.25f, "%.2f");
        RowBool(fonts, "Stream Proof", "##cfg_streamproof", state.cfg_stream_proof);
        RowBool(fonts, "Safe Mode", "##cfg_safemode", state.cfg_safe_mode);
        ImGui::EndTable();
    }
}

bool RowButton(const Fonts& fonts, const char* row_label, const char* id, const char* button_label, float width = 180.0f)
{
    RowLabel(row_label);
    return PanoramaButton(fonts, id, button_label, S(width, 38.0f));
}

void RenderPreview(State& state, const Fonts& fonts, float left, float width)
{
    static constexpr const char* combo_modes[] = { "NEAREST", "CROSSHAIR", "LOWEST HP", "HIGHEST DAMAGE" };
    static constexpr const char* combo_presets[] = { "HEAD ONLY", "BODY AIM", "DYNAMIC", "MULTIPOINT" };
    static constexpr const char* combo_qualities[] = { "LOW", "MEDIUM", "HIGH", "ULTRA" };
    static constexpr const char* combo_themes[] = { "PANORAMA BLUR", "FROSTED GLASS", "DARK TRANSLUCENT" };

    switch (static_cast<PreviewSection>(state.section))
    {
    case PreviewSection::Combos:
        SectionTitle(fonts, "Text-Only Combos", left);
        if (BeginTable("##prev_combos_text", width))
        {
            RowBool(fonts, "Text-Only Yes / No Combo", "##prev_c_bt", state.prev_combo_bool_text);
            RowCombo(fonts, "Text-Only Selection Combo", "##prev_c_t1", state.prev_combo_text_1, combo_modes, IM_ARRAYSIZE(combo_modes));
            RowCombo(fonts, "Text-Only Preset Combo", "##prev_c_t2", state.prev_combo_text_2, combo_presets, IM_ARRAYSIZE(combo_presets));
            ImGui::EndTable();
        }
        ImGui::Dummy({ 0.0f, S(22.0f) });
        SectionTitle(fonts, "Filled Box Combos", left);
        if (BeginTable("##prev_combos_filled", width))
        {
            RowBoolFilled(fonts, "Filled Yes / No Combo", "##prev_c_bf", state.prev_combo_bool_filled);
            RowComboFilled(fonts, "Filled Quality Combo", "##prev_c_f1", state.prev_combo_filled_1, combo_qualities, IM_ARRAYSIZE(combo_qualities));
            RowComboFilled(fonts, "Filled Theme Style Combo", "##prev_c_f2", state.prev_combo_filled_2, combo_themes, IM_ARRAYSIZE(combo_themes));
            ImGui::EndTable();
        }
        break;

    case PreviewSection::Sliders:
        SectionTitle(fonts, "Float Sliders", left);
        if (BeginTable("##prev_sliders_float", width))
        {
            RowSlider(fonts, "Field of View (Float)", "##prev_s_f1", state.prev_slider_float_1, 0.0f, 180.0f, "%.1f deg");
            RowSlider(fonts, "Scale Multiplier (Float)", "##prev_s_f2", state.prev_slider_float_2, 0.25f, 3.00f, "%.2fx");
            RowSlider(fonts, "Blend Intensity (Float)", "##prev_s_f3", state.prev_slider_float_3, 0.0f, 100.0f, "%.0f%%");
            ImGui::EndTable();
        }
        ImGui::Dummy({ 0.0f, S(22.0f) });
        SectionTitle(fonts, "Integer Sliders", left);
        if (BeginTable("##prev_sliders_int", width))
        {
            RowSliderInt(fonts, "Hitchance (Integer)", "##prev_s_i1", state.prev_slider_int_1, 0, 100, "%d%%");
            RowSliderInt(fonts, "Minimum Damage (Integer)", "##prev_s_i2", state.prev_slider_int_2, 1, 120, "%d hp");
            RowSliderInt(fonts, "Reaction Delay (Integer)", "##prev_s_i3", state.prev_slider_int_3, 0, 500, "%d ms");
            ImGui::EndTable();
        }
        break;

    case PreviewSection::SliderCheck:
        SectionTitle(fonts, "Slider + Checkbox", left);
        if (BeginTable("##prev_slider_check", width))
        {
            RowSliderCheck(fonts, "Outline Width (Float + Check)", "##prev_sc_1", state.prev_schk_1, state.prev_schk_val_1, 0.1f, 3.0f, "%.1f px");
            RowSliderCheck(fonts, "Jitter Range (Float + Check)", "##prev_sc_2", state.prev_schk_2, state.prev_schk_val_2, 0.0f, 90.0f, "%.0f deg");
            RowSliderIntCheck(fonts, "Damage Override (Int + Check)", "##prev_sc_3", state.prev_schk_3, state.prev_schk_val_3, 1, 100, "%d hp");
            RowSliderIntCheck(fonts, "Fakelag Ticks (Int + Check)", "##prev_sc_4", state.prev_schk_4, state.prev_schk_val_4, 1, 16, "%d ticks");
            ImGui::EndTable();
        }
        break;

    case PreviewSection::Checkboxes:
        SectionTitle(fonts, "Checkboxes", left);
        if (BeginTable("##prev_checkboxes", width))
        {
            RowCheckbox(fonts, "Master Feature Switch", "##prev_chk_1", state.prev_chk_1);
            RowCheckbox(fonts, "Player Bounding Box", "##prev_chk_2", state.prev_chk_2);
            RowCheckbox(fonts, "Dynamic Health Bar", "##prev_chk_3", state.prev_chk_3);
            RowCheckbox(fonts, "Shader Blur Backdrop", "##prev_chk_4", state.prev_chk_4);
            RowCheckbox(fonts, "Stream Proof Overlay", "##prev_chk_5", state.prev_chk_5);
            ImGui::EndTable();
        }
        break;

    case PreviewSection::Colors:
        SectionTitle(fonts, "Primary RGBA Color Picker", left);
        if (BeginTable("##prev_color_1", width))
        {
            RenderColorRows(fonts, "##prev_col1", state.prev_color_1);
            ImGui::EndTable();
        }
        ImGui::Dummy({ 0.0f, S(22.0f) });
        SectionTitle(fonts, "Secondary RGBA Color Picker", left);
        if (BeginTable("##prev_color_2", width))
        {
            RenderColorRows(fonts, "##prev_col2", state.prev_color_2);
            ImGui::EndTable();
        }
        break;

    case PreviewSection::Buttons:
        SectionTitle(fonts, "Buttons & Inputs", left);
        if (BeginTable("##prev_buttons_table", width))
        {
            RowInputText(fonts, "Text Input Field", "##prev_input", "Type any text...", state.prev_input_text, sizeof(state.prev_input_text));
            char click_label[48];
            std::snprintf(click_label, sizeof(click_label), "CLICK ME (%d)", state.prev_btn_clicks);
            if (RowButton(fonts, "Interactive Row Button", "##prev_row_btn", click_label, 200.0f))
                ++state.prev_btn_clicks;
            if (RowButton(fonts, "Reset Preview Values", "##prev_reset_btn", "RESET PREVIEW", 200.0f))
            {
                state.prev_btn_clicks = 0;
                std::snprintf(state.prev_input_text, sizeof(state.prev_input_text), "Panorama UI Showcase");
            }
            ImGui::EndTable();
        }

        ImGui::Dummy({ 0.0f, S(16.0f) });
        SectionTitle(fonts, "Action Button Bar", left);
        ImGui::SetCursorPosX(left);
        constexpr int btn_count = 5;
        const float btn_gap = S(12.0f);
        const float btn_width = std::floor((width - btn_gap * (btn_count - 1)) / static_cast<float>(btn_count));
        const float btn_height = S(42.0f);

        if (PanoramaButton(fonts, "##prev_b_add", "ADD", { btn_width, btn_height }))
            ++state.prev_btn_clicks;
        ImGui::SameLine(0.0f, btn_gap);
        if (PanoramaButton(fonts, "##prev_b_load", "LOAD", { btn_width, btn_height }))
            ++state.prev_btn_clicks;
        ImGui::SameLine(0.0f, btn_gap);
        if (PanoramaButton(fonts, "##prev_b_save", "SAVE", { btn_width, btn_height }))
            ++state.prev_btn_clicks;
        ImGui::SameLine(0.0f, btn_gap);
        if (PanoramaButton(fonts, "##prev_b_del", "DELETE", { btn_width, btn_height }))
            ++state.prev_btn_clicks;
        ImGui::SameLine(0.0f, btn_gap);
        if (PanoramaButton(fonts, "##prev_b_rst", "RESET", { btn_width, btn_height }))
            state.prev_btn_clicks = 0;
        break;
    }
}

void RenderPanoramaScrollbar(ImGuiWindow* child_window, ImVec2 position, ImVec2 size, bool interactive)
{
    if (!child_window || size.y <= 32.0f)
        return;

    const float scroll_max = child_window->ScrollMax.y;
    ImGuiStorage* storage = &child_window->StateStorage;
    const ImGuiID target_key = ImHashStr("pano_scroll_target", 0, child_window->ID);
    const ImGuiID hover_key = ImHashStr("pano_scroll_hover", 0, child_window->ID);
    const ImGuiID active_key = ImHashStr("pano_scroll_active", 0, child_window->ID);
    const ImGuiID vis_key = ImHashStr("pano_scroll_vis", 0, child_window->ID);
    const ImGuiID drag_key = ImHashStr("pano_scroll_drag", 0, child_window->ID);
    const ImGuiID dragging_key = ImHashStr("pano_scroll_dragging", 0, child_window->ID);

    const float dt = std::clamp(ImGui::GetIO().DeltaTime, 0.0f, 1.0f / 15.0f);
    const float vis_target = scroll_max > 0.5f ? 1.0f : 0.0f;
    const float vis = Smooth(storage->GetFloat(vis_key, vis_target), vis_target, 16.0f, dt);
    storage->SetFloat(vis_key, vis);
    if (scroll_max <= 0.5f && vis <= 0.01f)
    {
        storage->SetFloat(target_key, 0.0f);
        return;
    }

    float scroll_target = std::clamp(storage->GetFloat(target_key, child_window->Scroll.y), 0.0f, scroll_max);
    const ImGuiIO& io = ImGui::GetIO();

    if (interactive && scroll_max > 0.0f && io.MouseWheel != 0.0f &&
        !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel))
    {
        const ImRect page_rect(position, Add(position, size.x, size.y));
        if (page_rect.Contains(io.MousePos))
            scroll_target = std::clamp(scroll_target - io.MouseWheel * S(88.0f), 0.0f, scroll_max);
    }

    const float margin_y = S(14.0f);
    const float right_inset = S(14.0f);
    const float hit_width = S(14.0f);
    const float track_height = std::max(S(24.0f), size.y - margin_y * 2.0f);
    const float track_center_x = std::floor(position.x + size.x - right_inset + 0.5f);
    const float track_top_y = std::floor(position.y + margin_y + 0.5f);
    const ImRect hit_rect(
        { track_center_x - hit_width * 0.5f, track_top_y },
        { track_center_x + hit_width * 0.5f, track_top_y + track_height });

    const float content_total = std::max(1.0f, size.y + scroll_max);
    const float thumb_height = std::clamp(std::floor(track_height * (size.y / content_total) + 0.5f),
        S(34.0f), track_height);
    const float scrollable_track = std::max(1.0f, track_height - thumb_height);

    bool hovered = interactive && scroll_max > 0.0f && hit_rect.Contains(io.MousePos) &&
        !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
    bool dragging = storage->GetInt(dragging_key, 0) != 0;

    if (hovered && ImGui::IsMouseClicked(0))
    {
        const float current_ratio = scroll_max > 0.0f ? (child_window->Scroll.y / scroll_max) : 0.0f;
        const float current_thumb_y = track_top_y + scrollable_track * current_ratio;
        dragging = true;
        storage->SetInt(dragging_key, 1);
        if (io.MousePos.y >= current_thumb_y && io.MousePos.y <= current_thumb_y + thumb_height)
        {
            storage->SetFloat(drag_key, io.MousePos.y - current_thumb_y);
        }
        else
        {
            storage->SetFloat(drag_key, thumb_height * 0.5f);
            const float new_ratio = std::clamp((io.MousePos.y - thumb_height * 0.5f - track_top_y) / scrollable_track, 0.0f, 1.0f);
            scroll_target = new_ratio * scroll_max;
            child_window->Scroll.y = scroll_target;
        }
    }

    if (dragging)
    {
        if (!interactive || !ImGui::IsMouseDown(0) || scroll_max <= 0.0f)
        {
            dragging = false;
            storage->SetInt(dragging_key, 0);
        }
        else
        {
            const float drag_offset = storage->GetFloat(drag_key, thumb_height * 0.5f);
            const float new_ratio = std::clamp((io.MousePos.y - drag_offset - track_top_y) / scrollable_track, 0.0f, 1.0f);
            scroll_target = new_ratio * scroll_max;
            child_window->Scroll.y = scroll_target;
        }
    }

    if (!dragging)
        child_window->Scroll.y = Smooth(child_window->Scroll.y, scroll_target, 18.0f, dt);
    storage->SetFloat(target_key, scroll_target);

    const float hover = Smooth(storage->GetFloat(hover_key, 0.0f), (hovered || dragging) ? 1.0f : 0.0f, 18.0f, dt);
    const float active = Smooth(storage->GetFloat(active_key, 0.0f), dragging ? 1.0f : 0.0f, 22.0f, dt);
    storage->SetFloat(hover_key, hover);
    storage->SetFloat(active_key, active);

    const float ratio = scroll_max > 0.0f ? std::clamp(child_window->Scroll.y / scroll_max, 0.0f, 1.0f) : 0.0f;
    const float thumb_y = std::floor(track_top_y + scrollable_track * ratio + 0.5f);
    const float bar_half_w = S(2.5f + 1.0f * std::max(hover, active));

    ImDrawList* draw = ImGui::GetWindowDrawList();
    // Panorama scrollbar track groove
    draw->AddRectFilled(
        { track_center_x - bar_half_w, track_top_y },
        { track_center_x + bar_half_w, track_top_y + track_height },
        ImGui::GetColorU32({ 0.08f, 0.09f, 0.11f, (0.32f + 0.14f * hover) * vis }),
        bar_half_w);
    draw->AddRect(
        { track_center_x - bar_half_w, track_top_y },
        { track_center_x + bar_half_w, track_top_y + track_height },
        ImGui::GetColorU32({ 0.90f, 0.92f, 0.95f, (0.10f + 0.10f * hover) * vis }),
        bar_half_w);

    // Panorama scrollbar thumb
    const float thumb_alpha = (0.58f + 0.28f * hover + 0.14f * active) * vis;
    draw->AddRectFilled(
        { track_center_x - bar_half_w, thumb_y },
        { track_center_x + bar_half_w, thumb_y + thumb_height },
        ImGui::GetColorU32({ 0.92f + 0.06f * hover, 0.93f + 0.06f * hover, 0.95f + 0.05f * hover, thumb_alpha }),
        bar_half_w);
}

void RenderPage(State& state, const Fonts& fonts, ImVec2 position, ImVec2 size, bool interactive)
{
    char id[32];
    std::snprintf(id, sizeof(id), "##page_%d_%d", static_cast<int>(state.tab), state.section);
    ImGui::SetCursorScreenPos(position);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoBackground |
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
        (interactive ? ImGuiWindowFlags_None : ImGuiWindowFlags_NoInputs);
    ImGuiWindow* child_window = nullptr;
    if (ImGui::BeginChild(id, size, ImGuiChildFlags_None, flags))
    {
        child_window = ImGui::GetCurrentWindow();
        const float available = ImGui::GetContentRegionAvail().x;
        const float table_width = std::min(S(830.0f), available - S(70.0f));
        const float left = (available - table_width) * 0.5f;
        ImGui::Dummy({ 0.0f, S(20.0f) });
        ImGui::PushFont(fonts.noto_regular, S(16.0f));
        switch (state.tab)
        {
        case SettingsTab::Ragebot: RenderRagebot(state, fonts, left, table_width); break;
        case SettingsTab::Legitbot: RenderLegitbot(state, fonts, left, table_width); break;
        case SettingsTab::Visuals: RenderVisuals(state, fonts, left, table_width); break;
        case SettingsTab::Misc: RenderMisc(state, fonts, left, table_width); break;
        case SettingsTab::Config: RenderConfig(state, fonts, left, table_width); break;
        case SettingsTab::Preview: RenderPreview(state, fonts, left, table_width); break;
        }
        ImGui::PopFont();
        ImGui::Dummy({ 0.0f, S(22.0f) });
    }
    ImGui::EndChild();
    RenderPanoramaScrollbar(child_window, position, size, interactive);
}
}

void Render(State& state, const Fonts& fonts)
{
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    if (!state.visible || display.x <= 0.0f || display.y <= 0.0f)
        return;

    g_scale = std::clamp(state.cfg_menu_scale, 0.75f, 1.25f);
    static float s_prev_scale = 1.0f;

    ImGui::SetNextWindowPos({ display.x * 0.5f, display.y * 0.5f }, ImGuiCond_FirstUseEver, { 0.5f, 0.5f });
    if (std::abs(g_scale - s_prev_scale) > 0.0005f)
    {
        ImGui::SetNextWindowSize({
            std::min(std::floor(S(1050.0f) + 0.5f), display.x * 0.96f),
            std::min(std::floor(S(650.0f) + 0.5f), display.y * 0.96f)
        }, ImGuiCond_Always);
        s_prev_scale = g_scale;
    }
    else
    {
        ImGui::SetNextWindowSize({
            std::min(S(1050.0f), display.x * 0.82f),
            std::min(S(650.0f), display.y * 0.80f)
        }, ImGuiCond_FirstUseEver);
    }
    ImGui::SetNextWindowSizeConstraints(S(900.0f, 560.0f), { FLT_MAX, FLT_MAX });

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, { 0.0f, 0.0f });
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, S(9.0f, 7.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, S(10.0f, 8.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, S(9.0f, 7.0f));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_TitleBg, IM_COL32(35, 61, 96, 255));
    ImGui::PushStyleColor(ImGuiCol_TitleBgActive, IM_COL32(43, 74, 115, 255));
    ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(85, 85, 88, 110));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, IM_COL32(110, 118, 130, 150));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, IM_COL32(110, 125, 145, 175));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, IM_COL32(220, 225, 235, 36));
    ImGui::PushStyleColor(ImGuiCol_HeaderActive, IM_COL32(220, 225, 235, 60));
    ImGui::PushStyleColor(ImGuiCol_Separator, kDivider);
    ImGui::PushStyleColor(ImGuiCol_TableBorderLight, kDivider);
    ImGui::PushStyleColor(ImGuiCol_Text, kMuted);

    if (ImGui::Begin("Panorama UI by kdt255", &state.visible,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
        ImGuiWindowFlags_NoBackground))
    {
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        const ImVec2 body = ImGui::GetContentRegionAvail();
        ImDrawList* draw = ImGui::GetWindowDrawList();
        const ImVec2 end = Add(origin, body.x, body.y);
        draw->PushClipRect(origin, end, true);
        draw->AddCallback(DrawBlurCallback, nullptr);
        draw->AddCallback(ImDrawCallback_ResetRenderState, nullptr);
        draw->PopClipRect();
        draw->AddRectFilledMultiColor(origin, end,
            IM_COL32(55, 72, 94, 115), IM_COL32(52, 74, 98, 115),
            IM_COL32(72, 58, 48, 125), IM_COL32(44, 44, 50, 125));

        const float header_h = std::floor(S(69.0f) + 0.5f);
        const float tabs_y = header_h + 1.0f;
        const float tabs_h = std::floor(S(52.0f) + 0.5f);
        const float tabs_bottom = tabs_y + tabs_h;
        const float subtabs_y = tabs_bottom + 1.0f;
        const float subtabs_h = std::floor(S(44.0f) + 0.5f);
        const float subtabs_bottom = subtabs_y + subtabs_h;
        const float content_top = subtabs_bottom + 1.0f;

        draw->AddRectFilled(origin, Add(origin, body.x, header_h), IM_COL32(34, 44, 58, 55));
        draw->AddText(fonts.stratum_bold, S(30.0f), Add(origin, S(28.0f), S(20.0f)), kWhite, "Panorama UI by kdt255");
        draw->AddLine(Add(origin, 0.0f, header_h + 0.5f), Add(origin, body.x, header_h + 0.5f), kDivider);

        float x = S(kTabsLeft);
        for (size_t i = 0; i < kTabs.size(); ++i)
        {
            const SettingsTab target_tab = static_cast<SettingsTab>(i);
            const float tab_w = S(kTabs[i].width);
            ImGui::PushID(static_cast<int>(i));
            if (TabButton("##tab", kTabs[i].label, fonts.stratum_regular, Add(origin, x, tabs_y),
                tab_w, tabs_h, state.tab == target_tab, false))
                SelectPage(state, target_tab, state.tab == target_tab ? state.section : 0);
            ImGui::PopID();
            x += tab_w;
        }
        draw->AddLine(Add(origin, 0.0f, tabs_bottom + 0.5f), Add(origin, body.x, tabs_bottom + 0.5f), kDivider);

        const TabSpec* subtabs = nullptr;
        size_t subtab_count = 0;
        switch (state.tab)
        {
        case SettingsTab::Ragebot:
            subtabs = kRagebotSections.data();
            subtab_count = kRagebotSections.size();
            break;
        case SettingsTab::Visuals:
            subtabs = kVisualsSections.data();
            subtab_count = kVisualsSections.size();
            break;
        case SettingsTab::Misc:
            subtabs = kMiscSections.data();
            subtab_count = kMiscSections.size();
            break;
        case SettingsTab::Preview:
            subtabs = kPreviewSections.data();
            subtab_count = kPreviewSections.size();
            break;
        default:
            break;
        }

        if (subtabs != nullptr)
        {
            float section_x = S(kTabsLeft);
            for (size_t i = 0; i < subtab_count; ++i)
            {
                const float sub_w = S(subtabs[i].width);
                ImGui::PushID(static_cast<int>(state.tab) * 100 + static_cast<int>(i) + 100);
                if (TabButton("##section", subtabs[i].label, fonts.stratum_regular,
                    Add(origin, section_x, subtabs_y), sub_w, subtabs_h,
                    state.section == static_cast<int>(i), true))
                    SelectPage(state, state.tab, static_cast<int>(i));
                ImGui::PopID();
                section_x += sub_w;
            }
        }
        draw->AddLine(Add(origin, 0.0f, subtabs_bottom + 0.5f), Add(origin, body.x, subtabs_bottom + 0.5f), kDivider);

        const ImVec2 content_position = Add(origin, 0.0f, content_top);
        const ImVec2 content_size = { body.x, body.y - content_top };
        if (state.page_transition < 1.0f)
        {
            const float dt = std::clamp(ImGui::GetIO().DeltaTime, 0.0f, 1.0f / 15.0f);
            state.page_transition = std::min(1.0f, state.page_transition + dt / 0.30f);
        }
        const float progress = EaseOutCubic(state.page_transition);
        ImGui::PushClipRect(content_position, Add(content_position, content_size.x, content_size.y), true);
        if (state.page_transition < 1.0f)
        {
            State previous = state;
            previous.tab = state.previous_tab;
            previous.section = state.previous_section;
            const float previous_fraction = state.previous_offset +
                (static_cast<float>(state.page_direction) - state.previous_offset) * progress;
            const float previous_pixels = std::floor(previous_fraction * content_size.x + 0.5f);
            RenderPage(previous, fonts, Add(content_position, previous_pixels, 0.0f),
                content_size, false);
        }
        const float current_fraction = -static_cast<float>(state.page_direction) * (1.0f - progress);
        const float current_pixels = std::floor(current_fraction * content_size.x + 0.5f);
        RenderPage(state, fonts, Add(content_position, current_pixels, 0.0f),
            content_size, state.page_transition >= 1.0f);
        ImGui::PopClipRect();
    }
    ImGui::End();
    ImGui::PopStyleColor(12);
    ImGui::PopStyleVar(4);
}
}
