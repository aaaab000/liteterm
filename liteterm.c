/*
 * LiteTerm — ультралёгкий X11-терминал
 * ~1200 строк, ~2-4 МБ RAM, темная тема
 *
 * Зависимости: Xlib, Xft, fontconfig
 * Сборка: make
 *
 * Фичи:
 *   - Полная эмуляция VT220/xterm (достаточная для bash, vim, htop, mc)
 *   - 256 цветов + TrueColor (SGR)
 *   - Прокрутка (scrollback)
 *   - Выделение мышью + copy/paste
 *   - Zoom шрифта (Ctrl+Shift+Plus/Minus)
 *   - UTF-8
 *   - Минимальное потребление ресурсов
 */

#define _XOPEN_SOURCE 600
#define _POSIX_C_SOURCE 200112L

#include <stdint.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/Xft/Xft.h>
#include <X11/keysym.h>
#include <X11/cursorfont.h>
#include <X11/XKBlib.h>

#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/wait.h>

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <locale.h>
#include <wchar.h>
#include <pty.h>
#include <utmp.h>
#include <termios.h>
#include <ctype.h>

#include "config.h"

/* ===================== Типы ===================== */

/* Атрибуты одного символа */
typedef struct {
    uint32_t fg;           /* цвет текста (RGB) */
    uint32_t bg;           /* цвет фона (RGB) */
    uint8_t  bold    : 1;
    uint8_t  italic  : 1;
    uint8_t  underline : 1;
    uint8_t  reverse : 1;
    uint8_t  dim     : 1;
    uint8_t  strike  : 1;
    uint8_t  blink   : 1;
} CellAttr;

/* Одна ячейка */
typedef struct {
    uint32_t  ch;          /* Unicode codepoint */
    CellAttr  attr;
    uint8_t   dirty : 1;
    uint8_t   width : 2;   /* 0=empty, 1=normal, 2=wide char first half */
} Cell;

/* Строка */
typedef struct {
    Cell *cells;
    int   len;              /* == cols */
} Line;

/* Состояние курсора (для сохранения/восстановления) */
typedef struct {
    int x, y;
    CellAttr attr;
} CursorState;

/* Состояние ESC-парсера */
enum {
    ESC_NONE = 0,
    ESC_START,              /* получили ESC */
    ESC_CSI,                /* ESC [ ... */
    ESC_OSC,                /* ESC ] ... */
    ESC_CHARSET,            /* ESC ( или ESC ) */
    ESC_HASH,               /* ESC # */
    ESC_TITLE,              /* внутри OSC, читаем строку */
};

/* ==================== Глобальные ==================== */

static Display    *dpy;
static int         scr;
static Window      win;
static Visual     *vis;
static Colormap    cmap;
static XftDraw    *xftdraw;
static XftFont    *font_normal;
static XftFont    *font_bold;
static GC          gc;
static Atom        wm_delete_msg;
static Atom        clipboard_atom, utf8_atom, targets_atom;

/* PTY */
static int         master_fd = -1;
static pid_t       child_pid = -1;

/* Размеры */
static int         cols, rows;
static int         cw, ch;         /* размер символа в пикселях */
static int         win_w, win_h;   /* размер окна */
static int         font_ascent;

/* Буфер терминала */
static Line       *screen_buf;     /* rows строк — видимая область */
static Line       *scroll_buf;     /* scrollback */
static int         scroll_size;    /* сколько строк в scrollback */
static int         scroll_pos;     /* текущая позиция прокрутки (0=внизу) */

/* Курсор */
static int         cur_x, cur_y;
static CellAttr    cur_attr;
static int         cursor_visible = 1;
static CursorState saved_cursor;

/* Парсер */
static int         esc_state;
static int         esc_params[16];
static int         esc_nparam;
static int         esc_priv;        /* '?' prefix */
static char        esc_intermediate;
static char        title_buf[256];
static int         title_len;

/* Области прокрутки */
static int         scroll_top, scroll_bot;

/* Режимы */
static int         mode_wrap = 1;
static int         mode_insert = 0;
static int         mode_appkeypad = 0;
static int         mode_appcursor = 0;
static int         mode_altscreen = 0;
static int         mode_bracketpaste = 0;
static int         mode_origin = 0;

/* Альтернативный буфер экрана */
static Line       *alt_buf;
static int         alt_cur_x, alt_cur_y;
static CellAttr    alt_cur_attr;

/* Выделение мышью */
static int         sel_active = 0;
static int         sel_x1, sel_y1, sel_x2, sel_y2;
static int         sel_dragging = 0;
static char       *sel_text = NULL;

/* Палитра 16 цветов */
static uint32_t    palette[256];

/* Флаг перерисовки */
static int         dirty = 1;
static int         running = 1;

/* Zoom */
static int         font_size_delta = 0;

/* ==================== Утилиты ==================== */

static XftColor xft_color_from_rgb(uint32_t rgb) {
    XftColor c;
    XRenderColor rc;
    rc.red   = ((rgb >> 16) & 0xFF) * 257;
    rc.green = ((rgb >> 8)  & 0xFF) * 257;
    rc.blue  = (rgb & 0xFF) * 257;
    rc.alpha = 0xFFFF;
    XftColorAllocValue(dpy, vis, cmap, &rc, &c);
    return c;
}

static void init_palette(void) {
    /* 16 базовых */
    palette[0]  = COLOR_0;  palette[1]  = COLOR_1;
    palette[2]  = COLOR_2;  palette[3]  = COLOR_3;
    palette[4]  = COLOR_4;  palette[5]  = COLOR_5;
    palette[6]  = COLOR_6;  palette[7]  = COLOR_7;
    palette[8]  = COLOR_8;  palette[9]  = COLOR_9;
    palette[10] = COLOR_10; palette[11] = COLOR_11;
    palette[12] = COLOR_12; palette[13] = COLOR_13;
    palette[14] = COLOR_14; palette[15] = COLOR_15;

    /* 216 цветов куба 6x6x6 */
    for (int i = 0; i < 216; i++) {
        int r = (i / 36) % 6;
        int g = (i / 6) % 6;
        int b = i % 6;
        r = r ? (r * 40 + 55) : 0;
        g = g ? (g * 40 + 55) : 0;
        b = b ? (b * 40 + 55) : 0;
        palette[16 + i] = (r << 16) | (g << 8) | b;
    }

    /* 24 градации серого */
    for (int i = 0; i < 24; i++) {
        int v = i * 10 + 8;
        palette[232 + i] = (v << 16) | (v << 8) | v;
    }
}

/* ==================== Буфер строк ==================== */

static Line line_alloc(int ncols) {
    Line l;
    l.len = ncols;
    l.cells = calloc(ncols, sizeof(Cell));
    for (int i = 0; i < ncols; i++) {
        l.cells[i].ch = ' ';
        l.cells[i].attr.fg = COLOR_FG;
        l.cells[i].attr.bg = COLOR_BG;
        l.cells[i].width = 1;
        l.cells[i].dirty = 1;
    }
    return l;
}

static void line_free(Line *l) {
    free(l->cells);
    l->cells = NULL;
    l->len = 0;
}

static void line_clear(Line *l, CellAttr attr) {
    for (int i = 0; i < l->len; i++) {
        l->cells[i].ch = ' ';
        l->cells[i].attr = attr;
        l->cells[i].attr.fg = COLOR_FG;
        l->cells[i].attr.bg = COLOR_BG;
        l->cells[i].width = 1;
        l->cells[i].dirty = 1;
    }
}

/* ==================== Управление экраном ==================== */

static void screen_alloc(void) {
    screen_buf = malloc(rows * sizeof(Line));
    for (int i = 0; i < rows; i++)
        screen_buf[i] = line_alloc(cols);

    scroll_buf = malloc(SCROLLBACK * sizeof(Line));
    scroll_size = 0;
    for (int i = 0; i < SCROLLBACK; i++)
        scroll_buf[i].cells = NULL;

    alt_buf = malloc(rows * sizeof(Line));
    for (int i = 0; i < rows; i++)
        alt_buf[i] = line_alloc(cols);
}

