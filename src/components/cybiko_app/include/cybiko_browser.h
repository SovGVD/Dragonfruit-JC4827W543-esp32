#pragma once
/* The web browser in Cybiko mode. The cybiko-browser project
 * (src/components/cy_browser, a git submodule) has the browser itself and
 * the Browser launcher for the Cybiko desktop; this is the glue: it puts
 * the launcher on the RAM disk and, when the launcher asks for it
 * (CY_APP_BROWSER), pauses CyOS and runs the browser on the Cybiko screen
 * until the user closes it. */
#include <stdbool.h>
#include <stdint.h>
#include "cybiko.h"
#include "cy_browser.h"

/* Puts Browser.app on the RAM disk unless it is there already, as built. */
void cybiko_browser_install(uint8_t *disk, uint32_t size);

/* The host's app_call hook (cy_host_t). */
uint32_t cybiko_browser_app_call(void *user, uint32_t fn, uint32_t arg);
/* True once the launcher asked for the browser (and clears it). */
bool cybiko_browser_requested(void);

/* Runs the browser until the user closes it, on the CPU task (CyOS does not
 * run meanwhile). next_key gives the keys pressed since the last call
 * (false when there are none); flash is the CyOS flash, for its fonts. */
void cybiko_browser_run(cybiko_t *m, const uint8_t *flash,
                        bool (*next_key)(int *key, bool *down));

/* A Cybiko key (CYK_*) as the browser's (CYB_K_* or a character), 0 if it
 * has none. */
static inline int cybiko_browser_key(int cyk)
{
    static const struct { int16_t cyk, key; } map[] = {
        { CYK_UP, CYB_K_UP }, { CYK_DOWN, CYB_K_DOWN }, { CYK_LEFT, CYB_K_LEFT }, { CYK_RIGHT, CYB_K_RIGHT },
        { CYK_ENTER, CYB_K_ENTER }, { CYK_ESC, CYB_K_ESC }, { CYK_TAB, CYB_K_TAB }, { CYK_DEL, CYB_K_DEL },
        { CYK_SELECT, CYB_K_SELECT }, { CYK_MENU, CYB_K_MENU }, { CYK_HELP, CYB_K_HELP },
        { CYK_SHIFT, CYB_K_SHIFT }, { CYK_FN, CYB_K_FN },
        { CYK_F1, CYB_K_F1 }, { CYK_F2, CYB_K_F2 }, { CYK_F3, CYB_K_F3 }, { CYK_F4, CYB_K_F4 },
        { CYK_F5, CYB_K_F5 }, { CYK_F6, CYB_K_F6 }, { CYK_F7, CYB_K_F7 },
        { CYK_SPACE, ' ' }, { CYK_DOT, '.' }, { CYK_COMMA, ',' }, { CYK_SEMI, ';' }, { CYK_EXCL, '!' },
        { CYK_LPAREN, '(' }, { CYK_RPAREN, ')' },
        { CYK_A, 'a' }, { CYK_B, 'b' }, { CYK_C, 'c' }, { CYK_D, 'd' }, { CYK_E, 'e' }, { CYK_F, 'f' },
        { CYK_G, 'g' }, { CYK_H, 'h' }, { CYK_I, 'i' }, { CYK_J, 'j' }, { CYK_K, 'k' }, { CYK_L, 'l' },
        { CYK_M, 'm' }, { CYK_N, 'n' }, { CYK_O, 'o' }, { CYK_P, 'p' }, { CYK_Q, 'q' }, { CYK_R, 'r' },
        { CYK_S, 's' }, { CYK_T, 't' }, { CYK_U, 'u' }, { CYK_V, 'v' }, { CYK_W, 'w' }, { CYK_X, 'x' },
        { CYK_Y, 'y' }, { CYK_Z, 'z' },
    };
    for (unsigned i = 0; i < sizeof map / sizeof map[0]; i++)
        if (map[i].cyk == cyk) return map[i].key;
    return 0;
}
