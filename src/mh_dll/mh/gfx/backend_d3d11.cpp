//
// backend_d3d11.cpp -- D3D11 present_backend (PT-GFX3). Scaled draw of the
// game's software-rendered frame with a real GPU pipeline: texture upload, a fullscreen-triangle
// vertex shader, and a pixel shader per scale_filter (point / linear / sharp-bilinear / area). Falls back
// to gdi (present_backend.h's contract: attach() returns false) whenever a usable device can't be
// stood up -- no hardware, no d3d11.dll, sub-10_0 feature level.
//
// NO LINK-TIME DEPENDENCY on d3d11.lib / dxgi.lib / d3dcompiler.lib. d3d11.dll is LoadLibrary'd and
// D3D11CreateDevice is resolved with GetProcAddress -- see create_device() below -- so a machine
// with no D3D11 at all (or a stripped d3d11.dll) still LINKS this DLL fine and only fails at
// attach() time, exactly like the game's own on-demand ddraw.dll loader this presenter sits behind.
// The other half of "no link dependency" is never
// referencing the IID_* named GUID globals (dxguid.lib) -- every QueryInterface/GetParent call below
// uses the compiler-intrinsic __uuidof(T) instead, which needs no import library at all.
//
// FORMAT FALLBACK PATH (upload). The source frame is 16bpp (rgb565 or rgb555, present_backend.h).
// DXGI_FORMAT_B5G6R5_UNORM's bit layout -- by the DXGI packed-format naming convention (channels
// listed low-bit-first) -- is R at bits 15:11, G at 10:5, B at 4:0, which is bit-for-bit the game's
// rgb565 (R 0xF800 / G 0x07E0 / B 0x001F). So when the device supports it, upload is a straight
// row memcpy, no per-pixel work. rgb555 (R 0x7C00 / G 0x03E0 / B 0x001F, top bit unused) is one bit
// short of DXGI_FORMAT_B5G5R5A1_UNORM (same R5G5B5, but bit 15 is alpha) -- when that format is
// supported we still do a full-frame pass, but only to OR in the alpha bit (0x8000) per pixel, never
// a real expand. Only when NEITHER 16bpp format is supported (checked via CheckFormatSupport, and
// reconfirmed by a fallback retry if CreateTexture2D itself rejects a format the query approved --
// seen on some driver/WARP combinations) do we CPU-convert to B8G8R8A8_UNORM, one small stack row
// buffer at a time (no heap allocation on the present() path). Either 16bpp format additionally
// requires the device to have been created with D3D11_CREATE_DEVICE_BGRA_SUPPORT -- undocumented
// outside the "Format Support for Direct3D Feature Level Hardware" MSDN table, but real: without
// that flag every feature-level-11.0 driver still refuses B5G6R5/B5G5R5A1 as a sampleable texture.
//
// WARP DECISION: ACCEPTED, logged distinctly. create_device() tries D3D_DRIVER_TYPE_HARDWARE first
// and falls back to D3D_DRIVER_TYPE_WARP (Microsoft's software rasterizer, built into d3d11.dll,
// no extra DLL to load) if that fails, accepting either as long as the achieved feature level is
// >= 10_0. WARP is a fully spec-conformant D3D11 device -- every format check, shader and swap-chain
// path below runs identically on it, just slower -- so a GPU-less rig VM still gets correct scaling
// and filtering through the SAME code PT-GFX4's overlay will later draw into, rather than silently
// losing the d3d11 backend to gdi. The one place this matters operationally is frame pacing (a WARP
// present can take longer than a real GPU's), which is exactly why "driver=WARP" gets its own log
// line -- a pacing complaint on a VM is then diagnosable at a glance instead of looking like a bug.
//
// SWAP CHAIN: flip-model (DXGI_SWAP_EFFECT_FLIP_DISCARD, via IDXGIFactory2::CreateSwapChainForHwnd)
// is tried first; if IDXGIFactory2 is unavailable or that call fails, create_swapchain() falls back
// to the legacy IDXGIFactory::CreateSwapChain with DXGI_SWAP_EFFECT_DISCARD. The swap chain is
// always resized to exactly match the window's client rect (handle_client_resize(), called from
// both on_window_changed() and defensively from present() for a resize the caller didn't announce);
// our own aspect-preserving letterbox is done with a GPU viewport, never with DXGI's own
// stretch-to-fit, so it only ever affects the game-content sub-rect and not the letterbox bars.
// DXGI_ERROR_DEVICE_REMOVED / _RESET (from Present or ResizeBuffers) tears everything down and sets
// device_lost_; the NEXT present() call re-attaches from scratch (attach() is itself idempotent --
// it detaches first) rather than trying to selectively repair a half-dead device.
//
// WINDOW STYLE: this backend does NOT touch WS_* styles for window_mode::borderless -- per
// present_backend.h, "the window itself is sized by the game... or by the owner". RECOMMENDATION for
// whoever wires window_mode::borderless up: apply WS_POPUP (no caption/border) sized to the target
// monitor BEFORE calling attach()/on_window_changed(), the same way the game's own exclusive-
// fullscreen ddraw path never had to fight a bordered HWND. This backend only ever reads the current
// client rect and renders into it, so it needs no signal about *why* the rect is what it is.
//
// PT-GFX4 HOOK POINT (wired): the Dear ImGui overlay (gfx/overlay_imgui.cpp) runs between the
// game-frame Draw(3, 0) and swap_chain_->Present() inside present() below -- search for "PT-GFX4 HOOK
// POINT". The render target is already bound there, so imgui_impl_dx11's own draw call layers
// straight on top with no extra Present. release_device_resources() tells the overlay first, so ImGui
// never holds a reference on a device this backend is tearing down. The overlay is inert unless
// [video] imgui=1 armed it; no ImGui code lives in this file.
//
// -------------------------------------------------------------------------------------------------
// HLSL SOURCE (compiled offline -- see the byte arrays below; this project does not link
// d3dcompiler.lib, so there is no runtime-compile path and none is needed: three tiny, static
// shaders). Regenerate with the Windows SDK's fxc.exe (any recent SDK; used here:
// "C:\Program Files (x86)\Windows Kits\10\bin\10.0.22621.0\x86\fxc.exe") and paste the three
// "/Fh"-emitted BYTE arrays back into this file:
//
//   fxc /nologo /T vs_4_0 /E main /Fh fs_triangle_vs.h  /Vn g_fs_triangle_vs     fs_triangle_vs.hlsl
//   fxc /nologo /T ps_4_0 /E main /Fh sample_ps.h        /Vn g_sample_ps         sample_ps.hlsl
//   fxc /nologo /T ps_4_0 /E main /Fh sharp_bilinear_ps.h /Vn g_sharp_bilinear_ps sharp_bilinear_ps.hlsl
//
// ---- fs_triangle_vs.hlsl (vs_4_0) ----------------------------------------------------------------
//
//   // Fullscreen triangle, no vertex/index buffer (SV_VertexID trick). id 0,1,2 -> uv (0,0) (2,0)
//   // (0,2); the clip-space position covers the whole viewport, the extra triangle area is clipped
//   // by the rasterizer. D3D texcoord origin is top-left, matching how the RGB565 frame is uploaded
//   // (row 0 = top row), so no V-flip is needed here.
//   struct vs_out {
//       float4 pos : SV_Position;
//       float2 uv  : TEXCOORD0;
//   };
//
//   vs_out main(uint id : SV_VertexID) {
//       vs_out o;
//       float2 uv = float2((id << 1) & 2, id & 2);
//       o.pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
//       o.uv  = uv;
//       return o;
//   }
//
// ---- sample_ps.hlsl (ps_4_0) -- used for BOTH point and linear filters -------------------------
//
//   // The filter is a property of the D3D11_SAMPLER_DESC bound at draw time
//   // (D3D11_FILTER_MIN_MAG_MIP_POINT vs _LINEAR), not of the shader, so one pixel shader covers
//   // both scale_filter::point and scale_filter::linear.
//   Texture2D    tex0  : register(t0);
//   SamplerState samp0 : register(s0);
//
//   float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
//       return tex0.Sample(samp0, uv);
//   }
//
// ---- sharp_bilinear_ps.hlsl (ps_4_0) -- scale_filter::sharp -------------------------------------
//
//   // ALSO scale_filter::area: the same shader with texel_scale = the TRUE (non-integer) scale. The
//   // ramp near a texel edge is then (p - edge) * scale + 0.5 -- 0 half an output pixel before the
//   // edge, 1 half a pixel after it -- which is exactly the box filter's coverage of the next texel
//   // by one output pixel's 1/scale-texel footprint: a 1-output-pixel antialiased edge at any scale.
//   //
//   // "Integer prescale then linear": crisp upscaling that still blends across the (rare,
//   // non-integer-ratio) fractional remainder, instead of true nearest-neighbour's uneven texel
//   // widths. This is the well-known public-domain "sharp-bilinear-simple" algorithm (libretro
//   // shader-pack lineage): widen the linear-filtering region to a per-axis integer count of texels
//   // around each output pixel's center, and only blend inside a small edge band around each source
//   // texel's own border, so the interior of a source texel reads perfectly flat. At texel_scale ==
//   // 1 the region_range term is exactly 0 and the formula degenerates to plain bilinear.
//   cbuffer scale_params : register(b0) {
//       float2 src_size;    // source texture size in texels
//       float2 texel_scale; // floor(viewport_size / src_size) per axis, clamped to >= 1
//   };
//
//   Texture2D    tex0  : register(t0);
//   SamplerState samp0 : register(s0); // must be a LINEAR sampler
//
//   float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
//       float2 texel        = uv * src_size;
//       float2 texel_floor  = floor(texel);
//       float2 s             = frac(texel);
//       float2 region_range = 0.5 - 0.5 / texel_scale;
//       float2 center_dist  = s - 0.5;
//       float2 f = (center_dist - clamp(center_dist, -region_range, region_range)) * texel_scale + 0.5;
//       float2 mod_texel = texel_floor + f;
//       return tex0.Sample(samp0, mod_texel / src_size);
//   }
//
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdint.h>
#include <string.h>