static void screen_free(void) {
    for (int i = 0; i < rows; i++) {
        line_free(&screen_buf[i]);
        line_free(&alt_buf[i]);
    }
    free(screen_buf);
    free(alt_buf);

    for (int i = 0; i < SCROLLBACK; i++) {
        if (scroll_buf[i].cells)
            line_free(&scroll_buf[i]);
    }
    free(scroll_buf);
}

static void scroll_up(int top, int bot) {
    /* Сохраняем верхнюю строку в scrollback */
    if (top == 0 && scroll_pos == 0) {
        if (scroll_size < SCROLLBACK) {
            /* Сдвигаем scrollback вниз */
            if (scroll_buf[SCROLLBACK - 1].cells)
                line_free(&scroll_buf[SCROLLBACK - 1]);
            memmove(&scroll_buf[1], &scroll_buf[0],
                    (SCROLLBACK - 1) * sizeof(Line));
            scroll_buf[0] = screen_buf[top];
            screen_buf[top] = line_alloc(cols);
            if (scroll_size < SCROLLBACK) scroll_size++;
        } else {
            if (scroll_buf[SCROLLBACK - 1].cells)
                line_free(&scroll_buf[SCROLLBACK - 1]);
            memmove(&scroll_buf[1], &scroll_buf[0],
                    (SCROLLBACK - 1) * sizeof(Line));
            scroll_buf[0] = screen_buf[top];
            screen_buf[top] = line_alloc(cols);
        }
    } else {
        line_free(&screen_buf[top]);
        screen_buf[top] = line_alloc(cols);
    }

    /* Сдвигаем строки вверх */
    Line tmp = screen_buf[top];
    for (int i = top; i < bot; i++)
        screen_buf[i] = screen_buf[i + 1];
    screen_buf[bot] = tmp;

    /* Очищаем нижнюю строку */
    line_clear(&screen_buf[bot], cur_attr);
    dirty = 1;
}

static void scroll_down(int top, int bot) {
    line_free(&screen_buf[bot]);
    screen_buf[bot] = line_alloc(cols);

    Line tmp = screen_buf[bot];
    for (int i = bot; i > top; i--)
        screen_buf[i] = screen_buf[i - 1];
    screen_buf[top] = tmp;
    line_clear(&screen_buf[top], cur_attr);
    dirty = 1;
}

static void mark_all_dirty(void) {
    for (int y = 0; y < rows; y++)
        for (int x = 0; x < cols; x++)
            screen_buf[y].cells[x].dirty = 1;
    dirty = 1;
}

/* ==================== Шрифт ==================== */

static void load_fonts(void) {
    char fname[256], fbold[256];
    snprintf(fname, sizeof(fname), "monospace:size=%d",
             11 + font_size_delta);
    snprintf(fbold, sizeof(fbold), "monospace:bold:size=%d",
             11 + font_size_delta);

    if (font_normal) XftFontClose(dpy, font_normal);
    if (font_bold) XftFontClose(dpy, font_bold);

    font_normal = XftFontOpenName(dpy, scr, fname);
    font_bold   = XftFontOpenName(dpy, scr, fbold);

    if (!font_normal) {
        fprintf(stderr, "liteterm: cannot open font\n");
        exit(1);
    }
    if (!font_bold) font_bold = font_normal;

    /* Вычисляем размер символа */
    XGlyphInfo ext;
    XftTextExtents8(dpy, font_normal, (FcChar8 *)"M", 1, &ext);
    cw = ext.xOff;
    ch = font_normal->ascent + font_normal->descent;
    font_ascent = font_normal->ascent;

    if (cw < 1) cw = 8;
    if (ch < 1) ch = 16;
}

/* ==================== Шелл/PTY ==================== */

static void pty_resize(void) {
    struct winsize ws;
    ws.ws_col = cols;
    ws.ws_row = rows;
    ws.ws_xpixel = win_w;
    ws.ws_ypixel = win_h;
    if (master_fd >= 0)
        ioctl(master_fd, TIOCSWINSZ, &ws);
}

static void spawn_shell(void) {
    child_pid = forkpty(&master_fd, NULL, NULL, NULL);
    if (child_pid < 0) {
        perror("forkpty");
        exit(1);
    }
    if (child_pid == 0) {
        /* Дочерний */
        setenv("TERM", "xterm-256color", 1);
        setenv("COLORTERM", "truecolor", 1);

        char *shell = getenv("SHELL");
        if (!shell || !*shell) shell = SHELL;

        char *args[] = { shell, NULL };
        execvp(shell, args);
        perror("execvp");
        _exit(1);
    }

    /* Родитель */
    int flags = fcntl(master_fd, F_GETFL);
    fcntl(master_fd, F_SETFL, flags | O_NONBLOCK);

    pty_resize();
}

static void pty_write(const char *s, int len) {
    if (master_fd < 0) return;
    while (len > 0) {
        int n = write(master_fd, s, len);
        if (n < 0) {
            if (errno == EAGAIN || errno == EINTR) continue;
            break;
        }
        s += n;
        len -= n;
    }
}

/* ==================== ESC/CSI парсер ==================== */

static void reset_attr(void) {
    memset(&cur_attr, 0, sizeof(cur_attr));
    cur_attr.fg = COLOR_FG;
    cur_attr.bg = COLOR_BG;
}

static void cursor_clamp(void) {
    if (cur_x < 0) cur_x = 0;
    if (cur_x >= cols) cur_x = cols - 1;
    if (cur_y < 0) cur_y = 0;
    if (cur_y >= rows) cur_y = rows - 1;
    dirty = 1;  // <--- ДОБАВИТЬ
}

static void newline(void) {
    if (cur_y == scroll_bot) {
        scroll_up(scroll_top, scroll_bot);
    } else if (cur_y < rows - 1) {
        cur_y++;
    }
}

static void put_char(uint32_t ch) {
    if (cur_x >= cols) {
        if (mode_wrap) {
            cur_x = 0;
            newline();
        } else {
            cur_x = cols - 1;
        }
    }

    if (mode_insert) {
        /* Сдвигаем вправо */
        for (int i = cols - 1; i > cur_x; i--)
            screen_buf[cur_y].cells[i] = screen_buf[cur_y].cells[i - 1];
    }

    Cell *cell = &screen_buf[cur_y].cells[cur_x];
    cell->ch = ch;
    cell->attr = cur_attr;
    cell->dirty = 1;
    cell->width = 1;

    cur_x++;
    dirty = 1;
}

