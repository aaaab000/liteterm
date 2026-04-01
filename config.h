#ifndef LITETERM_CONFIG_H
#define LITETERM_CONFIG_H

/* Шрифт */
#define FONT_NAME    "Roboto Mono Regular:size=11"
#define FONT_BOLD    "Roboto Mono Regular:size=11"

/* Размер по умолчанию (в символах) */
#define DEFAULT_COLS  80
#define DEFAULT_ROWS  24

/* Прокрутка — сколько строк хранить */
#define SCROLLBACK    2000

/* Цветовая схема Nord-inspired dark */
#define COLOR_BG       0x1A1B26   /* глубокий тёмно-синий */
#define COLOR_FG       0xC0CAF5   /* светло-лавандовый */
#define COLOR_CURSOR   0x7AA2F7   /* голубой курсор */
#define COLOR_SEL_BG   0x33467C   /* выделение */
#define COLOR_SEL_FG   0xC0CAF5

/* Прозрачность бордера (для интеграции с LiteWM) */
#define INTERNAL_PAD   6          /* внутренний отступ в пикселях */

/* 16 цветов терминала */
#define COLOR_0   0x15161E   /* black */
#define COLOR_1   0xF7768E   /* red */
#define COLOR_2   0x9ECE6A   /* green */
#define COLOR_3   0xE0AF68   /* yellow */
#define COLOR_4   0x7AA2F7   /* blue */
#define COLOR_5   0xBB9AF7   /* magenta */
#define COLOR_6   0x7DCFFF   /* cyan */
#define COLOR_7   0xA9B1D6   /* white */
#define COLOR_8   0x414868   /* bright black */
#define COLOR_9   0xF7768E   /* bright red */
#define COLOR_10  0x9ECE6A   /* bright green */
#define COLOR_11  0xE0AF68   /* bright yellow */
#define COLOR_12  0x7AA2F7   /* bright blue */
#define COLOR_13  0xBB9AF7   /* bright magenta */
#define COLOR_14  0x7DCFFF   /* bright cyan */
#define COLOR_15  0xC0CAF5   /* bright white */

/* Шелл */
#define SHELL      "/bin/bash"

/* Ускорение клавиш (Ctrl+Shift+...) */
#define KEYBIND_COPY      XK_C
#define KEYBIND_PASTE     XK_V
#define KEYBIND_ZOOM_IN   XK_equal
#define KEYBIND_ZOOM_OUT  XK_minus
#define KEYBIND_ZOOM_RST  XK_0

#endif