#include <d3d11.h>
#include <dxgi1_2.h>
#include <dxgi1_5.h> // IDXGIFactory5::CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING)

#include "present_backend.h"
#include "overlay_imgui.h"          // PT-GFX4: on_frame / on_device_release
#include "include/mh_run_context.h" // MH_ProcessDir -- shared mh_video.log location (video.cpp)

#pragma comment(lib, "user32.lib") // wsprintfA / wvsprintfA / lstrlenA / GetClientRect / IsIconic

namespace mh::gfx {

namespace {

constexpr int kMaxRowPixels = 4096; // headroom well above any resolution this engine's view metrics
                                    // support (video.cpp's relocate_view_arrays tops out far below
                                    // this); lets upload_frame() convert a row through a stack
                                    // buffer instead of a heap allocation on the present() path.

// ---- compiled shader bytecode (fxc, see the header comment above for the HLSL + regen command) ---

// clang-format off
const BYTE g_fs_triangle_vs[] =
{
     68,  88,  66,  67,  73, 225,
     43,  52, 175,   5, 125, 216,
    191,  37, 177,  63, 234,  30,
     44, 144,   1,   0,   0,   0,
    168,   2,   0,   0,   5,   0,
      0,   0,  52,   0,   0,   0,
    128,   0,   0,   0, 180,   0,
      0,   0,  12,   1,   0,   0,
     44,   2,   0,   0,  82,  68,
     69,  70,  68,   0,   0,   0,
      0,   0,   0,   0,   0,   0,
      0,   0,   0,   0,   0,   0,
     28,   0,   0,   0,   0,   4,
    254, 255,   0,   1,   0,   0,
     28,   0,   0,   0,  77, 105,
     99, 114, 111, 115, 111, 102,
    116,  32,  40,  82,  41,  32,
     72,  76,  83,  76,  32,  83,
    104,  97, 100, 101, 114,  32,
     67, 111, 109, 112, 105, 108,
    101, 114,  32,  49,  48,  46,
     49,   0,  73,  83,  71,  78,
     44,   0,   0,   0,   1,   0,
      0,   0,   8,   0,   0,   0,
     32,   0,   0,   0,   0,   0,
      0,   0,   6,   0,   0,   0,
      1,   0,   0,   0,   0,   0,
      0,   0,   1,   1,   0,   0,
     83,  86,  95,  86, 101, 114,
    116, 101, 120,  73,  68,   0,
     79,  83,  71,  78,  80,   0,
      0,   0,   2,   0,   0,   0,
      8,   0,   0,   0,  56,   0,
      0,   0,   0,   0,   0,   0,
      1,   0,   0,   0,   3,   0,
      0,   0,   0,   0,   0,   0,
     15,   0,   0,   0,  68,   0,
      0,   0,   0,   0,   0,   0,
      0,   0,   0,   0,   3,   0,
      0,   0,   1,   0,   0,   0,
      3,  12,   0,   0,  83,  86,
     95,  80, 111, 115, 105, 116,
    105, 111, 110,   0,  84,  69,
     88,  67,  79,  79,  82,  68,
      0, 171, 171, 171,  83,  72,
     68,  82,  24,   1,   0,   0,
     64,   0,   1,   0,  70,   0,
      0,   0,  96,   0,   0,   4,
     18,  16,  16,   0,   0,   0,
      0,   0,   6,   0,   0,   0,
    103,   0,   0,   4, 242,  32,
     16,   0,   0,   0,   0,   0,
      1,   0,   0,   0, 101,   0,
      0,   3,  50,  32,  16,   0,
      1,   0,   0,   0, 104,   0,
      0,   2,   1,   0,   0,   0,
     41,   0,   0,   7,  18,   0,
     16,   0,   0,   0,   0,   0,
     10,  16,  16,   0,   0,   0,
      0,   0,   1,  64,   0,   0,
      1,   0,   0,   0,   1,   0,
      0,   7,  18,   0,  16,   0,
      0,   0,   0,   0,  10,   0,
     16,   0,   0,   0,   0,   0,
      1,  64,   0,   0,   2,   0,
      0,   0,   1,   0,   0,   7,
     66,   0,  16,   0,   0,   0,
      0,   0,  10,  16,  16,   0,
      0,   0,   0,   0,   1,  64,
      0,   0,   2,   0,   0,   0,
     86,   0,   0,   5,  50,   0,
     16,   0,   0,   0,   0,   0,
    134,   0,  16,   0,   0,   0,
      0,   0,  50,   0,   0,  15,
     50,  32,  16,   0,   0,   0,
      0,   0,  70,   0,  16,   0,
      0,   0,   0,   0,   2,  64,
      0,   0,   0,   0,   0,  64,
      0,   0,   0, 192,   0,   0,
      0,   0,   0,   0,   0,   0,
      2,  64,   0,   0,   0,   0,
    128, 191,   0,   0, 128,  63,
      0,   0,   0,   0,   0,   0,
      0,   0,  54,   0,   0,   5,
     50,  32,  16,   0,   1,   0,
      0,   0,  70,   0,  16,   0,
      0,   0,   0,   0,  54,   0,
      0,   8, 194,  32,  16,   0,
      0,   0,   0,   0,   2,  64,
      0,   0,   0,   0,   0,   0,
      0,   0,   0,   0,   0,   0,
      0,   0,   0,   0, 128,  63,
     62,   0,   0,   1,  83,  84,
     65,  84, 116,   0,   0,   0,
      8,   0,   0,   0,   1,   0,
      0,   0,   0,   0,   0,   0,
      3,   0,   0,   0,   1,   0,
      0,   0,   1,   0,   0,   0,
      2,   0,   0,   0,   1,   0,
      0,   0,   0,   0,   0,   0,
      0,   0,   0,   0,   0,   0,
      0,   0,   0,   0,   0,   0,
      0,   0,   0,   0,   0,   0,
      0,   0,   0,   0,   0,   0,
      0,   0,   0,   0,   0,   0,
      0,   0,   0,   0,   0,   0,
      0,   0,   0,   0,   2,   0,
      0,   0,   0,   0,   0,   0,
      1,   0,   0,   0,   0,   0,
      0,   0,   0,   0,   0,   0,
      0,   0,   0,   0,   0,   0,
      0,   0,   0,   0,   0,   0,
      0,   0,   0,   0,   0,   0,
      0,   0
};

const BYTE g_sample_ps[] =
{
     68,  88,  66,  67, 129,  48,
    113, 204, 220, 146, 197,  20,
     45, 177, 233,  58, 201, 225,
     35, 123,   1,   0,   0,   0,
     64,   2,   0,   0,   5,   0,
      0,   0,  52,   0,   0,   0,
    204,   0,   0,   0,  36,   1,
      0,   0,  88,   1,   0,   0,
    196,   1,   0,   0,  82,  68,
     69,  70, 144,   0,   0,   0,
      0,   0,   0,   0,   0,   0,
      0,   0,   2,   0,   0,   0,
     28,   0,   0,   0,   0,   4,
    255, 255,   0,   1,   0,   0,
    103,   0,   0,   0,  92,   0,
      0,   0,   3,   0,   0,   0,
      0,   0,   0,   0,   0,   0,
      0,   0,   0,   0,   0,   0,
      0,   0,   0,   0,   1,   0,
      0,   0,   1,   0,   0,   0,
     98,   0,   0,   0,   2,   0,
      0,   0,   5,   0,   0,   0,
      4,   0,   0,   0, 255, 255,
    255, 255,   0,   0,   0,   0,
      1,   0,   0,   0,  13,   0,
      0,   0, 115,  97, 109, 112,
     48,   0, 116, 101, 120,  48,
      0,  77, 105,  99, 114, 111,
    115, 111, 102, 116,  32,  40,
     82,  41,  32,  72,  76,  83,
     76,  32,  83, 104,  97, 100,
    101, 114,  32,  67, 111, 109,
    112, 105, 108, 101, 114,  32,
     49,  48,  46,  49,   0, 171,
     73,  83,  71,  78,  80,   0,
      0,   0,   2,   0,   0,   0,
      8,   0,   0,   0,  56,   0,
      0,   0,   0,   0,   0,   0,
      1,   0,   0,   0,   3,   0,
      0,   0,   0,   0,   0,   0,
     15,   0,   0,   0,  68,   0,
      0,   0,   0,   0,   0,   0,
      0,   0,   0,   0,   3,   0,
      0,   0,   1,   0,   0,   0,
      3,   3,   0,   0,  83,  86,
     95,  80, 111, 115, 105, 116,
    105, 111, 110,   0,  84,  69,
     88,  67,  79,  79,  82,  68,
      0, 171, 171, 171,  79,  83,
     71,  78,  44,   0,   0,   0,
      1,   0,   0,   0,   8,   0,
      0,   0,  32,   0,   0,   0,
      0,   0,   0,   0,   0,   0,
      0,   0,   3,   0,   0,   0,
      0,   0,   0,   0,  15,   0,
      0,   0,  83,  86,  95,  84,
     97, 114, 103, 101, 116,   0,
    171, 171,  83,  72,  68,  82,
    100,   0,   0,   0,  64,   0,
      0,   0,  25,   0,   0,   0,
     90,   0,   0,   3,   0,  96,
     16,   0,   0,   0,   0,   0,
     88,  24,   0,   4,   0, 112,
     16,   0,   0,   0,   0,   0,
     85,  85,   0,   0,  98,  16,
      0,   3,  50,  16,  16,   0,
      1,   0,   0,   0, 101,   0,
      0,   3, 242,  32,  16,   0,
      0,   0,   0,   0,  69,   0,
      0,   9, 242,  32,  16,   0,
      0,   0,   0,   0,  70,  16,
     16,   0,   1,   0,   0,   0,
     70, 126,  16,   0,   0,   0,
      0,   0,   0,  96,  16,   0,
      0,   0,   0,   0,  62,   0,
      0,   1,  83,  84,  65,  84,
    116,   0,   0,   0,   2,   0,
      0,   0,   0,   0,   0,   0,
      0,   0,   0,   0,   2,   0,
      0,   0,   0,   0,   0,   0,
      0,   0,   0,   0,   0,   0,
      0,   0,   1,   0,   0,   0,
      0,   0,   0,   0,   0,   0,
      0,   0,   0,   0,   0,   0,
      0,   0,   0,   0,   0,   0,
      0,   0,   0,   0,   0,   0,
      1,   0,   0,   0,   0,   0,
      0,   0,   0,   0,   0,   0,
      0,   0,   0,   0,   0,   0,
      0,   0,   0,   0,   0,   0,
      0,   0,   0,   0,   0,   0,
      0,   0,   0,   0,   0,   0,
      0,   0,   0,   0,   0,   0,
      0,   0,   0,   0,   0,   0,
      0,   0,   0,   0,   0,   0,
      0,   0,   0,   0,   0,   0
};

const BYTE g_sharp_bilinear_ps[] =
{
     68,  88,  66,  67, 227, 241,
      3,  97,  95,  25, 143, 108,
    120, 170, 120, 185, 153, 108,
    211,  43,   1,   0,   0,   0,
    136,   4,   0,   0,   5,   0,
      0,   0,  52,   0,   0,   0,
    104,   1,   0,   0, 192,   1,
      0,   0, 244,   1,   0,   0,
     12,   4,   0,   0,  82,  68,
     69,  70,  44,   1,   0,   0,
      1,   0,   0,   0, 148,   0,
      0,   0,   3,   0,   0,   0,
     28,   0,   0,   0,   0,   4,
    255, 255,   0,   1,   0,   0,
      4,   1,   0,   0, 124,   0,
      0,   0,   3,   0,   0,   0,
      0,   0,   0,   0,   0,   0,
      0,   0,   0,   0,   0,   0,
      0,   0,   0,   0,   1,   0,
      0,   0,   1,   0,   0,   0,
    130,   0,   0,   0,   2,   0,
      0,   0,   5,   0,   0,   0,
      4,   0,   0,   0, 255, 255,
    255, 255,   0,   0,   0,   0,
      1,   0,   0,   0,  13,   0,
      0,   0, 135,   0,   0,   0,
      0,   0,   0,   0,   0,   0,
      0,   0,   0,   0,   0,   0,
      0,   0,   0,   0,   0,   0,
      0,   0,   1,   0,   0,   0,
      1,   0,   0,   0, 115,  97,
    109, 112,  48,   0, 116, 101,
    120,  48,   0, 115,  99,  97,
    108, 101,  95, 112,  97, 114,
     97, 109, 115,   0, 135,   0,
      0,   0,   2,   0,   0,   0,
    172,   0,   0,   0,  16,   0,
      0,   0,   0,   0,   0,   0,
      0,   0,   0,   0, 220,   0,
      0,   0,   0,   0,   0,   0,
      8,   0,   0,   0,   2,   0,
      0,   0, 232,   0,   0,   0,
      0,   0,   0,   0, 248,   0,
      0,   0,   8,   0,   0,   0,
      8,   0,   0,   0,   2,   0,
      0,   0, 232,   0,   0,   0,
      0,   0,   0,   0, 115, 114,
     99,  95, 115, 105, 122, 101,
      0, 171, 171, 171,   1,   0,
      3,   0,   1,   0,   2,   0,
      0,   0,   0,   0,   0,   0,
      0,   0, 116, 101, 120, 101,
    108,  95, 115,  99,  97, 108,
    101,   0,  77, 105,  99, 114,
    111, 115, 111, 102, 116,  32,
     40,  82,  41,  32,  72,  76,
     83,  76,  32,  83, 104,  97,
    100, 101, 114,  32,  67, 111,
    109, 112, 105, 108, 101, 114,
     32,  49,  48,  46,  49,   0,
     73,  83,  71,  78,  80,   0,
      0,   0,   2,   0,   0,   0,
      8,   0,   0,   0,  56,   0,
      0,   0,   0,   0,   0,   0,
      1,   0,   0,   0,   3,   0,
      0,   0,   0,   0,   0,   0,
     15,   0,   0,   0,  68,   0,
      0,   0,   0,   0,   0,   0,
      0,   0,   0,   0,   3,   0,
      0,   0,   1,   0,   0,   0,
      3,   3,   0,   0,  83,  86,
     95,  80, 111, 115, 105, 116,
    105, 111, 110,   0,  84,  69,
     88,  67,  79,  79,  82,  68,
      0, 171, 171, 171,  79,  83,
     71,  78,  44,   0,   0,   0,
      1,   0,   0,   0,   8,   0,
      0,   0,  32,   0,   0,   0,
      0,   0,   0,   0,   0,   0,
      0,   0,   3,   0,   0,   0,
      0,   0,   0,   0,  15,   0,
      0,   0,  83,  86,  95,  84,
     97, 114, 103, 101, 116,   0,
    171, 171,  83,  72,  68,  82,
     16,   2,   0,   0,  64,   0,
      0,   0, 132,   0,   0,   0,
     89,   0,   0,   4,  70, 142,
     32,   0,   0,   0,   0,   0,
      1,   0,   0,   0,  90,   0,
      0,   3,   0,  96,  16,   0,
      0,   0,   0,   0,  88,  24,
      0,   4,   0, 112,  16,   0,
      0,   0,   0,   0,  85,  85,
      0,   0,  98,  16,   0,   3,
     50,  16,  16,   0,   1,   0,
      0,   0, 101,   0,   0,   3,
    242,  32,  16,   0,   0,   0,
      0,   0, 104,   0,   0,   2,
      2,   0,   0,   0,  14,   0,
      0,  11,  50,   0,  16,   0,
      0,   0,   0,   0,   2,  64,
      0,   0,   0,   0,   0,  63,
      0,   0,   0,  63,   0,   0,
      0,   0,   0,   0,   0,   0,
    230, 138,  32,   0,   0,   0,
      0,   0,   0,   0,   0,   0,
      0,   0,   0,  11,  50,   0,
     16,   0,   0,   0,   0,   0,
     70,   0,  16, 128,  65,   0,
      0,   0,   0,   0,   0,   0,
      2,  64,   0,   0,   0,   0,
      0,  63,   0,   0,   0,  63,
      0,   0,   0,   0,   0,   0,
      0,   0,  56,   0,   0,   8,
    194,   0,  16,   0,   0,   0,
      0,   0,   6,  20,  16,   0,
      1,   0,   0,   0,   6, 132,
     32,   0,   0,   0,   0,   0,
      0,   0,   0,   0,  26,   0,
      0,   5,  50,   0,  16,   0,
      1,   0,   0,   0, 230,  10,
     16,   0,   0,   0,   0,   0,
     65,   0,   0,   5, 194,   0,
     16,   0,   0,   0,   0,   0,
    166,  14,  16,   0,   0,   0,
      0,   0,   0,   0,   0,  10,
     50,   0,  16,   0,   1,   0,
      0,   0,  70,   0,  16,   0,
      1,   0,   0,   0,   2,  64,
      0,   0,   0,   0,   0, 191,
      0,   0,   0, 191,   0,   0,
      0,   0,   0,   0,   0,   0,
     52,   0,   0,   8, 194,   0,
     16,   0,   1,   0,   0,   0,
      6,   4,  16, 128,  65,   0,
      0,   0,   0,   0,   0,   0,
      6,   4,  16,   0,   1,   0,
      0,   0,  51,   0,   0,   7,
     50,   0,  16,   0,   0,   0,
      0,   0,  70,   0,  16,   0,
      0,   0,   0,   0, 230,  10,
     16,   0,   1,   0,   0,   0,
      0,   0,   0,   8,  50,   0,
     16,   0,   0,   0,   0,   0,
     70,   0,  16, 128,  65,   0,
      0,   0,   0,   0,   0,   0,
     70,   0,  16,   0,   1,   0,
      0,   0,  50,   0,   0,  10,
     50,   0,  16,   0,   0,   0,
      0,   0,  70,   0,  16,   0,
      0,   0,   0,   0, 230, 138,
     32,   0,   0,   0,   0,   0,
      0,   0,   0,   0, 230,  10,
     16,   0,   0,   0,   0,   0,
      0,   0,   0,  10,  50,   0,
     16,   0,   0,   0,   0,   0,
     70,   0,  16,   0,   0,   0,
      0,   0,   2,  64,   0,   0,
      0,   0,   0,  63,   0,   0,
      0,  63,   0,   0,   0,   0,
      0,   0,   0,   0,  14,   0,
      0,   8,  50,   0,  16,   0,
      0,   0,   0,   0,  70,   0,
     16,   0,   0,   0,   0,   0,
     70, 128,  32,   0,   0,   0,
      0,   0,   0,   0,   0,   0,
     69,   0,   0,   9, 242,  32,
     16,   0,   0,   0,   0,   0,
     70,   0,  16,   0,   0,   0,
      0,   0,  70, 126,  16,   0,
      0,   0,   0,   0,   0,  96,
     16,   0,   0,   0,   0,   0,
     62,   0,   0,   1,  83,  84,
     65,  84, 116,   0,   0,   0,
     14,   0,   0,   0,   2,   0,
      0,   0,   0,   0,   0,   0,
      2,   0,   0,   0,  12,   0,
      0,   0,   0,   0,   0,   0,
      0,   0,   0,   0,   1,   0,
      0,   0,   0,   0,   0,   0,
      0,   0,   0,   0,   0,   0,
      0,   0,   0,   0,   0,   0,
      0,   0,   0,   0,   0,   0,
      0,   0,   1,   0,   0,   0,
      0,   0,   0,   0,   0,   0,
      0,   0,   0,   0,   0,   0,
      0,   0,   0,   0,   0,   0,
      0,   0,   0,   0,   0,   0,
      2,   0,   0,   0,   0,   0,
      0,   0,   0,   0,   0,   0,
      0,   0,   0,   0,   0,   0,
      0,   0,   0,   0,   0,   0,
      0,   0,   0,   0,   0,   0,
      0,   0
};
// clang-format on

// 16 bytes, matches the HLSL cbuffer scale_params layout exactly (two float2 = 4 floats, already a
// multiple of 16, no padding needed).
struct scale_params_cb {
    float src_w, src_h;
    float texel_scale_x, texel_scale_y;
};

typedef HRESULT(WINAPI *PFN_D3D11CreateDevice)(IDXGIAdapter *pAdapter, D3D_DRIVER_TYPE DriverType, HMODULE Software,
                                               UINT Flags, const D3D_FEATURE_LEVEL *pFeatureLevels,
                                               UINT FeatureLevels, UINT SDKVersion, ID3D11Device **ppDevice,
                                               D3D_FEATURE_LEVEL    *pFeatureLevel,
                                               ID3D11DeviceContext **ppImmediateContext);

} // namespace

class d3d11_backend final : public present_backend {
public:
    d3d11_backend() = default;
    ~d3d11_backend() override { detach(); }