/* Обработка SGR (Select Graphic Rendition) */
static void csi_sgr(void) {
    if (esc_nparam == 0) {
        reset_attr();
        return;
    }

    for (int i = 0; i < esc_nparam; i++) {
        int p = esc_params[i];

        if (p == 0) {
            reset_attr();
        } else if (p == 1) {
            cur_attr.bold = 1;
        } else if (p == 2) {
            cur_attr.dim = 1;
        } else if (p == 3) {
            cur_attr.italic = 1;
        } else if (p == 4) {
            cur_attr.underline = 1;
        } else if (p == 5) {
            cur_attr.blink = 1;
        } else if (p == 7) {
            cur_attr.reverse = 1;
        } else if (p == 9) {
            cur_attr.strike = 1;
        } else if (p == 22) {
            cur_attr.bold = 0; cur_attr.dim = 0;
        } else if (p == 23) {
            cur_attr.italic = 0;
        } else if (p == 24) {
            cur_attr.underline = 0;
        } else if (p == 27) {
            cur_attr.reverse = 0;
        } else if (p == 29) {
            cur_attr.strike = 0;
        } else if (p >= 30 && p <= 37) {
            cur_attr.fg = palette[p - 30];
        } else if (p == 38) {
            /* 256-color or truecolor foreground */
            if (i + 1 < esc_nparam && esc_params[i + 1] == 5 && i + 2 < esc_nparam) {
                cur_attr.fg = palette[esc_params[i + 2] & 0xFF];
                i += 2;
            } else if (i + 1 < esc_nparam && esc_params[i + 1] == 2 && i + 4 < esc_nparam) {
                cur_attr.fg = (esc_params[i + 2] << 16) |
                              (esc_params[i + 3] << 8) |
                               esc_params[i + 4];
                i += 4;
            }
        } else if (p == 39) {
            cur_attr.fg = COLOR_FG;
        } else if (p >= 40 && p <= 47) {
            cur_attr.bg = palette[p - 40];
        } else if (p == 48) {
            if (i + 1 < esc_nparam && esc_params[i + 1] == 5 && i + 2 < esc_nparam) {
                cur_attr.bg = palette[esc_params[i + 2] & 0xFF];
                i += 2;
            } else if (i + 1 < esc_nparam && esc_params[i + 1] == 2 && i + 4 < esc_nparam) {
                cur_attr.bg = (esc_params[i + 2] << 16) |
                              (esc_params[i + 3] << 8) |
                               esc_params[i + 4];
                i += 4;
            }
        } else if (p == 49) {
            cur_attr.bg = COLOR_BG;
        } else if (p >= 90 && p <= 97) {
            cur_attr.fg = palette[p - 90 + 8];
        } else if (p >= 100 && p <= 107) {
            cur_attr.bg = palette[p - 100 + 8];
        }
    }
}

static void csi_dispatch(char c) {
    int p0 = esc_nparam > 0 ? esc_params[0] : 0;
    int p1 = esc_nparam > 1 ? esc_params[1] : 0;

    switch (c) {
    case 'A':
		cur_y -= (p0 ? p0 : 1);
		cursor_clamp();
		dirty = 1;    // <--- ДОБАВИТЬ
		break;
	case 'B':
	case 'e':
		cur_y += (p0 ? p0 : 1);
		cursor_clamp();
		dirty = 1;    // <--- ДОБАВИТЬ
		break;
	case 'C':
	case 'a':
		cur_x += (p0 ? p0 : 1);
		cursor_clamp();
		dirty = 1;    // <--- ДОБАВИТЬ
		break;
	case 'D':
		cur_x -= (p0 ? p0 : 1);
		cursor_clamp();
		dirty = 1;    // <--- ДОБАВИТЬ
		break;
    case 'E': /* CNL */
        cur_x = 0;
        cur_y += (p0 ? p0 : 1);
        cursor_clamp();
        break;
    case 'F': /* CPL */
        cur_x = 0;
        cur_y -= (p0 ? p0 : 1);
        cursor_clamp();
        break;
    case 'G': /* CHA */
    case '`':
        cur_x = (p0 ? p0 - 1 : 0);
        cursor_clamp();
        break;
    case 'H': /* CUP */
    case 'f':
        cur_y = (p0 ? p0 - 1 : 0);
        cur_x = (p1 ? p1 - 1 : 0);
        if (mode_origin) cur_y += scroll_top;
        cursor_clamp();
        break;
    case 'J': /* ED — erase display */
        if (p0 == 0) {
            /* Erase from cursor to end */
            for (int x = cur_x; x < cols; x++) {
                screen_buf[cur_y].cells[x].ch = ' ';
                screen_buf[cur_y].cells[x].attr = cur_attr;
                screen_buf[cur_y].cells[x].dirty = 1;
            }
            for (int y = cur_y + 1; y < rows; y++)
                line_clear(&screen_buf[y], cur_attr);
        } else if (p0 == 1) {
            /* Erase from start to cursor */
            for (int y = 0; y < cur_y; y++)
                line_clear(&screen_buf[y], cur_attr);
            for (int x = 0; x <= cur_x && x < cols; x++) {
                screen_buf[cur_y].cells[x].ch = ' ';
                screen_buf[cur_y].cells[x].attr = cur_attr;
                screen_buf[cur_y].cells[x].dirty = 1;
            }
        } else if (p0 == 2 || p0 == 3) {
            for (int y = 0; y < rows; y++)
                line_clear(&screen_buf[y], cur_attr);
        }
        dirty = 1;
        break;
    case 'K': /* EL — erase in line */
        if (p0 == 0) {
            for (int x = cur_x; x < cols; x++) {
                screen_buf[cur_y].cells[x].ch = ' ';
                screen_buf[cur_y].cells[x].attr = cur_attr;
                screen_buf[cur_y].cells[x].dirty = 1;
            }
        } else if (p0 == 1) {
            for (int x = 0; x <= cur_x && x < cols; x++) {
                screen_buf[cur_y].cells[x].ch = ' ';
                screen_buf[cur_y].cells[x].attr = cur_attr;
                screen_buf[cur_y].cells[x].dirty = 1;
            }
        } else if (p0 == 2) {
            line_clear(&screen_buf[cur_y], cur_attr);
        }
        dirty = 1;
        break;
    case 'L': { /* IL — insert lines */
        int n = p0 ? p0 : 1;
        for (int i = 0; i < n && cur_y <= scroll_bot; i++)
            scroll_down(cur_y, scroll_bot);
        break;
    }
    case 'M': { /* DL — delete lines */
        int n = p0 ? p0 : 1;
        for (int i = 0; i < n && cur_y <= scroll_bot; i++)
            scroll_up(cur_y, scroll_bot);
        break;
    }
    case 'P': { /* DCH — delete characters */
        int n = p0 ? p0 : 1;
        if (n > cols - cur_x) n = cols - cur_x;
        memmove(&screen_buf[cur_y].cells[cur_x],
                &screen_buf[cur_y].cells[cur_x + n],
                (cols - cur_x - n) * sizeof(Cell));
        for (int i = cols - n; i < cols; i++) {
            screen_buf[cur_y].cells[i].ch = ' ';
            screen_buf[cur_y].cells[i].attr = cur_attr;
            screen_buf[cur_y].cells[i].dirty = 1;
        }
        dirty = 1;
        break;
    }
    case '@': { /* ICH — insert characters */
        int n = p0 ? p0 : 1;
        if (n > cols - cur_x) n = cols - cur_x;
        memmove(&screen_buf[cur_y].cells[cur_x + n],
                &screen_buf[cur_y].cells[cur_x],
                (cols - cur_x - n) * sizeof(Cell));
        for (int i = cur_x; i < cur_x + n; i++) {
            screen_buf[cur_y].cells[i].ch = ' ';
            screen_buf[cur_y].cells[i].attr = cur_attr;
            screen_buf[cur_y].cells[i].dirty = 1;
        }
        dirty = 1;
        break;
    }
    case 'X': { /* ECH — erase characters */
        int n = p0 ? p0 : 1;
        for (int i = 0; i < n && cur_x + i < cols; i++) {
            screen_buf[cur_y].cells[cur_x + i].ch = ' ';
            screen_buf[cur_y].cells[cur_x + i].attr = cur_attr;
            screen_buf[cur_y].cells[cur_x + i].dirty = 1;
        }
        dirty = 1;
        break;
    }
    case 'S': { /* SU — scroll up */
        int n = p0 ? p0 : 1;
        for (int i = 0; i < n; i++)
            scroll_up(scroll_top, scroll_bot);
        break;
    }
    case 'T': { /* SD — scroll down */
        int n = p0 ? p0 : 1;
        for (int i = 0; i < n; i++)
            scroll_down(scroll_top, scroll_bot);
        break;
    }
    case 'd': /* VPA — vertical position absolute */
        cur_y = (p0 ? p0 - 1 : 0);
        cursor_clamp();
        break;
    case 'm': /* SGR */
        csi_sgr();
        break;
    case 'n': /* DSR — device status report */
        if (p0 == 6) {
            char buf[32];
            snprintf(buf, sizeof(buf), "\033[%d;%dR", cur_y + 1, cur_x + 1);
            pty_write(buf, strlen(buf));
        } else if (p0 == 5) {
            pty_write("\033[0n", 4);
        }
        break;
    case 'r': /* DECSTBM — set scrolling region */
        scroll_top = p0 ? p0 - 1 : 0;
        scroll_bot = p1 ? p1 - 1 : rows - 1;
        if (scroll_top >= rows) scroll_top = rows - 1;
        if (scroll_bot >= rows) scroll_bot = rows - 1;
        if (scroll_top > scroll_bot) {
            int t = scroll_top; scroll_top = scroll_bot; scroll_bot = t;
        }
        cur_x = 0;
        cur_y = mode_origin ? scroll_top : 0;
        break;
    case 's': /* SCP — save cursor */
        saved_cursor.x = cur_x;
        saved_cursor.y = cur_y;
        saved_cursor.attr = cur_attr;
        break;
    case 'u': /* RCP — restore cursor */
        cur_x = saved_cursor.x;
        cur_y = saved_cursor.y;
        cur_attr = saved_cursor.attr;
        cursor_clamp();
        break;
    case 'c': /* DA — device attributes */
        if (esc_priv) break;
        pty_write("\033[?6c", 5); /* VT102 */
        break;
    case 'h': /* SM — set mode */
        for (int i = 0; i < esc_nparam; i++) {
            int m = esc_params[i];
            if (esc_priv) {
                switch (m) {
                case 1:    mode_appcursor = 1; break;
                case 7:    mode_wrap = 1; break;
                case 25:   cursor_visible = 1; break;
                case 47: case 1047: case 1049:
                    if (!mode_altscreen) {
                        mode_altscreen = 1;
                        /* Сохраняем основной буфер */
                        for (int y = 0; y < rows; y++) {
                            Line tmp = alt_buf[y];
                            alt_buf[y] = screen_buf[y];
                            screen_buf[y] = tmp;
                        }
                        alt_cur_x = cur_x;
                        alt_cur_y = cur_y;
                        alt_cur_attr = cur_attr;
                        for (int y = 0; y < rows; y++)
                            line_clear(&screen_buf[y], cur_attr);
                        cur_x = cur_y = 0;
                        mark_all_dirty();
                    }
                    break;
                case 2004: mode_bracketpaste = 1; break;
                case 6:    mode_origin = 1; break;
                }
            } else {
                if (m == 4) mode_insert = 1;
            }
        }
        break;
    case 'l': /* RM — reset mode */
        for (int i = 0; i < esc_nparam; i++) {
            int m = esc_params[i];
            if (esc_priv) {
                switch (m) {
                case 1:    mode_appcursor = 0; break;
                case 7:    mode_wrap = 0; break;
                case 25:   cursor_visible = 0; break;
                case 47: case 1047: case 1049:
                    if (mode_altscreen) {
                        mode_altscreen = 0;
                        for (int y = 0; y < rows; y++) {
                            Line tmp = alt_buf[y];
                            alt_buf[y] = screen_buf[y];
                            screen_buf[y] = tmp;
                        }
                        cur_x = alt_cur_x;
                        cur_y = alt_cur_y;
                        cur_attr = alt_cur_attr;
                        mark_all_dirty();
                    }
                    break;
                case 2004: mode_bracketpaste = 0; break;
                case 6:    mode_origin = 0; break;
                }
            } else {
                if (m == 4) mode_insert = 0;
            }
        }
        break;
    case 't': /* Window manipulation — ignore most */
        break;
    case 'q': /* DECSCUSR — cursor style, ignore */
        break;
    }
}

