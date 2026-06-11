/* GPU-accelerated constellation renderer (Direct2D).
 *
 * d2d1.dll is loaded dynamically at runtime; if it is missing or render
 * target creation fails, the caller keeps using the GDI fallback path.
 * Direct2D itself degrades hardware -> WARP software rasterization
 * automatically, so "active" does not strictly guarantee GPU — but it
 * does guarantee antialiased, vsync-presented, composited rendering.
 */
#ifndef PHANTOM_GUI_D2D_H
#define PHANTOM_GUI_D2D_H
#ifdef _WIN32

#include <windows.h>
#include <stdbool.h>

typedef struct {
    COLORREF bg;
    COLORREF bone;      /* text/particle base */
    COLORREF accent;    /* plum voltage */
    COLORREF accent_br;
    COLORREF spark;     /* amber */
    COLORREF lichen;    /* teal */
    bool particles;     /* false: clear to bg only */
} ph_d2d_scene;

/* Bind a render target to the canvas window. Safe to call once at startup;
 * returns false when Direct2D is unavailable (caller uses GDI). */
bool ph_d2d_init(HWND canvas);

bool ph_d2d_active(void);

/* Resize the swap surface after the canvas window moves. */
void ph_d2d_resize(int width, int height);

/* Render one frame (time-driven; call from the animation timer).
 * Returns false if the device was lost and could not be recreated —
 * the caller should fall back to GDI. */
bool ph_d2d_render(const ph_d2d_scene *scene, double t_seconds);

void ph_d2d_shutdown(void);

#endif /* _WIN32 */
#endif
