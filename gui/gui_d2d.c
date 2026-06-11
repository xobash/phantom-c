#ifdef _WIN32

#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d2d1.h>
#include <math.h>

#include "gui_d2d.h"

/* IID_ID2D1Factory, defined locally so we don't depend on libuuid. */
static const IID PH_IID_ID2D1Factory =
    { 0x06152247, 0x6f50, 0x465a, { 0x92, 0x45, 0x11, 0x8b, 0xfd, 0x3b, 0x60, 0x07 } };

typedef HRESULT(WINAPI *D2D1CreateFactoryFn)(D2D1_FACTORY_TYPE, REFIID, const D2D1_FACTORY_OPTIONS *, void **);

static struct {
    HMODULE dll;
    ID2D1Factory *factory;
    ID2D1HwndRenderTarget *rt;
    ID2D1SolidColorBrush *brush;
    HWND canvas;
    bool active;
} d2d;

static D2D1_COLOR_F color_from(COLORREF c, float alpha) {
    D2D1_COLOR_F out;
    out.r = (float)GetRValue(c) / 255.0f;
    out.g = (float)GetGValue(c) / 255.0f;
    out.b = (float)GetBValue(c) / 255.0f;
    out.a = alpha;
    return out;
}

static bool create_target(void) {
    RECT rc;
    GetClientRect(d2d.canvas, &rc);
    UINT w = (UINT)(rc.right - rc.left), h = (UINT)(rc.bottom - rc.top);
    if (w == 0) w = 1;
    if (h == 0) h = 1;

    D2D1_RENDER_TARGET_PROPERTIES props;
    memset(&props, 0, sizeof props);
    props.type = D2D1_RENDER_TARGET_TYPE_DEFAULT; /* hardware, WARP fallback */
    props.pixelFormat.format = DXGI_FORMAT_UNKNOWN;
    props.pixelFormat.alphaMode = D2D1_ALPHA_MODE_UNKNOWN;

    D2D1_HWND_RENDER_TARGET_PROPERTIES hwnd_props;
    memset(&hwnd_props, 0, sizeof hwnd_props);
    hwnd_props.hwnd = d2d.canvas;
    hwnd_props.pixelSize.width = w;
    hwnd_props.pixelSize.height = h;
    hwnd_props.presentOptions = D2D1_PRESENT_OPTIONS_NONE; /* vsync */

    if (FAILED(ID2D1Factory_CreateHwndRenderTarget(d2d.factory, &props, &hwnd_props, &d2d.rt)))
        return false;

    D2D1_COLOR_F white = { 1.0f, 1.0f, 1.0f, 1.0f };
    if (FAILED(ID2D1HwndRenderTarget_CreateSolidColorBrush(d2d.rt, &white, NULL, &d2d.brush))) {
        ID2D1HwndRenderTarget_Release(d2d.rt);
        d2d.rt = NULL;
        return false;
    }
    return true;
}

static void release_target(void) {
    if (d2d.brush) { ID2D1SolidColorBrush_Release(d2d.brush); d2d.brush = NULL; }
    if (d2d.rt) { ID2D1HwndRenderTarget_Release(d2d.rt); d2d.rt = NULL; }
}

bool ph_d2d_init(HWND canvas) {
    d2d.canvas = canvas;
    d2d.dll = LoadLibraryW(L"d2d1.dll");
    if (!d2d.dll) return false;
    D2D1CreateFactoryFn create = (D2D1CreateFactoryFn)(void *)GetProcAddress(d2d.dll, "D2D1CreateFactory");
    if (!create) return false;
    D2D1_FACTORY_OPTIONS opts;
    memset(&opts, 0, sizeof opts);
    if (FAILED(create(D2D1_FACTORY_TYPE_SINGLE_THREADED, &PH_IID_ID2D1Factory, &opts, (void **)&d2d.factory)))
        return false;
    if (!create_target()) {
        ID2D1Factory_Release(d2d.factory);
        d2d.factory = NULL;
        return false;
    }
    d2d.active = true;
    return true;
}

bool ph_d2d_active(void) {
    return d2d.active;
}

void ph_d2d_resize(int width, int height) {
    if (!d2d.active || !d2d.rt) return;
    D2D1_SIZE_U size = { (UINT32)(width > 0 ? width : 1), (UINT32)(height > 0 ? height : 1) };
    ID2D1HwndRenderTarget_Resize(d2d.rt, &size);
}