    const char *name() const override { return "d3d11"; }

    bool attach(HWND hwnd, const backend_config &cfg) override;
    void on_mode(int width, int height, pixel_format format) override;
    bool present(const frame_view &frame) override;
    void on_window_changed() override;
    void set_config(const backend_config &cfg) override { // all three are read per present()
        cfg_.filter        = cfg.filter;
        cfg_.vsync         = cfg.vsync;
        cfg_.integer_scale = cfg.integer_scale;
    }
    void detach() override;

    // PT-GFX4 hook accessors -- see the file header comment. Valid only between a successful
    // attach() and detach(); null otherwise (device_lost_ included -- an overlay reading these
    // mid-recovery should just skip a frame, same as this backend does).
    ID3D11Device        *device() const { return device_; }
    ID3D11DeviceContext *context() const { return context_; }

private:
    bool        create_device();
    bool        create_swapchain(int w, int h);
    bool        create_pipeline();
    bool        ensure_backbuffer_views();
    void        release_backbuffer_views();
    bool        ensure_source_texture(const frame_view &frame);
    void        upload_frame(const frame_view &frame);
    void        update_viewport_and_cbuffer(const frame_view &frame);
    void        service_readback();
    bool        handle_client_resize();
    void        release_device_resources();
    static void log(const char *fmt, ...);