static void esc_dispatch(char c) {
    switch (c) {
    case 'c': /* RIS — full reset */
        reset_attr();
        cur_x = cur_y = 0;
        scroll_top = 0;
        scroll_bot = rows - 1;
        mode_wrap = 1;
        mode_insert = 0;
        mode_appkeypad = 0;
        mode_appcursor = 0;
        mode_origin = 0;
        for (int y = 0; y < rows; y++)
            line_clear(&screen_buf[y], cur_attr);
        dirty = 1;
        break;
    case 'D': /* IND — index (move down, scroll if needed) */
        if (cur_y == scroll_bot)
            scroll_up(scroll_top, scroll_bot);
        else
            cur_y++;
        break;
    case 'E': /* NEL — next line */
        cur_x = 0;
        if (cur_y == scroll_bot)
            scroll_up(scroll_top, scroll_bot);
        else
            cur_y++;
        break;
    case 'M': /* RI — reverse index */
        if (cur_y == scroll_top)
            scroll_down(scroll_top, scroll_bot);
        else
            cur_y--;
        break;
    case '7': /* DECSC — save cursor */
        saved_cursor.x = cur_x;
        saved_cursor.y = cur_y;
        saved_cursor.attr = cur_attr;
        break;
    case '8': /* DECRC — restore cursor */
        cur_x = saved_cursor.x;
        cur_y = saved_cursor.y;
        cur_attr = saved_cursor.attr;
        cursor_clamp();
        break;
    case '=':
        mode_appkeypad = 1;
        break;
    case '>':
        mode_appkeypad = 0;
        break;
    }
}

static void process_byte(uint8_t b) {
    /* UTF-8 декодирование (стейт-машина) */
    static uint32_t utf8_cp = 0;
    static int utf8_remain = 0;

    if (esc_state == ESC_OSC || esc_state == ESC_TITLE) {
        if (b == 0x07 || (b == '\\' && esc_state == ESC_TITLE)) {
            /* End of OSC */
            title_buf[title_len] = '\0';
            /* Ставим заголовок окна */
            if (title_len > 0) {
                XStoreName(dpy, win, title_buf);
                XChangeProperty(dpy, win,
                    XInternAtom(dpy, "_NET_WM_NAME", False),
                    XInternAtom(dpy, "UTF8_STRING", False),
                    8, PropModeReplace,
                    (unsigned char *)title_buf, title_len);
            }
            esc_state = ESC_NONE;
            return;
        }
        if (b == 0x1B) {
            esc_state = ESC_TITLE; /* ожидаем \ */
            return;
        }
        if (esc_state == ESC_OSC) {
            /* Пропускаем числовой параметр и ; */
            if (b == ';') {
                esc_state = ESC_TITLE;
                title_len = 0;
                return;
            }
            return;
        }
        /* ESC_TITLE — собираем символы */
        if (title_len < (int)sizeof(title_buf) - 1)
            title_buf[title_len++] = b;
        return;
    }

    if (esc_state == ESC_CHARSET) {
        esc_state = ESC_NONE;
        return;
    }

    if (esc_state == ESC_HASH) {
        esc_state = ESC_NONE;
        return;
    }

    if (esc_state == ESC_CSI) {
        if (b == '?') {
            esc_priv = 1;
            return;
        }
        if (b == '>') {
            esc_priv = 2;
            return;
        }
        if (b >= '0' && b <= '9') {
            if (esc_nparam == 0) esc_nparam = 1;
            esc_params[esc_nparam - 1] = esc_params[esc_nparam - 1] * 10 + (b - '0');
            return;
        }
        if (b == ';') {
            if (esc_nparam < 16) esc_nparam++;
            esc_params[esc_nparam - 1] = 0;
            return;
        }
        if (b == ':') {
            /* Subparameter separator, treat like ';' for compatibility */
            if (esc_nparam < 16) esc_nparam++;
            esc_params[esc_nparam - 1] = 0;
            return;
        }
        if (b >= 0x20 && b <= 0x2F) {
            esc_intermediate = b;
            return;
        }
        if (b >= 0x40 && b <= 0x7E) {
            csi_dispatch(b);
            esc_state = ESC_NONE;
            return;
        }
        esc_state = ESC_NONE;
        return;
    }

    if (esc_state == ESC_START) {
        esc_state = ESC_NONE;
        switch (b) {
        case '[':
            esc_state = ESC_CSI;
            memset(esc_params, 0, sizeof(esc_params));
            esc_nparam = 0;
            esc_priv = 0;
            esc_intermediate = 0;
            return;
        case ']':
            esc_state = ESC_OSC;
            title_len = 0;
            return;
        case '(':
        case ')':
            esc_state = ESC_CHARSET;
            return;
        case '#':
            esc_state = ESC_HASH;
            return;
        default:
            esc_dispatch(b);
            return;
        }
    }

    /* Обычные символы */
    if (b == 0x1B) {
        esc_state = ESC_START;
        utf8_remain = 0;
        return;
    }

    /* Управляющие символы */
	if (b < 0x20 || b == 0x7F) {
		switch (b) {
		case '\r': 
			cur_x = 0; 
			dirty = 1;
			return;
		case '\n': case '\v': case '\f':
			newline();
			dirty = 1;
			return;
		case '\t': {
			int next = (cur_x + 8) & ~7;
			if (next >= cols) next = cols - 1;
			cur_x = next;
			dirty = 1;
			return;
		}
		case '\b':
			if (cur_x > 0) cur_x--;
			dirty = 1;           // <--- ВОТ ЭТО ИСПРАВЛЕНИЕ
			return;
		case '\a': /* bell */
			return;
		case 0x0E: case 0x0F: /* SI/SO */
			return;
		}
		return;
	}

    /* UTF-8 декодирование */
    uint32_t ch;
    if (utf8_remain > 0) {
        if ((b & 0xC0) == 0x80) {
            utf8_cp = (utf8_cp << 6) | (b & 0x3F);
            utf8_remain--;
            if (utf8_remain > 0) return;
            ch = utf8_cp;
        } else {
            /* Ошибка UTF-8 — пропускаем */
            utf8_remain = 0;
            ch = b;
        }
    } else if ((b & 0x80) == 0) {
        ch = b;
    } else if ((b & 0xE0) == 0xC0) {
        utf8_cp = b & 0x1F;
        utf8_remain = 1;
        return;
    } else if ((b & 0xF0) == 0xE0) {
        utf8_cp = b & 0x0F;
        utf8_remain = 2;
        return;
    } else if ((b & 0xF8) == 0xF0) {
        utf8_cp = b & 0x07;
        utf8_remain = 3;
        return;
    } else {
        ch = b;
    }

    put_char(ch);
}