/* One particle quad rotated 45° = diamond; D2D transforms make it cheap. */
static void fill_diamond(float cx, float cy, float half) {
    D2D1_MATRIX_3X2_F rot;
    float c = 0.70710678f, s = 0.70710678f;
    rot._11 = c;  rot._12 = s;
    rot._21 = -s; rot._22 = c;
    rot._31 = cx - c * cx + s * cy;
    rot._32 = cy - s * cx - c * cy;
    ID2D1HwndRenderTarget_SetTransform(d2d.rt, &rot);
    D2D1_RECT_F r = { cx - half, cy - half, cx + half, cy + half };
    ID2D1HwndRenderTarget_FillRectangle(d2d.rt, &r, (ID2D1Brush *)d2d.brush);
    D2D1_MATRIX_3X2_F identity;
    identity._11 = 1; identity._12 = 0;
    identity._21 = 0; identity._22 = 1;
    identity._31 = 0; identity._32 = 0;
    ID2D1HwndRenderTarget_SetTransform(d2d.rt, &identity);
}

bool ph_d2d_render(const ph_d2d_scene *scene, double t) {
    if (!d2d.active || !d2d.rt) return false;

    D2D1_SIZE_F size = ID2D1HwndRenderTarget_GetSize(d2d.rt);
    float w = size.width, h = size.height;

    ID2D1HwndRenderTarget_BeginDraw(d2d.rt);
    D2D1_COLOR_F bg = color_from(scene->bg, 1.0f);
    ID2D1HwndRenderTarget_Clear(d2d.rt, &bg);

    if (scene->particles && w > 40.0f && h > 40.0f) {
        unsigned seed = 0x9E3779B9u;
        for (int i = 0; i < 460; i++) {
            seed = seed * 1664525u + 1013904223u;
            unsigned r1 = (seed >> 8) & 0xFFFF;
            seed = seed * 1664525u + 1013904223u;
            unsigned r2 = (seed >> 8) & 0xFFFF;
            seed = seed * 1664525u + 1013904223u;
            unsigned r3 = (seed >> 8) & 0xFFFF;

            /* anchor biased toward the focal center */
            float fx = (((float)r1 / 65535.0f) + ((float)r2 / 65535.0f)) / 2.0f;
            float fy = (((float)r2 / 65535.0f) + ((float)r3 / 65535.0f)) / 2.0f;

            /* per-particle orbit */
            float orbit_r = 3.0f + (float)(r3 % 1400) / 100.0f;
            float speed = 0.15f + (float)(r1 % 85) / 100.0f;
            if (i & 1) speed = -speed;
            float ang = speed * (float)t + (float)(r2 % 628) / 100.0f;
            float x = fx * w + orbit_r * cosf(ang);
            float y = fy * h + orbit_r * 0.6f * sinf(ang);
            if (x < 2.0f || x > w - 4.0f || y < 2.0f || y > h - 4.0f) continue;

            float half = 1.0f + (float)(r3 % 200) / 100.0f; /* 1..3 px half-size */

            /* alpha: base per particle, edge falloff, slow twinkle pulse */
            float base_a = 0.35f + (float)(r1 % 60) / 100.0f;
            float dx = (x / w) - 0.5f, dy = (y / h) - 0.5f;
            float dist = sqrtf(dx * dx + dy * dy) * 2.0f;
            float edge = dist > 1.0f ? 0.15f : 1.0f - 0.7f * dist;
            float pulse = 0.65f + 0.35f * sinf((float)t * 1.6f + (float)(i % 41));
            float alpha = base_a * edge * pulse;
            if (alpha < 0.04f) continue;

            COLORREF c;
            unsigned roll = r1 % 100;
            if (roll < 52) c = scene->bone;
            else if (roll < 74) c = scene->accent;
            else if (roll < 86) c = scene->lichen;
            else if (roll < 94) c = scene->spark;
            else c = scene->accent_br;
            D2D1_COLOR_F col = color_from(c, alpha);
            ID2D1SolidColorBrush_SetColor(d2d.brush, &col);

            switch (r2 % 3) {
                case 0: {
                    D2D1_ELLIPSE e = { { x, y }, half, half };
                    ID2D1HwndRenderTarget_FillEllipse(d2d.rt, &e, (ID2D1Brush *)d2d.brush);
                    break;
                }
                case 1: {
                    D2D1_RECT_F r = { x - half, y - half, x + half, y + half };
                    ID2D1HwndRenderTarget_FillRectangle(d2d.rt, &r, (ID2D1Brush *)d2d.brush);
                    break;
                }
                default:
                    fill_diamond(x, y, half * 1.2f);
                    break;
            }
        }
    }

    HRESULT hr = ID2D1HwndRenderTarget_EndDraw(d2d.rt, NULL, NULL);
    if (hr == (HRESULT)D2DERR_RECREATE_TARGET) {
        release_target();
        if (!create_target()) {
            d2d.active = false;
            return false;
        }
    }
    return true;
}

void ph_d2d_shutdown(void) {
    release_target();
    if (d2d.factory) { ID2D1Factory_Release(d2d.factory); d2d.factory = NULL; }
    d2d.active = false;
}

#endif /* _WIN32 */