    HWND           hwnd_ = nullptr;
    backend_config cfg_{};
    bool           device_lost_ = false;

    HMODULE           d3d11_dll_      = nullptr;
    bool              using_warp_     = false;
    D3D_FEATURE_LEVEL achieved_level_ = (D3D_FEATURE_LEVEL)0;

    ID3D11Device        *device_     = nullptr;
    ID3D11DeviceContext *context_    = nullptr;
    IDXGISwapChain      *swap_chain_ = nullptr; // IDXGISwapChain1* stored through the base interface
    bool                 flip_model_ = false;
    bool                 tearing_    = false; // swap chain created with ALLOW_TEARING (vsync=0 is then uncapped)

    ID3D11RenderTargetView *rtv_           = nullptr;
    int                     back_buffer_w_ = 0;
    int                     back_buffer_h_ = 0;

    ID3D11VertexShader *vs_fullscreen_triangle_ = nullptr;
    ID3D11PixelShader  *ps_sample_              = nullptr; // point AND linear (sampler picks the filter)
    ID3D11PixelShader  *ps_sharp_bilinear_      = nullptr;
    ID3D11SamplerState *sampler_point_          = nullptr;
    ID3D11SamplerState *sampler_linear_         = nullptr;
    ID3D11Buffer       *cbuffer_scale_          = nullptr;

