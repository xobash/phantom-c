/* Phantom GUI runtime theme system.
 *
 * Two design languages ship in one binary, switchable at runtime from
 * Settings (persisted to %LOCALAPPDATA%\Phantom\settings.json):
 *
 *  - "Verdant" — the dark green-tinted language carried over from the
 *    original WPF app: shell/canvas/card surfaces, #0078D4 accent.
 *  - "Void"    — cosmic minimalism (DESIGN.md): pure-black canvas as the
 *    only surface, bone/ash/smoke text, hairline borders instead of
 *    fills, full pill geometry, ultra-thin display type, a particle
 *    constellation as the brand visual, and a single violet (#8052FF)
 *    as the only filled color. Amber (#FFB829) is reserved for outlined
 *    emphasis; lichen (#15846E) appears only in the constellation.
 *
 * The accent can additionally be overridden per user: theme default,
 * the Windows accent color (DWM colorization), or a custom hex value —
 * parity with the original ThemeService.
 */
#ifndef PHANTOM_GUI_THEME_H
#define PHANTOM_GUI_THEME_H

#include <windows.h>
#include <stdbool.h>

typedef enum { PH_THEME_VERDANT = 0, PH_THEME_VOID = 1 } ph_theme_id;
typedef enum { PH_ACCENT_THEME = 0, PH_ACCENT_WINDOWS = 1, PH_ACCENT_CUSTOM = 2 } ph_accent_mode;

typedef struct {
    const wchar_t *name;
    COLORREF bg, shell, card, card2, border;
    COLORREF text, muted, faint;
    COLORREF accent, accent_br;
    COLORREF nav_sel, input;
    COLORREF row, row_alt, row_sel;
    COLORREF btn_pri_disabled, btn_pri_pressed, btn_txt_disabled;
    COLORREF spark;            /* outlined emphasis (Void: amber) */
    COLORREF lichen;           /* constellation accent (Void only) */
    int rad_nav, rad_button, rad_card, rad_badge;
    int title_weight;          /* section titles */
    int display_weight;        /* hero (uptime) */
    bool uppercase_buttons;    /* Void: 12px tracked uppercase pill labels */
    bool particles;            /* Void: constellation on Home */
} ph_theme;

extern ph_theme g_theme;

/* The existing drawing code reads these names. */
#define CLR_BG        (g_theme.bg)
#define CLR_SHELL     (g_theme.shell)
#define CLR_CARD      (g_theme.card)
#define CLR_CARD2     (g_theme.card2)
#define CLR_BORDER    (g_theme.border)
#define CLR_TEXT      (g_theme.text)
#define CLR_MUTED     (g_theme.muted)
#define CLR_FAINT     (g_theme.faint)
#define CLR_ACCENT    (g_theme.accent)
#define CLR_ACCENT_BR (g_theme.accent_br)
#define CLR_NAV_SEL   (g_theme.nav_sel)
#define CLR_INPUT     (g_theme.input)
#define CLR_ROW       (g_theme.row)
#define CLR_ROW_ALT   (g_theme.row_alt)
#define CLR_ROW_SEL   (g_theme.row_sel)
#define CLR_BTN_PRI_DISABLED (g_theme.btn_pri_disabled)
#define CLR_BTN_PRI_PRESSED  (g_theme.btn_pri_pressed)
#define CLR_BTN_TXT_DISABLED (g_theme.btn_txt_disabled)
#define RAD_NAV     (g_theme.rad_nav)
#define RAD_BUTTON  (g_theme.rad_button)
#define RAD_CARD    (g_theme.rad_card)
#define RAD_BADGE   (g_theme.rad_badge)

static const ph_theme PH_THEMES[2] = {
    [PH_THEME_VERDANT] = {
        .name = L"Verdant",
        .bg = RGB(0x17, 0x1C, 0x1B), .shell = RGB(0x13, 0x19, 0x18),
        .card = RGB(0x1D, 0x24, 0x23), .card2 = RGB(0x21, 0x2A, 0x28),
        .border = RGB(0x34, 0x41, 0x3D),
        .text = RGB(0xF3, 0xF4, 0xF1), .muted = RGB(0xB9, 0xC2, 0xBC), .faint = RGB(0x7E, 0x8A, 0x85),
        .accent = RGB(0x00, 0x78, 0xD4), .accent_br = RGB(0x3B, 0x9D, 0xFF),
        .nav_sel = RGB(0x28, 0x31, 0x2F), .input = RGB(0x14, 0x1A, 0x19),
        .row = RGB(0x1B, 0x22, 0x21), .row_alt = RGB(0x1F, 0x27, 0x25), .row_sel = RGB(0x1E, 0x3A, 0x52),
        .btn_pri_disabled = RGB(0x2A, 0x42, 0x55), .btn_pri_pressed = RGB(0x00, 0x5A, 0xA0),
        .btn_txt_disabled = RGB(0x6E, 0x78, 0x74),
        .spark = RGB(0x3B, 0x9D, 0xFF), .lichen = RGB(0x15, 0x84, 0x6E),
        .rad_nav = 10, .rad_button = 8, .rad_card = 12, .rad_badge = 11,
        .title_weight = FW_SEMIBOLD, .display_weight = FW_LIGHT,
        .uppercase_buttons = false, .particles = false,
    },
    [PH_THEME_VOID] = {
        .name = L"Void",
        .bg = RGB(0x00, 0x00, 0x00), .shell = RGB(0x00, 0x00, 0x00),
        .card = RGB(0x00, 0x00, 0x00), .card2 = RGB(0x0A, 0x0A, 0x0A),
        .border = RGB(0x29, 0x29, 0x29),
        .text = RGB(0xFF, 0xFF, 0xFF), .muted = RGB(0xBD, 0xBD, 0xBD), .faint = RGB(0x9A, 0x9A, 0x9A),
        .accent = RGB(0x80, 0x52, 0xFF), .accent_br = RGB(0xA8, 0x8C, 0xFF),
        .nav_sel = RGB(0x14, 0x0E, 0x26), .input = RGB(0x0A, 0x0A, 0x0A),
        .row = RGB(0x00, 0x00, 0x00), .row_alt = RGB(0x0B, 0x0B, 0x0B), .row_sel = RGB(0x2A, 0x1C, 0x55),
        .btn_pri_disabled = RGB(0x3A, 0x2E, 0x66), .btn_pri_pressed = RGB(0x66, 0x40, 0xCC),
        .btn_txt_disabled = RGB(0x6E, 0x6E, 0x6E),
        .spark = RGB(0xFF, 0xB8, 0x29), .lichen = RGB(0x15, 0x84, 0x6E),
        .rad_nav = 24, .rad_button = 24, .rad_card = 24, .rad_badge = 22,
        .title_weight = FW_EXTRALIGHT, .display_weight = FW_EXTRALIGHT,
        .uppercase_buttons = true, .particles = true,
    },
};

#endif /* PHANTOM_GUI_THEME_H */