/* ==================== Отрисовка ==================== */

static void draw(void) {
    if (!dirty) return;

    /* Рисуем фон целиком */
    XftColor bg_c = xft_color_from_rgb(COLOR_BG);
    XftDrawRect(xftdraw, &bg_c, 0, 0, win_w, win_h);
    XftColorFree(dpy, vis, cmap, &bg_c);

    for (int y = 0; y < rows; y++) {
        for (int x = 0; x < cols; x++) {
            Cell *cell = &screen_buf[y].cells[x];

            int px = INTERNAL_PAD + x * cw;
            int py = INTERNAL_PAD + y * ch;

            uint32_t fg_rgb = cell->attr.fg;
            uint32_t bg_rgb = cell->attr.bg;

            if (cell->attr.reverse) {
                uint32_t tmp = fg_rgb;
                fg_rgb = bg_rgb;
                bg_rgb = tmp;
            }

            if (cell->attr.dim) {
                fg_rgb = ((fg_rgb >> 1) & 0x7F7F7F);
            }

            /* Выделение */
            if (sel_active) {
                int in_sel = 0;
                int s1 = sel_y1 * cols + sel_x1;
                int s2 = sel_y2 * cols + sel_x2;
                if (s1 > s2) { int t = s1; s1 = s2; s2 = t; }
                int pos = y * cols + x;
                if (pos >= s1 && pos <= s2) in_sel = 1;
                if (in_sel) {
                    bg_rgb = COLOR_SEL_BG;
                    fg_rgb = COLOR_SEL_FG;
                }
            }

            /* Рисуем фон ячейки если не стандартный */
            if (bg_rgb != COLOR_BG) {
                XftColor c = xft_color_from_rgb(bg_rgb);
                XftDrawRect(xftdraw, &c, px, py, cw, ch);
                XftColorFree(dpy, vis, cmap, &c);
            }

            /* Рисуем символ */
            if (cell->ch != ' ' && cell->ch != 0) {
                XftColor fc = xft_color_from_rgb(fg_rgb);
                XftFont *f = cell->attr.bold ? font_bold : font_normal;

                /* Конвертируем codepoint в FcChar32 */
                FcChar32 ucs = cell->ch;
                XftDrawString32(xftdraw, &fc, f, px, py + font_ascent,
                                &ucs, 1);
                XftColorFree(dpy, vis, cmap, &fc);
            }

            /* Подчёркивание */
            if (cell->attr.underline) {
                XftColor fc = xft_color_from_rgb(fg_rgb);
                XftDrawRect(xftdraw, &fc, px, py + ch - 1, cw, 1);
                XftColorFree(dpy, vis, cmap, &fc);
            }

            /* Зачёркивание */
            if (cell->attr.strike) {
                XftColor fc = xft_color_from_rgb(fg_rgb);
                XftDrawRect(xftdraw, &fc, px, py + ch / 2, cw, 1);
                XftColorFree(dpy, vis, cmap, &fc);
            }
        }
    }

    /* Курсор */
    if (cursor_visible && scroll_pos == 0) {
        int cx = INTERNAL_PAD + cur_x * cw;
        int cy = INTERNAL_PAD + cur_y * ch;
        XftColor cc = xft_color_from_rgb(COLOR_CURSOR);

        /* Блочный курсор — полупрозрачный блок */
        XftDrawRect(xftdraw, &cc, cx, cy, cw, ch);

        /* Перерисовываем символ под курсором инвертированным цветом */
        if (cur_x < cols && cur_y < rows) {
            Cell *cell = &screen_buf[cur_y].cells[cur_x];
            if (cell->ch != ' ' && cell->ch != 0) {
                XftColor inv = xft_color_from_rgb(COLOR_BG);
                XftFont *f = cell->attr.bold ? font_bold : font_normal;
                FcChar32 ucs = cell->ch;
                XftDrawString32(xftdraw, &inv, f, cx, cy + font_ascent,
                                &ucs, 1);
                XftColorFree(dpy, vis, cmap, &inv);
            }
        }

        XftColorFree(dpy, vis, cmap, &cc);
    }

    dirty = 0;
}

/* ==================== Выделение ==================== */

static char *selection_get_text(void) {
    int s1 = sel_y1 * cols + sel_x1;
    int s2 = sel_y2 * cols + sel_x2;
    if (s1 > s2) { int t = s1; s1 = s2; s2 = t; }

    int len = 0;
    int maxlen = (s2 - s1 + 1) * 4 + rows * 2;
    char *buf = malloc(maxlen + 1);

    int sy1 = s1 / cols, sx1_start = s1 % cols;
    int sy2 = s2 / cols, sx2_end = s2 % cols;

    for (int y = sy1; y <= sy2 && y < rows; y++) {
        int x_start = (y == sy1) ? sx1_start : 0;
        int x_end = (y == sy2) ? sx2_end : cols - 1;

        /* Убираем trailing пробелы */
        while (x_end >= x_start &&
               screen_buf[y].cells[x_end].ch == ' ')
            x_end--;

        for (int x = x_start; x <= x_end && x < cols; x++) {
            uint32_t ch = screen_buf[y].cells[x].ch;
            if (ch == 0) ch = ' ';

            /* Простейшее UTF-8 кодирование */
            if (ch < 0x80) {
                buf[len++] = ch;
            } else if (ch < 0x800) {
                buf[len++] = 0xC0 | (ch >> 6);
                buf[len++] = 0x80 | (ch & 0x3F);
            } else if (ch < 0x10000) {
                buf[len++] = 0xE0 | (ch >> 12);
                buf[len++] = 0x80 | ((ch >> 6) & 0x3F);
                buf[len++] = 0x80 | (ch & 0x3F);
            } else {
                buf[len++] = 0xF0 | (ch >> 18);
                buf[len++] = 0x80 | ((ch >> 12) & 0x3F);
                buf[len++] = 0x80 | ((ch >> 6) & 0x3F);
                buf[len++] = 0x80 | (ch & 0x3F);
            }
        }

        if (y < sy2) buf[len++] = '\n';
    }

    buf[len] = '\0';
    return buf;
}