    ID3D11Texture2D          *src_tex_              = nullptr;
    ID3D11ShaderResourceView *src_srv_              = nullptr;
    int                       src_w_                = 0;
    int                       src_h_                = 0;
    pixel_format              src_fmt_              = pixel_format::rgb565;
    DXGI_FORMAT               src_dxgi_fmt_         = DXGI_FORMAT_UNKNOWN;
    bool                      src_needs_conversion_ = false; // false = direct 16bpp upload path
};

// =====================================================================================================
// Device + swap chain setup
// =====================================================================================================

bool d3d11_backend::create_device() {
    d3d11_dll_ = LoadLibraryW(L"d3d11.dll"); // mh-str-ok: DLL name
    if (!d3d11_dll_) {
        log("create_device: d3d11.dll not present on this machine");
        return false;
    }
    auto pfn_create = reinterpret_cast<PFN_D3D11CreateDevice>(GetProcAddress(d3d11_dll_, "D3D11CreateDevice"));
    if (!pfn_create) {
        log("create_device: D3D11CreateDevice export missing from d3d11.dll");
        return false;
    }

    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
    // BGRA_SUPPORT is required for DXGI_FORMAT_B5G6R5_UNORM / B5G5R5A1_UNORM to be usable as a
    // sampleable texture at all -- see the file header's format-fallback note -- so it is requested
    // unconditionally rather than only when the direct-upload path is later chosen.
    const UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;

    struct attempt {
        D3D_DRIVER_TYPE type;
        const char     *label;
    };
    const attempt attempts[] = {
        {D3D_DRIVER_TYPE_HARDWARE, "hardware"},
        {D3D_DRIVER_TYPE_WARP, "WARP (software rasterizer)"},
    };

    for (const attempt &a : attempts) {
        D3D_FEATURE_LEVEL achieved = (D3D_FEATURE_LEVEL)0;
        HRESULT           hr       = pfn_create(nullptr, a.type, nullptr, flags, levels, ARRAYSIZE(levels), D3D11_SDK_VERSION,
                                                &device_, &achieved, &context_);
        if (SUCCEEDED(hr) && achieved >= D3D_FEATURE_LEVEL_10_0) {
            using_warp_     = (a.type == D3D_DRIVER_TYPE_WARP);
            achieved_level_ = achieved;
            if (using_warp_) {
                // WARP decision: see the file header comment -- accepted, logged distinctly so a
                // frame-pacing complaint on a GPU-less VM is diagnosable at a glance.
                log("create_device: no hardware adapter, using WARP (feature level 0x%04x)", (unsigned)achieved);
            } else {
                log("create_device: hardware device created (feature level 0x%04x)", (unsigned)achieved);
            }
            return true;
        }
        if (device_) {
            device_->Release();
            device_ = nullptr;
        }
        if (context_) {
            context_->Release();
            context_ = nullptr;
        }
        log("create_device: %s attempt failed (hr=0x%08lx)", a.label, (unsigned long)hr);
    }
    return false;
}

bool d3d11_backend::create_swapchain(int w, int h) {
    IDXGIDevice *dxgi_device = nullptr;
    HRESULT      hr          = device_->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void **>(&dxgi_device));
    if (FAILED(hr)) {
        log("create_swapchain: QueryInterface(IDXGIDevice) failed (hr=0x%08lx)", (unsigned long)hr);
        return false;
    }
    IDXGIAdapter *adapter = nullptr;
    hr                    = dxgi_device->GetAdapter(&adapter);
    dxgi_device->Release();
    if (FAILED(hr)) {
        log("create_swapchain: GetAdapter failed (hr=0x%08lx)", (unsigned long)hr);
        return false;
    }

    // Prefer flip-model via IDXGIFactory2::CreateSwapChainForHwnd.
    IDXGIFactory2 *factory2 = nullptr;
    hr                      = adapter->GetParent(__uuidof(IDXGIFactory2), reinterpret_cast<void **>(&factory2));
    if (SUCCEEDED(hr) && factory2) {
        DXGI_SWAP_CHAIN_DESC1 desc1 = {};
        desc1.Width                 = (UINT)w;
        desc1.Height                = (UINT)h;
        desc1.Format                = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc1.SampleDesc.Count      = 1;
        desc1.BufferUsage           = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc1.BufferCount           = 2;
        desc1.SwapEffect            = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        desc1.AlphaMode             = DXGI_ALPHA_MODE_IGNORE;

        // vsync=0 IS NOT UNCAPPED ON A FLIP-MODEL CHAIN BY ITSELF: without ALLOW_TEARING, DWM still
        // composes a windowed/borderless flip chain once per refresh and Present(0, 0) queues behind
        // it, so the game ran at the monitor rate with fps_limit=0 exactly as if vsync were on
        // (user report 2026-09-29; a remote session hides it -- its "display" is not paced). With the
        // flag, Present(0, DXGI_PRESENT_ALLOW_TEARING) goes out immediately in independent flip (a
        // borderless window covering the monitor) and is uncomposed-fast otherwise.
        tearing_                = false; // re-decided on every (re)create
        IDXGIFactory5 *factory5 = nullptr;
        if (SUCCEEDED(factory2->QueryInterface(__uuidof(IDXGIFactory5), reinterpret_cast<void **>(&factory5)))) {
            BOOL allow = FALSE;
            if (SUCCEEDED(factory5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allow, sizeof(allow))) && allow)
                tearing_ = true;
            factory5->Release();
        }
        if (tearing_) desc1.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;

        IDXGISwapChain1 *sc1 = nullptr;
        hr                   = factory2->CreateSwapChainForHwnd(device_, hwnd_, &desc1, nullptr, nullptr, &sc1);
        if (SUCCEEDED(hr)) {
            swap_chain_ = sc1; // IDXGISwapChain1 IS-A IDXGISwapChain
            flip_model_ = true;
            // We own window_mode ourselves (present_backend.h) -- DXGI must never do its own
            // Alt+Enter fullscreen switch behind our back.
            factory2->MakeWindowAssociation(hwnd_, DXGI_MWA_NO_ALT_ENTER);
        } else {
            tearing_ = false;
            log("create_swapchain: flip-model CreateSwapChainForHwnd failed (hr=0x%08lx), falling back to DISCARD",
                (unsigned long)hr);
        }
    } else {
        log("create_swapchain: IDXGIFactory2 unavailable (hr=0x%08lx), falling back to DISCARD", (unsigned long)hr);
    }

    if (!swap_chain_) {
        IDXGIFactory *factory1 = nullptr;
        hr                     = adapter->GetParent(__uuidof(IDXGIFactory), reinterpret_cast<void **>(&factory1));
        if (FAILED(hr)) {
            log("create_swapchain: legacy IDXGIFactory unavailable (hr=0x%08lx)", (unsigned long)hr);
            if (factory2) factory2->Release();
            adapter->Release();
            return false;
        }

        DXGI_SWAP_CHAIN_DESC desc               = {};
        desc.BufferDesc.Width                   = (UINT)w;
        desc.BufferDesc.Height                  = (UINT)h;
        desc.BufferDesc.Format                  = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.BufferDesc.RefreshRate.Numerator   = 0;
        desc.BufferDesc.RefreshRate.Denominator = 1;
        desc.SampleDesc.Count                   = 1;
        desc.BufferUsage                        = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount                        = 1;
        desc.OutputWindow                       = hwnd_;
        desc.Windowed                           = TRUE;
        desc.SwapEffect                         = DXGI_SWAP_EFFECT_DISCARD;

        hr = factory1->CreateSwapChain(device_, &desc, &swap_chain_);
        factory1->Release();
        if (FAILED(hr)) {
            log("create_swapchain: legacy CreateSwapChain failed (hr=0x%08lx)", (unsigned long)hr);
            if (factory2) factory2->Release();
            adapter->Release();
            return false;
        }
        flip_model_ = false;
    }

    if (factory2) factory2->Release();
    adapter->Release();
    back_buffer_w_ = w;
    back_buffer_h_ = h;
    log("create_swapchain: %dx%d, swap_effect=%s, tearing=%s", w, h, flip_model_ ? "flip_discard" : "discard",
        tearing_ ? "allowed (vsync=0 uncapped)" : "unsupported (vsync=0 may still pace to the display)");
    return true;
}

