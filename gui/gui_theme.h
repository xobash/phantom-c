/* Phantom GUI themes.
 *
 * Default: "Verdant" — the dark green-tinted language carried over from the
 * original WPF app (shell #131918, canvas #171C1B, cards, #0078D4 accent).
 *
 * -DPHANTOM_THEME_VOID selects "Void" — a cosmic, pure-black language:
 * the void as the only surface, bone-white type, hairline borders, full
 * pill geometry, and a single violet (#8052FF) as the only filled color.
 * No shadows, no gradients; depth comes from contrast alone.
 */
#ifndef PHANTOM_GUI_THEME_H
#define PHANTOM_GUI_THEME_H

#ifdef PHANTOM_THEME_VOID

#define PHANTOM_THEME_NAME L"Void"

#define CLR_BG        RGB(0x00, 0x00, 0x00)   /* the void */
#define CLR_SHELL     RGB(0x00, 0x00, 0x00)   /* sidebar sits on the same canvas */
#define CLR_CARD      RGB(0x00, 0x00, 0x00)   /* cards are hairline borders on void */
#define CLR_CARD2     RGB(0x0A, 0x0A, 0x0A)   /* ghost button fill (barely there) */
#define CLR_BORDER    RGB(0x2B, 0x2B, 0x2B)   /* hairline ~white @ 17% */
#define CLR_TEXT      RGB(0xFF, 0xFF, 0xFF)   /* bone */
#define CLR_MUTED     RGB(0xBD, 0xBD, 0xBD)   /* ash */
#define CLR_FAINT     RGB(0x9A, 0x9A, 0x9A)   /* smoke */
#define CLR_ACCENT    RGB(0x80, 0x52, 0xFF)   /* plum voltage — the only filled color */
#define CLR_ACCENT_BR RGB(0xA8, 0x8C, 0xFF)   /* lighter plum for glyphs/selected text */
#define CLR_NAV_SEL   RGB(0x16, 0x10, 0x2A)   /* violet-tinted void for the active pill */
#define CLR_INPUT     RGB(0x0A, 0x0A, 0x0A)
#define CLR_ROW       RGB(0x00, 0x00, 0x00)
#define CLR_ROW_ALT   RGB(0x0B, 0x0B, 0x0B)
#define CLR_ROW_SEL   RGB(0x2A, 0x1C, 0x55)   /* selection: dark plum */

#define CLR_BTN_PRI_DISABLED  RGB(0x3A, 0x2E, 0x66)
#define CLR_BTN_PRI_PRESSED   RGB(0x66, 0x40, 0xCC)
#define CLR_BTN_TXT_DISABLED  RGB(0x6E, 0x6E, 0x6E)

/* Pill geometry — every interactive surface is fully rounded. */
#define RAD_NAV     24
#define RAD_BUTTON  24
#define RAD_CARD    24
#define RAD_BADGE   22

#else /* default: Verdant */

#define PHANTOM_THEME_NAME L"Verdant"

#define CLR_BG        RGB(0x17, 0x1C, 0x1B)
#define CLR_SHELL     RGB(0x13, 0x19, 0x18)
#define CLR_CARD      RGB(0x1D, 0x24, 0x23)
#define CLR_CARD2     RGB(0x21, 0x2A, 0x28)
#define CLR_BORDER    RGB(0x34, 0x41, 0x3D)
#define CLR_TEXT      RGB(0xF3, 0xF4, 0xF1)
#define CLR_MUTED     RGB(0xB9, 0xC2, 0xBC)
#define CLR_FAINT     RGB(0x7E, 0x8A, 0x85)
#define CLR_ACCENT    RGB(0x00, 0x78, 0xD4)
#define CLR_ACCENT_BR RGB(0x3B, 0x9D, 0xFF)
#define CLR_NAV_SEL   RGB(0x28, 0x31, 0x2F)
#define CLR_INPUT     RGB(0x14, 0x1A, 0x19)
#define CLR_ROW       RGB(0x1B, 0x22, 0x21)
#define CLR_ROW_ALT   RGB(0x1F, 0x27, 0x25)
#define CLR_ROW_SEL   RGB(0x1E, 0x3A, 0x52)

#define CLR_BTN_PRI_DISABLED  RGB(0x2A, 0x42, 0x55)
#define CLR_BTN_PRI_PRESSED   RGB(0x00, 0x5A, 0xA0)
#define CLR_BTN_TXT_DISABLED  RGB(0x6E, 0x78, 0x74)

#define RAD_NAV     10
#define RAD_BUTTON  8
#define RAD_CARD    12
#define RAD_BADGE   11

#endif
#endif /* PHANTOM_GUI_THEME_H */