static void selection_copy(void) {
    if (!sel_active) return;
    if (sel_text) free(sel_text);
    sel_text = selection_get_text();
    XSetSelectionOwner(dpy, clipboard_atom, win, CurrentTime);
    XSetSelectionOwner(dpy, XA_PRIMARY, win, CurrentTime);
}

static void selection_paste(void) {
    XConvertSelection(dpy, clipboard_atom, utf8_atom,
                      clipboard_atom, win, CurrentTime);
}

// Добавить функцию очистки выделения (после selection_paste)
static void selection_clear(void) {
    if (sel_active) {
        sel_active = 0;
        sel_dragging = 0;
        dirty = 1;
    }
}

/* ==================== Ввод клавиатуры ==================== */

// ==================== Ввод клавиатуры ====================

static void handle_key(XKeyEvent *ev) {
    KeySym ksym = NoSymbol;
    char buf[32];
    int len;

    len = XLookupString(ev, buf, sizeof(buf) - 1, &ksym, NULL);

    unsigned int state = ev->state & (ControlMask | ShiftMask | Mod1Mask);

    if (state == (ControlMask | ShiftMask)) {
        if (ksym == KEYBIND_COPY) {
            selection_copy();
            return;  // <-- выделение остаётся
        }
        if (ksym == KEYBIND_PASTE) {
            selection_clear();  // <-- снимаем при вставке
            selection_paste();
            return;
        }
        if (ksym == KEYBIND_ZOOM_IN) {
            font_size_delta++;
            load_fonts();
            cols = (win_w - 2 * INTERNAL_PAD) / cw;
            rows = (win_h - 2 * INTERNAL_PAD) / ch;
            if (cols < 2) cols = 2;
            if (rows < 2) rows = 2;
            mark_all_dirty();
            pty_resize();
            return;
        }
        if (ksym == KEYBIND_ZOOM_OUT) {
            if (font_size_delta > -5) font_size_delta--;
            load_fonts();
            cols = (win_w - 2 * INTERNAL_PAD) / cw;
            rows = (win_h - 2 * INTERNAL_PAD) / ch;
            if (cols < 2) cols = 2;
            if (rows < 2) rows = 2;
            mark_all_dirty();
            pty_resize();
            return;
        }
        if (ksym == KEYBIND_ZOOM_RST) {
            font_size_delta = 0;
            load_fonts();
            cols = (win_w - 2 * INTERNAL_PAD) / cw;
            rows = (win_h - 2 * INTERNAL_PAD) / ch;
            if (cols < 2) cols = 2;
            if (rows < 2) rows = 2;
            mark_all_dirty();
            pty_resize();
            return;
        }
    }

    switch (ksym) {
    case XK_Shift_L: case XK_Shift_R:
    case XK_Control_L: case XK_Control_R:
    case XK_Alt_L: case XK_Alt_R:
    case XK_Super_L: case XK_Super_R:
    case XK_Meta_L: case XK_Meta_R:
    case XK_Caps_Lock: case XK_Num_Lock:
    case XK_Scroll_Lock:
        break;
    default:
        selection_clear();
        break;
    }
    // ===========================================

    // Вычисляем модификатор xterm-стиля
    int mod = 0;
    if (ev->state & ShiftMask)   mod |= 1;
    if (ev->state & Mod1Mask)    mod |= 2;
    if (ev->state & ControlMask) mod |= 4;

    // Специальные клавиши
    const char *seq = NULL;
    char seqbuf[32];
    int is_special = 0;  // <-- флаг: клавиша обработана как специальная

    switch (ksym) {
    case XK_Up:
        if (mod) {
            snprintf(seqbuf, sizeof(seqbuf), "\033[1;%dA", mod + 1);
            seq = seqbuf;
        } else {
            seq = mode_appcursor ? "\033OA" : "\033[A";
        }
        is_special = 1;
        break;
    case XK_Down:
        if (mod) {
            snprintf(seqbuf, sizeof(seqbuf), "\033[1;%dB", mod + 1);
            seq = seqbuf;
        } else {
            seq = mode_appcursor ? "\033OB" : "\033[B";
        }
        is_special = 1;
        break;
    case XK_Right:
        if (mod) {
            snprintf(seqbuf, sizeof(seqbuf), "\033[1;%dC", mod + 1);
            seq = seqbuf;
        } else {
            seq = mode_appcursor ? "\033OC" : "\033[C";
        }
        is_special = 1;
        break;
    case XK_Left:
        if (mod) {
            snprintf(seqbuf, sizeof(seqbuf), "\033[1;%dD", mod + 1);
            seq = seqbuf;
        } else {
            seq = mode_appcursor ? "\033OD" : "\033[D";
        }
        is_special = 1;
        break;
    case XK_Home:
        if (mod) {
            snprintf(seqbuf, sizeof(seqbuf), "\033[1;%dH", mod + 1);
            seq = seqbuf;
        } else {
            seq = mode_appcursor ? "\033OH" : "\033[H";
        }
        is_special = 1;
        break;
    case XK_End:
        if (mod) {
            snprintf(seqbuf, sizeof(seqbuf), "\033[1;%dF", mod + 1);
            seq = seqbuf;
        } else {
            seq = mode_appcursor ? "\033OF" : "\033[F";
        }
        is_special = 1;
        break;
    case XK_Insert:
        if (mod) {
            snprintf(seqbuf, sizeof(seqbuf), "\033[2;%d~", mod + 1);
            seq = seqbuf;
        } else {
            seq = "\033[2~";
        }
        is_special = 1;
        break;
    case XK_Delete:
        if (mod) {
            snprintf(seqbuf, sizeof(seqbuf), "\033[3;%d~", mod + 1);
            seq = seqbuf;
        } else {
            seq = "\033[3~";
        }
        is_special = 1;
        break;
    case XK_Page_Up:
        if (ev->state & ShiftMask && !(ev->state & ControlMask)) {
            scroll_pos += rows / 2;
            if (scroll_pos > scroll_size) scroll_pos = scroll_size;
            mark_all_dirty();
            return;
        }
        if (mod) {
            snprintf(seqbuf, sizeof(seqbuf), "\033[5;%d~", mod + 1);
            seq = seqbuf;
        } else {
            seq = "\033[5~";
        }
        is_special = 1;
        break;
    case XK_Page_Down:
        if (ev->state & ShiftMask && !(ev->state & ControlMask)) {
            scroll_pos -= rows / 2;
            if (scroll_pos < 0) scroll_pos = 0;
            mark_all_dirty();
            return;
        }
        if (mod) {
            snprintf(seqbuf, sizeof(seqbuf), "\033[6;%d~", mod + 1);
            seq = seqbuf;
        } else {
            seq = "\033[6~";
        }
        is_special = 1;
        break;
    case XK_F1:
        if (mod) { snprintf(seqbuf, sizeof(seqbuf), "\033[1;%dP", mod + 1); seq = seqbuf; }
        else seq = "\033OP";
        is_special = 1;
        break;
    case XK_F2:
        if (mod) { snprintf(seqbuf, sizeof(seqbuf), "\033[1;%dQ", mod + 1); seq = seqbuf; }
        else seq = "\033OQ";
        is_special = 1;
        break;
    case XK_F3:
        if (mod) { snprintf(seqbuf, sizeof(seqbuf), "\033[1;%dR", mod + 1); seq = seqbuf; }
        else seq = "\033OR";
        is_special = 1;
        break;
    case XK_F4:
        if (mod) { snprintf(seqbuf, sizeof(seqbuf), "\033[1;%dS", mod + 1); seq = seqbuf; }
        else seq = "\033OS";
        is_special = 1;
        break;
    case XK_F5:
        if (mod) { snprintf(seqbuf, sizeof(seqbuf), "\033[15;%d~", mod + 1); seq = seqbuf; }
        else seq = "\033[15~";
        is_special = 1;
        break;
    case XK_F6:
        if (mod) { snprintf(seqbuf, sizeof(seqbuf), "\033[17;%d~", mod + 1); seq = seqbuf; }
        else seq = "\033[17~";
        is_special = 1;
        break;
    case XK_F7:
        if (mod) { snprintf(seqbuf, sizeof(seqbuf), "\033[18;%d~", mod + 1); seq = seqbuf; }
        else seq = "\033[18~";
        is_special = 1;
        break;
    case XK_F8:
        if (mod) { snprintf(seqbuf, sizeof(seqbuf), "\033[19;%d~", mod + 1); seq = seqbuf; }
        else seq = "\033[19~";
        is_special = 1;
        break;
    case XK_F9:
        if (mod) { snprintf(seqbuf, sizeof(seqbuf), "\033[20;%d~", mod + 1); seq = seqbuf; }
        else seq = "\033[20~";
        is_special = 1;
        break;
    case XK_F10:
        if (mod) { snprintf(seqbuf, sizeof(seqbuf), "\033[21;%d~", mod + 1); seq = seqbuf; }
        else seq = "\033[21~";
        is_special = 1;
        break;
    case XK_F11:
        if (mod) { snprintf(seqbuf, sizeof(seqbuf), "\033[23;%d~", mod + 1); seq = seqbuf; }
        else seq = "\033[23~";
        is_special = 1;
        break;
    case XK_F12:
        if (mod) { snprintf(seqbuf, sizeof(seqbuf), "\033[24;%d~", mod + 1); seq = seqbuf; }
        else seq = "\033[24~";
        is_special = 1;
        break;
    case XK_BackSpace:
        if (ev->state & Mod1Mask) {
            seq = "\033\177";
        } else {
            seq = "\177";
        }
        is_special = 1;
        break;
    case XK_Return:
    case XK_KP_Enter:
        seq = mode_appkeypad ? "\033OM" : "\r";
        is_special = 1;
        break;
    case XK_Tab:
        if (ev->state & ShiftMask) {
            seq = "\033[Z";
        } else {
            seq = "\t";
        }
        is_special = 1;
        break;
    case XK_Escape:
        seq = "\033";
        is_special = 1;
        break;
    // Игнорируем клавиши-модификаторы
    case XK_Shift_L: case XK_Shift_R:
    case XK_Control_L: case XK_Control_R:
    case XK_Alt_L: case XK_Alt_R:
    case XK_Super_L: case XK_Super_R:
    case XK_Meta_L: case XK_Meta_R:
    case XK_Caps_Lock: case XK_Num_Lock:
    case XK_Scroll_Lock:
        return;
    }

    if (seq) {
        scroll_pos = 0;
        pty_write(seq, strlen(seq));
        return;
    }

    // Если клавиша была обработана как специальная но seq == NULL
    // (не должно случиться, но на всякий случай)
    if (is_special)
        return;

    // Обычные символы — используем результат XLookupString полученный ранее
    if (len > 0) {
        if (ev->state & Mod1Mask) {
            pty_write("\033", 1);
        }
        scroll_pos = 0;
        pty_write(buf, len);
    }
}