bool d3d11_backend::ensure_backbuffer_views() {
    ID3D11Texture2D *back_buffer = nullptr;
    HRESULT          hr          = swap_chain_->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void **>(&back_buffer));
    if (FAILED(hr)) {
        log("ensure_backbuffer_views: GetBuffer failed (hr=0x%08lx)", (unsigned long)hr);
        return false;
    }
    hr = device_->CreateRenderTargetView(back_buffer, nullptr, &rtv_);
    back_buffer->Release();
    if (FAILED(hr)) {
        log("ensure_backbuffer_views: CreateRenderTargetView failed (hr=0x%08lx)", (unsigned long)hr);
        return false;
    }
    return true;
}

void d3d11_backend::release_backbuffer_views() {
    if (rtv_) {
        rtv_->Release();
        rtv_ = nullptr;
    }
}

// =====================================================================================================
// Pipeline objects (shaders / samplers / cbuffer -- device-level, created once per attach())
// =====================================================================================================

bool d3d11_backend::create_pipeline() {
    HRESULT hr =
        device_->CreateVertexShader(g_fs_triangle_vs, sizeof(g_fs_triangle_vs), nullptr, &vs_fullscreen_triangle_);
    if (FAILED(hr)) {
        log("create_pipeline: CreateVertexShader failed (hr=0x%08lx)", (unsigned long)hr);
        return false;
    }
    hr = device_->CreatePixelShader(g_sample_ps, sizeof(g_sample_ps), nullptr, &ps_sample_);
    if (FAILED(hr)) {
        log("create_pipeline: CreatePixelShader(sample) failed (hr=0x%08lx)", (unsigned long)hr);
        return false;
    }
    hr = device_->CreatePixelShader(g_sharp_bilinear_ps, sizeof(g_sharp_bilinear_ps), nullptr, &ps_sharp_bilinear_);
    if (FAILED(hr)) {
        log("create_pipeline: CreatePixelShader(sharp) failed (hr=0x%08lx)", (unsigned long)hr);
        return false;
    }

    D3D11_SAMPLER_DESC sd = {};
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.ComparisonFunc                       = D3D11_COMPARISON_NEVER;
    sd.MaxLOD                               = D3D11_FLOAT32_MAX;

    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    hr        = device_->CreateSamplerState(&sd, &sampler_point_);
    if (FAILED(hr)) {
        log("create_pipeline: CreateSamplerState(point) failed (hr=0x%08lx)", (unsigned long)hr);
        return false;
    }
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    hr        = device_->CreateSamplerState(&sd, &sampler_linear_);
    if (FAILED(hr)) {
        log("create_pipeline: CreateSamplerState(linear) failed (hr=0x%08lx)", (unsigned long)hr);
        return false;
    }

    D3D11_BUFFER_DESC bd = {};
    bd.ByteWidth         = sizeof(scale_params_cb);
    bd.Usage             = D3D11_USAGE_DYNAMIC;
    bd.BindFlags         = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags    = D3D11_CPU_ACCESS_WRITE;
    hr                   = device_->CreateBuffer(&bd, nullptr, &cbuffer_scale_);
    if (FAILED(hr)) {
        log("create_pipeline: CreateBuffer(scale_params) failed (hr=0x%08lx)", (unsigned long)hr);
        return false;
    }
    return true;
}

// =====================================================================================================
// Source texture (frame upload)
// =====================================================================================================

bool d3d11_backend::ensure_source_texture(const frame_view &frame) {
    if (src_tex_ && src_w_ == frame.width && src_h_ == frame.height && src_fmt_ == frame.format) return true;

    if (src_srv_) {
        src_srv_->Release();
        src_srv_ = nullptr;
    }
    if (src_tex_) {
        src_tex_->Release();
        src_tex_ = nullptr;
    }

    const DXGI_FORMAT direct_fmt =
        (frame.format == pixel_format::rgb565) ? DXGI_FORMAT_B5G6R5_UNORM : DXGI_FORMAT_B5G5R5A1_UNORM;
    UINT       support   = 0;
    HRESULT    hr        = device_->CheckFormatSupport(direct_fmt, &support);
    const UINT needed    = D3D11_FORMAT_SUPPORT_TEXTURE2D | D3D11_FORMAT_SUPPORT_SHADER_SAMPLE;
    bool       direct_ok = SUCCEEDED(hr) && ((support & needed) == needed);

    src_w_                = frame.width;
    src_h_                = frame.height;
    src_fmt_              = frame.format;
    src_dxgi_fmt_         = direct_ok ? direct_fmt : DXGI_FORMAT_B8G8R8A8_UNORM;
    src_needs_conversion_ = !direct_ok;

    D3D11_TEXTURE2D_DESC td = {};
    td.Width                = (UINT)src_w_;
    td.Height               = (UINT)src_h_;
    td.MipLevels            = 1;
    td.ArraySize            = 1;
    td.Format               = src_dxgi_fmt_;
    td.SampleDesc.Count     = 1;
    td.Usage                = D3D11_USAGE_DYNAMIC;
    td.BindFlags            = D3D11_BIND_SHADER_RESOURCE;
    td.CPUAccessFlags       = D3D11_CPU_ACCESS_WRITE;

    hr = device_->CreateTexture2D(&td, nullptr, &src_tex_);
    if (FAILED(hr) && direct_ok) {
        // CheckFormatSupport said yes but the driver disagreed at creation time (seen on some
        // WARP/driver combinations for 16bpp dynamic textures) -- retry once with the
        // guaranteed-good BGRA8 CPU-convert path before giving up on this frame entirely.
        log("ensure_source_texture: dynamic %s texture rejected (hr=0x%08lx) despite CheckFormatSupport, falling "
            "back to BGRA8",
            direct_fmt == DXGI_FORMAT_B5G6R5_UNORM ? "B5G6R5" : "B5G5R5A1", (unsigned long)hr);
        src_dxgi_fmt_         = DXGI_FORMAT_B8G8R8A8_UNORM;
        src_needs_conversion_ = true;
        td.Format             = src_dxgi_fmt_;
        hr                    = device_->CreateTexture2D(&td, nullptr, &src_tex_);
    }
    if (FAILED(hr)) {
        log("ensure_source_texture: CreateTexture2D failed (hr=0x%08lx)", (unsigned long)hr);
        src_tex_ = nullptr;
        return false;
    }

    hr = device_->CreateShaderResourceView(src_tex_, nullptr, &src_srv_);
    if (FAILED(hr)) {
        log("ensure_source_texture: CreateShaderResourceView failed (hr=0x%08lx)", (unsigned long)hr);
        src_tex_->Release();
        src_tex_ = nullptr;
        return false;
    }

    log("ensure_source_texture: %dx%d %s (%s)", src_w_, src_h_,
        src_dxgi_fmt_ == DXGI_FORMAT_B5G6R5_UNORM
            ? "B5G6R5"
            : (src_dxgi_fmt_ == DXGI_FORMAT_B5G5R5A1_UNORM ? "B5G5R5A1" : "B8G8R8A8"),
        src_needs_conversion_ ? "CPU-converted" : "direct upload");
    return true;
}

void d3d11_backend::upload_frame(const frame_view &frame) {
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    HRESULT                  hr     = context_->Map(src_tex_, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (FAILED(hr)) {
        log("upload_frame: Map failed (hr=0x%08lx)", (unsigned long)hr);
        return;
    }

    const uint8_t *src_base = static_cast<const uint8_t *>(frame.pixels);
    uint8_t       *dst_base = static_cast<uint8_t *>(mapped.pData);

    if (!src_needs_conversion_) {
        // Direct 16bpp copy. DXGI_FORMAT_B5G6R5_UNORM's bit layout matches the game's rgb565
        // exactly (file header comment), so that case is a straight per-row memcpy. rgb555 into
        // B5G5R5A1_UNORM needs one OR pass to force the alpha bit -- the game's rgb555 leaves that
        // bit 0 (unused), which DXGI reads as fully transparent.
        for (int y = 0; y < frame.height; ++y) {
            const uint16_t *srow = reinterpret_cast<const uint16_t *>(src_base + (size_t)y * frame.pitch);
            uint16_t       *drow = reinterpret_cast<uint16_t *>(dst_base + (size_t)y * mapped.RowPitch);
            if (frame.format == pixel_format::rgb555) {
                for (int x = 0; x < frame.width; ++x) drow[x] = (uint16_t)(srow[x] | 0x8000u);
            } else {
                memcpy(drow, srow, (size_t)frame.width * 2);
            }
        }
    } else {
        // CPU expand to B8G8R8A8 (memory order B,G,R,A -- matches storing 0xAARRGGBB as a native
        // little-endian DWORD). One row at a time through a small stack buffer: no heap allocation
        // on this path. 5/6-bit -> 8-bit expansion is the standard high-bit-replicate widen, not a
        // division, to keep this cheap on WARP.
        uint32_t  row_buf[kMaxRowPixels];
        const int width = (frame.width < kMaxRowPixels) ? frame.width : kMaxRowPixels;
        for (int y = 0; y < frame.height; ++y) {
            const uint16_t *srow = reinterpret_cast<const uint16_t *>(src_base + (size_t)y * frame.pitch);
            for (int x = 0; x < width; ++x) {
                const uint16_t p = srow[x];
                uint8_t        r, g, b;
                if (frame.format == pixel_format::rgb565) {
                    r = (uint8_t)(((p >> 11) & 0x1F) << 3 | ((p >> 11) & 0x1F) >> 2);
                    g = (uint8_t)(((p >> 5) & 0x3F) << 2 | ((p >> 5) & 0x3F) >> 4);
                    b = (uint8_t)((p & 0x1F) << 3 | (p & 0x1F) >> 2);
                } else { // rgb555
                    r = (uint8_t)(((p >> 10) & 0x1F) << 3 | ((p >> 10) & 0x1F) >> 2);
                    g = (uint8_t)(((p >> 5) & 0x1F) << 3 | ((p >> 5) & 0x1F) >> 2);
                    b = (uint8_t)((p & 0x1F) << 3 | (p & 0x1F) >> 2);
                }
                row_buf[x] = 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
            }
            memcpy(dst_base + (size_t)y * mapped.RowPitch, row_buf, (size_t)width * 4);
        }
    }

    context_->Unmap(src_tex_, 0);
}

// =====================================================================================================
// Per-frame viewport / letterbox + present
// =====================================================================================================

void d3d11_backend::update_viewport_and_cbuffer(const frame_view &frame) {
    // The viewport is the WHOLE-PIXEL rect of present_backend.h fit_image_rect, not the fractional
    // frame*scale: the owned device maps mouse messages through that same rect, so the image a player
    // clicks on and the rect the click is mapped through cannot drift apart by a sub-pixel offset.
    const RECT     ir = fit_image_rect(back_buffer_w_, back_buffer_h_, frame.width, frame.height, cfg_.integer_scale);
    D3D11_VIEWPORT vp = {};
    vp.TopLeftX       = (float)ir.left;
    vp.TopLeftY       = (float)ir.top;
    vp.Width          = (float)(ir.right - ir.left);
    vp.Height         = (float)(ir.bottom - ir.top);
    vp.MinDepth       = 0.0f;
    vp.MaxDepth       = 1.0f;
    context_->RSSetViewports(1, &vp);

    if (cfg_.filter == scale_filter::sharp || cfg_.filter == scale_filter::area) {
        // The scale per axis is the image rect's own (whole pixels, so x and y can differ by a hair).
        float sx = frame.width > 0 ? vp.Width / (float)frame.width : 1.0f;
        float sy = frame.height > 0 ? vp.Height / (float)frame.height : 1.0f;
        if (cfg_.filter == scale_filter::sharp) {
            // libretro sharp-bilinear-simple: prescale = max(floor(scale), 1). vp/frame is exact for an
            // integer ratio here, so plain floor (libretro adds 0.01 against a float round-down).
            sx = (float)(int)sx;
            sy = (float)(int)sy;
        }
        // area: the TRUE scale, so the blend band is exactly one output pixel (the box filter; see the
        // HLSL note above). Below 1x (a window smaller than the mode) both degrade to plain bilinear.
        if (sx < 1.0f) sx = 1.0f;
        if (sy < 1.0f) sy = 1.0f;

        D3D11_MAPPED_SUBRESOURCE mapped = {};
        if (SUCCEEDED(context_->Map(cbuffer_scale_, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
            scale_params_cb *p = static_cast<scale_params_cb *>(mapped.pData);
            p->src_w           = (float)frame.width;
            p->src_h           = (float)frame.height;
            p->texel_scale_x   = sx;
            p->texel_scale_y   = sy;
            context_->Unmap(cbuffer_scale_, 0);
        }
    }
}

// ---- output readback (net_selftest.exe scalefiltertest, and its --dump mode) ---------------------------
// One pending request: the next present() copies the back buffer, right after the game-frame draw and
// BEFORE the ImGui overlay, into `buf` (w*h BGRA8, top row first). A request for a size other than the
// back buffer's is answered "failed" so a caller cannot wait forever.
namespace {
uint32_t *volatile g_rb_buf = nullptr;
int          g_rb_w         = 0;
int          g_rb_h         = 0;
volatile int g_rb_done      = 0; // 0 pending/none, 1 ok, -1 failed
} // namespace

void d3d11_request_readback(uint32_t *buf, int w, int h) {
    g_rb_done = 0;
    g_rb_w    = w;
    g_rb_h    = h;
    g_rb_buf  = buf;
}

int d3d11_readback_state() { return g_rb_done; }

void d3d11_backend::service_readback() {
    uint32_t *buf = g_rb_buf;
    if (!buf) return;
    g_rb_buf = nullptr;
    if (g_rb_w != back_buffer_w_ || g_rb_h != back_buffer_h_) {
        log("readback: asked %dx%d, back buffer is %dx%d -- refused", g_rb_w, g_rb_h, back_buffer_w_, back_buffer_h_);
        g_rb_done = -1;
        return;
    }
    ID3D11Texture2D *bb = nullptr;
    if (FAILED(swap_chain_->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void **>(&bb)))) {
        g_rb_done = -1;
        return;
    }
    D3D11_TEXTURE2D_DESC td = {};
    bb->GetDesc(&td);
    td.Usage            = D3D11_USAGE_STAGING;
    td.BindFlags        = 0;
    td.CPUAccessFlags   = D3D11_CPU_ACCESS_READ;
    td.MiscFlags        = 0;
    ID3D11Texture2D *st = nullptr;
    int              ok = -1;
    if (SUCCEEDED(device_->CreateTexture2D(&td, nullptr, &st))) {
        context_->CopyResource(st, bb);
        D3D11_MAPPED_SUBRESOURCE m = {};
        if (SUCCEEDED(context_->Map(st, 0, D3D11_MAP_READ, 0, &m))) {
            for (int y = 0; y < g_rb_h; ++y)
                memcpy(buf + (size_t)y * g_rb_w, static_cast<const uint8_t *>(m.pData) + (size_t)y * m.RowPitch,
                       (size_t)g_rb_w * 4);
            context_->Unmap(st, 0);
            ok = 1;
        }
        st->Release();
    }
    bb->Release();
    g_rb_done = ok;
}

bool d3d11_backend::handle_client_resize() {
    if (!hwnd_ || !swap_chain_) return false;
    if (IsIconic(hwnd_)) return false; // minimized: never Present into a 0-area window

    RECT rc = {};
    GetClientRect(hwnd_, &rc);
    const int w = rc.right - rc.left;
    const int h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0) return false;
    if (w == back_buffer_w_ && h == back_buffer_h_) return true;

    release_backbuffer_views();
    const HRESULT hr = swap_chain_->ResizeBuffers(0, (UINT)w, (UINT)h, DXGI_FORMAT_UNKNOWN,
                                                  tearing_ ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0); // flags must match creation
    if (FAILED(hr)) {
        log("handle_client_resize: ResizeBuffers(%dx%d) failed (hr=0x%08lx)", w, h, (unsigned long)hr);
        if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
            device_lost_ = true;
            release_device_resources();
        }
        return false;
    }
    back_buffer_w_ = w;
    back_buffer_h_ = h;
    if (!ensure_backbuffer_views()) return false;
    log("handle_client_resize: resized to %dx%d", w, h);
    return true;
}