/* ==================== Мышь ==================== */

static void handle_mouse_press(XButtonEvent *ev) {
    int mx = (ev->x - INTERNAL_PAD) / cw;
    int my = (ev->y - INTERNAL_PAD) / ch;
    if (mx < 0) mx = 0;
    if (my < 0) my = 0;
    if (mx >= cols) mx = cols - 1;
    if (my >= rows) my = rows - 1;

    if (ev->button == Button1) {
        sel_active = 1;
        sel_x1 = sel_x2 = mx;
        sel_y1 = sel_y2 = my;
        sel_dragging = 1;
        dirty = 1;
    } else if (ev->button == Button4) {
        /* Scroll up */
        scroll_pos += 3;
        if (scroll_pos > scroll_size) scroll_pos = scroll_size;
        mark_all_dirty();
    } else if (ev->button == Button5) {
        /* Scroll down */
        scroll_pos -= 3;
        if (scroll_pos < 0) scroll_pos = 0;
        mark_all_dirty();
    }
}

static void handle_mouse_motion(XMotionEvent *ev) {
    if (!sel_dragging) return;

    int mx = (ev->x - INTERNAL_PAD) / cw;
    int my = (ev->y - INTERNAL_PAD) / ch;
    if (mx < 0) mx = 0;
    if (my < 0) my = 0;
    if (mx >= cols) mx = cols - 1;
    if (my >= rows) my = rows - 1;

    sel_x2 = mx;
    sel_y2 = my;
    dirty = 1;
}

static void handle_mouse_release(XButtonEvent *ev) {
    if (ev->button == Button1 && sel_dragging) {
        sel_dragging = 0;
        /* Авто-копирование в PRIMARY */
        if (sel_text) free(sel_text);
        sel_text = selection_get_text();
        XSetSelectionOwner(dpy, XA_PRIMARY, win, CurrentTime);
    }
}

/* ==================== Обработка X-событий ==================== */

static void handle_selrequest(XSelectionRequestEvent *ev) {
    XSelectionEvent se;
    memset(&se, 0, sizeof(se));
    se.type = SelectionNotify;
    se.requestor = ev->requestor;
    se.selection = ev->selection;
    se.target = ev->target;
    se.time = ev->time;
    se.property = None;

    if (sel_text && ev->target == utf8_atom) {
        XChangeProperty(dpy, ev->requestor, ev->property,
                        utf8_atom, 8, PropModeReplace,
                        (unsigned char *)sel_text, strlen(sel_text));
        se.property = ev->property;
    } else if (ev->target == targets_atom) {
        Atom targets[] = { utf8_atom, XA_STRING, targets_atom };
        XChangeProperty(dpy, ev->requestor, ev->property,
                        XA_ATOM, 32, PropModeReplace,
                        (unsigned char *)targets, 3);
        se.property = ev->property;
    } else if (sel_text && ev->target == XA_STRING) {
        XChangeProperty(dpy, ev->requestor, ev->property,
                        XA_STRING, 8, PropModeReplace,
                        (unsigned char *)sel_text, strlen(sel_text));
        se.property = ev->property;
    }

    XSendEvent(dpy, ev->requestor, False, 0, (XEvent *)&se);
}

static void handle_selnotify(XSelectionEvent *ev) {
    if (ev->property == None) return;

    Atom type;
    int format;
    unsigned long nitems, after;
    unsigned char *data = NULL;

    XGetWindowProperty(dpy, win, ev->property, 0, 65536, True,
                       AnyPropertyType, &type, &format,
                       &nitems, &after, &data);

    if (data && nitems > 0) {
        if (mode_bracketpaste)
            pty_write("\033[200~", 6);
        pty_write((char *)data, nitems);
        if (mode_bracketpaste)
            pty_write("\033[201~", 6);
    }

    if (data) XFree(data);
}