bool d3d11_backend::present(const frame_view &frame) {
    if (device_lost_) {
        // Lazy recovery: rebuild everything from scratch. attach() detaches first, so it is safe to
        // call again with the config/hwnd this instance already has.
        if (!attach(hwnd_, cfg_)) return false;
    }

    if (!handle_client_resize()) return false; // minimized / zero-size client rect: skip, not an error
    if (!ensure_source_texture(frame)) return false;

    upload_frame(frame);
    update_viewport_and_cbuffer(frame);

    const float clear[4] = {0.0f, 0.0f, 0.0f, 1.0f}; // letterbox bars
    context_->OMSetRenderTargets(1, &rtv_, nullptr);
    context_->ClearRenderTargetView(rtv_, clear);

    context_->IASetInputLayout(nullptr); // VS has no per-vertex input, only SV_VertexID -- none needed
    context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context_->VSSetShader(vs_fullscreen_triangle_, nullptr, 0);

    // sharp and area are the same shader (see the HLSL note): only texel_scale differs.
    const bool          banded  = cfg_.filter == scale_filter::sharp || cfg_.filter == scale_filter::area;
    ID3D11SamplerState *sampler = (cfg_.filter == scale_filter::point) ? sampler_point_ : sampler_linear_;
    ID3D11PixelShader  *ps      = banded ? ps_sharp_bilinear_ : ps_sample_;
    context_->PSSetShader(ps, nullptr, 0);
    context_->PSSetShaderResources(0, 1, &src_srv_);
    context_->PSSetSamplers(0, 1, &sampler);
    if (banded) context_->PSSetConstantBuffers(0, 1, &cbuffer_scale_);

    context_->Draw(3, 0);
    service_readback(); // the filter's output alone, before the overlay (a no-op unless requested)

    // -------------------------------------------------------------------------------------------
    // PT-GFX4 HOOK POINT: the ImGui overlay pass, after the Draw(3, 0) above and before
    // swap_chain_->Present() below. The render target is bound; imgui_impl_dx11 sets its own
    // full-back-buffer viewport (output resolution) and restores the state it touched. A no-op
    // unless [video] imgui=1 armed it.
    // -------------------------------------------------------------------------------------------
    imgui_overlay::on_frame(device_, context_, hwnd_);

    const HRESULT hr = cfg_.vsync ? swap_chain_->Present(1, 0)
                                  : swap_chain_->Present(0, tearing_ ? DXGI_PRESENT_ALLOW_TEARING : 0);
    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
        log("present: device lost (hr=0x%08lx, reason=0x%08lx) -- recreating on next present", (unsigned long)hr,
            (unsigned long)device_->GetDeviceRemovedReason());
        device_lost_ = true;
        release_device_resources();
        return false;
    }
    if (FAILED(hr)) {
        log("present: Present failed (hr=0x%08lx)", (unsigned long)hr);
        return false;
    }
    return true;
}

// =====================================================================================================
// Lifecycle
// =====================================================================================================

bool d3d11_backend::attach(HWND hwnd, const backend_config &cfg) {
    detach(); // idempotent re-attach: release any previous state first

    hwnd_        = hwnd;
    cfg_         = cfg;
    device_lost_ = false;

    if (!create_device()) {
        log("attach: no usable D3D11 device -- caller falls back to gdi");
        detach();
        return false;
    }
    if (!create_pipeline()) {
        log("attach: pipeline object creation failed");
        detach();
        return false;
    }

    RECT rc = {};
    GetClientRect(hwnd_, &rc);
    int w = rc.right - rc.left;
    int h = rc.bottom - rc.top;
    if (w <= 0) w = 1;
    if (h <= 0) h = 1;

    if (!create_swapchain(w, h)) {
        log("attach: swap chain creation failed");
        detach();
        return false;
    }
    if (!ensure_backbuffer_views()) {
        log("attach: back buffer render target view creation failed");
        detach();
        return false;
    }

    static const char *const FILTERS[] = {"point", "linear", "sharp", "area"};
    log("attach: ok, driver=%s swap_effect=%s filter=%s scale=%s", using_warp_ ? "WARP" : "hardware",
        flip_model_ ? "flip_discard" : "discard", (unsigned)cfg_.filter < 4 ? FILTERS[(int)cfg_.filter] : "?",
        cfg_.integer_scale ? "integer" : "fit");
    return true;
}

void d3d11_backend::on_mode(int width, int height, pixel_format format) {
    // No pixel data at this point -- just log. ensure_source_texture() derives the real texture from
    // the next frame_view present() receives, which is authoritative.
    log("on_mode: game announced %dx%d format=%s", width, height, format == pixel_format::rgb565 ? "rgb565" : "rgb555");
}

void d3d11_backend::on_window_changed() {
    if (device_lost_) return; // next present() recreates everything anyway
    handle_client_resize();
}

void d3d11_backend::release_device_resources() {
    imgui_overlay::on_device_release(); // first: ImGui holds references on device_/context_
    release_backbuffer_views();
    if (src_srv_) {
        src_srv_->Release();
        src_srv_ = nullptr;
    }
    if (src_tex_) {
        src_tex_->Release();
        src_tex_ = nullptr;
    }
    src_w_ = src_h_ = 0;
    if (cbuffer_scale_) {
        cbuffer_scale_->Release();
        cbuffer_scale_ = nullptr;
    }
    if (sampler_linear_) {
        sampler_linear_->Release();
        sampler_linear_ = nullptr;
    }
    if (sampler_point_) {
        sampler_point_->Release();
        sampler_point_ = nullptr;
    }
    if (ps_sharp_bilinear_) {
        ps_sharp_bilinear_->Release();
        ps_sharp_bilinear_ = nullptr;
    }
    if (ps_sample_) {
        ps_sample_->Release();
        ps_sample_ = nullptr;
    }
    if (vs_fullscreen_triangle_) {
        vs_fullscreen_triangle_->Release();
        vs_fullscreen_triangle_ = nullptr;
    }
    if (swap_chain_) {
        swap_chain_->Release();
        swap_chain_ = nullptr;
    }
    if (context_) {
        context_->ClearState();
        context_->Release();
        context_ = nullptr;
    }
    if (device_) {
        device_->Release();
        device_ = nullptr;
    }
    if (d3d11_dll_) {
        FreeLibrary(d3d11_dll_);
        d3d11_dll_ = nullptr;
    }
    back_buffer_w_ = back_buffer_h_ = 0;
    flip_model_                     = false;
    using_warp_                     = false;
}

void d3d11_backend::detach() {
    // The window binding first (subclass + ImGui's Win32 backend), then the device. Only when this
    // instance was bound: attach() calls detach() before its first bind, and the overlay module is a
    // singleton another backend instance may be using.
    if (hwnd_) imgui_overlay::on_detach();
    release_device_resources();
    hwnd_        = nullptr;
    device_lost_ = false;
}

// =====================================================================================================
// Logging -- appends to the same mh_video.log the rest of the [video] seam writes to (video.cpp,
// arm-time banners per mh_run_context.h's MH_ProcessDir doc comment). Self-contained (own buffer,
// own file handle per call) rather than sharing video.cpp's internal vid_log helper, since that is
// file-local to a different translation unit; both writers append under FILE_SHARE_READ|WRITE, which
// is safe. Only notable state transitions are logged here (attach, fallback choices, resize, device
// loss) -- never per-frame -- so this stays a banner log, not a firehose.
// =====================================================================================================

void d3d11_backend::log(const char *fmt, ...) {
    char    line[256];
    va_list ap;
    va_start(ap, fmt);
    wvsprintfA(line, fmt, ap);
    va_end(ap);
    int n = lstrlenA(line);
    if (n < (int)sizeof(line) - 2) {
        line[n++] = '\n';
        line[n]   = 0;
    }

    char path[MAX_PATH];
    wsprintfA(path, "%smh_video.log", MH_ProcessDir());
    HANDLE h = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        DWORD w = 0;
        WriteFile(h, line, (DWORD)lstrlenA(line), &w, nullptr);
        CloseHandle(h);
    }
}

// =====================================================================================================
// Factory
// =====================================================================================================

present_backend *make_d3d11_backend() { return new d3d11_backend(); }

} // namespace mh::gfx