static void handle_configure(XConfigureEvent *ev) {
    if (ev->width == win_w && ev->height == win_h) return;

    win_w = ev->width;
    win_h = ev->height;

    int new_cols = (win_w - 2 * INTERNAL_PAD) / cw;
    int new_rows = (win_h - 2 * INTERNAL_PAD) / ch;
    if (new_cols < 2) new_cols = 2;
    if (new_rows < 2) new_rows = 2;

    if (new_cols == cols && new_rows == rows) return;

    /* Перевыделяем буферы */
    Line *new_screen = malloc(new_rows * sizeof(Line));
    Line *new_alt = malloc(new_rows * sizeof(Line));

    for (int i = 0; i < new_rows; i++) {
        new_screen[i] = line_alloc(new_cols);
        new_alt[i] = line_alloc(new_cols);
    }

    /* Копируем старый контент */
    int copy_rows = rows < new_rows ? rows : new_rows;
    int copy_cols = cols < new_cols ? cols : new_cols;

    for (int y = 0; y < copy_rows; y++) {
        for (int x = 0; x < copy_cols; x++) {
            new_screen[y].cells[x] = screen_buf[y].cells[x];
            new_alt[y].cells[x] = alt_buf[y].cells[x];
        }
    }

    /* Освобождаем старые */
    for (int i = 0; i < rows; i++) {
        line_free(&screen_buf[i]);
        line_free(&alt_buf[i]);
    }
    free(screen_buf);
    free(alt_buf);

    screen_buf = new_screen;
    alt_buf = new_alt;
    cols = new_cols;
    rows = new_rows;

    scroll_top = 0;
    scroll_bot = rows - 1;

    if (cur_x >= cols) cur_x = cols - 1;
    if (cur_y >= rows) cur_y = rows - 1;

    /* Пересоздаём XftDraw */
    if (xftdraw) XftDrawDestroy(xftdraw);
    xftdraw = XftDrawCreate(dpy, win, vis, cmap);

    pty_resize();
    mark_all_dirty();
}

/* ==================== Main loop ==================== */

static void sigchld(int sig) {
    (void)sig;
    int status;
    pid_t p = waitpid(child_pid, &status, WNOHANG);
    if (p == child_pid) {
        running = 0;
    }
}

int main(int argc, char *argv[]) {
    (void)argc; (void)argv;

    setlocale(LC_ALL, "");
    signal(SIGCHLD, sigchld);

    /* Открываем дисплей */
    dpy = XOpenDisplay(NULL);
    if (!dpy) {
        fprintf(stderr, "liteterm: cannot open display\n");
        return 1;
    }

    scr = DefaultScreen(dpy);
    vis = DefaultVisual(dpy, scr);
    cmap = DefaultColormap(dpy, scr);

    init_palette();

    /* Загружаем шрифты */
    font_normal = font_bold = NULL;
    load_fonts();

    /* Вычисляем начальные размеры */
    cols = DEFAULT_COLS;
    rows = DEFAULT_ROWS;
    win_w = cols * cw + 2 * INTERNAL_PAD;
    win_h = rows * ch + 2 * INTERNAL_PAD;

    /* Создаём окно */
    XSetWindowAttributes swa;
    swa.background_pixel = COLOR_BG;
    swa.event_mask = ExposureMask | KeyPressMask | StructureNotifyMask |
                     ButtonPressMask | ButtonReleaseMask | PointerMotionMask |
                     FocusChangeMask;

    win = XCreateWindow(dpy, RootWindow(dpy, scr),
                        0, 0, win_w, win_h, 0,
                        CopyFromParent, InputOutput, vis,
                        CWBackPixel | CWEventMask, &swa);

    /* WM hints */
    XClassHint class_hint;
    class_hint.res_name = "liteterm";
    class_hint.res_class = "LiteTerm";
    XSetClassHint(dpy, win, &class_hint);

    XStoreName(dpy, win, "LiteTerm");

    /* Size hints — для корректного ресайза кратно символам */
    XSizeHints *sh = XAllocSizeHints();
    sh->flags = PMinSize | PResizeInc | PBaseSize;
    sh->min_width = cw * 4 + 2 * INTERNAL_PAD;
    sh->min_height = ch * 2 + 2 * INTERNAL_PAD;
    sh->width_inc = cw;
    sh->height_inc = ch;
    sh->base_width = 2 * INTERNAL_PAD;
    sh->base_height = 2 * INTERNAL_PAD;
    XSetWMNormalHints(dpy, win, sh);
    XFree(sh);

    /* WM_DELETE_WINDOW */
    wm_delete_msg = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(dpy, win, &wm_delete_msg, 1);

    /* Clipboard atoms */
    clipboard_atom = XInternAtom(dpy, "CLIPBOARD", False);
    utf8_atom = XInternAtom(dpy, "UTF8_STRING", False);
    targets_atom = XInternAtom(dpy, "TARGETS", False);

    /* Курсор */
    Cursor cursor = XCreateFontCursor(dpy, XC_xterm);
    XDefineCursor(dpy, win, cursor);

    /* GC */
    gc = XCreateGC(dpy, win, 0, NULL);

    /* XftDraw */
    xftdraw = XftDrawCreate(dpy, win, vis, cmap);

    /* Выделяем буферы */
    screen_alloc();

    /* Инициализация состояния */
    cur_x = cur_y = 0;
    reset_attr();
    scroll_top = 0;
    scroll_bot = rows - 1;
    scroll_pos = 0;
    esc_state = ESC_NONE;

    /* Маппим окно */
    XMapWindow(dpy, win);
    XSync(dpy, False);

    /* Запускаем шелл */
    spawn_shell();

    printf("liteterm: started (%dx%d cells, %dx%d px)\n",
           cols, rows, win_w, win_h);

    /* Event loop */
    int xfd = ConnectionNumber(dpy);

    while (running) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(xfd, &rfds);
        if (master_fd >= 0) FD_SET(master_fd, &rfds);

        int maxfd = xfd > master_fd ? xfd : master_fd;

        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = dirty ? 16000 : 50000; /* 60fps если грязный, 20fps idle */

        int ret = select(maxfd + 1, &rfds, NULL, NULL, &tv);

        /* Читаем с PTY */
        if (ret > 0 && master_fd >= 0 && FD_ISSET(master_fd, &rfds)) {
            char buf[4096];
            int n = read(master_fd, buf, sizeof(buf));
            if (n > 0) {
                for (int i = 0; i < n; i++)
                    process_byte((uint8_t)buf[i]);
            } else if (n == 0 || (n < 0 && errno != EAGAIN && errno != EINTR)) {
                running = 0;
            }
        }

        /* X события */
        while (XPending(dpy)) {
            XEvent ev;
            XNextEvent(dpy, &ev);

            switch (ev.type) {
            case Expose:
                if (ev.xexpose.count == 0)
                    mark_all_dirty();
                break;
            case KeyPress:
                handle_key(&ev.xkey);
                break;
            case ButtonPress:
                handle_mouse_press(&ev.xbutton);
                break;
            case ButtonRelease:
                handle_mouse_release(&ev.xbutton);
                break;
            case MotionNotify:
                handle_mouse_motion(&ev.xmotion);
                break;
            case ConfigureNotify:
                handle_configure(&ev.xconfigure);
                break;
            case SelectionRequest:
                handle_selrequest(&ev.xselectionrequest);
                break;
            case SelectionNotify:
                handle_selnotify(&ev.xselection);
                break;
            case FocusIn:
            case FocusOut:
                dirty = 1;
                break;
            case ClientMessage:
                if ((Atom)ev.xclient.data.l[0] == wm_delete_msg)
                    running = 0;
                break;
            }
        }

        /* Отрисовка */
        draw();
        XFlush(dpy);
    }

    /* Cleanup */
    if (master_fd >= 0) close(master_fd);
    if (sel_text) free(sel_text);
    screen_free();
    XftDrawDestroy(xftdraw);
    if (font_bold != font_normal) XftFontClose(dpy, font_bold);
    XftFontClose(dpy, font_normal);
    XFreeGC(dpy, gc);
    XFreeCursor(dpy, cursor);
    XDestroyWindow(dpy, win);
    XCloseDisplay(dpy);

    printf("liteterm: bye\n");
    return 0;
}
