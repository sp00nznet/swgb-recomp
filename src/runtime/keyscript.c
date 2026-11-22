/* keyscript: the --keys grammar (keyscript.h). Plain C, no Win32. */
#include "keyscript.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Windows' virtual-key codes, by value: this file builds where windows.h
 * does not exist. */
static const struct { const char* n; int vk; } k_names[] = {
    { "esc", 0x1B }, { "enter", 0x0D }, { "space", 0x20 }, { "tab", 0x09 },
    { "up", 0x26 }, { "down", 0x28 }, { "left", 0x25 }, { "right", 0x27 },
    { "bksp", 0x08 }, { "f1", 0x70 }, { "f2", 0x71 }, { "f3", 0x72 }, { "f4", 0x73 },
    { "f5", 0x74 }, { "f6", 0x75 }, { "f7", 0x76 }, { "f8", 0x77 }, { "f9", 0x78 }, { "f10", 0x79 },
};

static int same_nocase(const char* a, const char* b) {
    for (; *a && *b; a++, b++)
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return 0;
    return *a == *b;
}

int keyscript_key(const char* n) {
    for (size_t i = 0; i < sizeof k_names / sizeof k_names[0]; i++)
        if (same_nocase(n, k_names[i].n)) return k_names[i].vk;
    if (n[0] && !n[1] && isalnum((unsigned char)n[0])) return toupper((unsigned char)n[0]);
    return 0;
}

int keyscript_parse(const char* spec, keyscript_step* steps, int max, const char** bad) {
    static char buf[4096];
    int n = 0;
    snprintf(buf, sizeof buf, "%s", spec);
    for (char* tok = strtok(buf, ","); tok && n < max; tok = strtok(NULL, ",")) {
        keyscript_step st;
        memset(&st, 0, sizeof st);
        char* c = strchr(tok, ':');
        if (!c) continue;
        if (!strncmp(tok, "on:", 3)) {
            snprintf(st.on, sizeof st.on, "%s", tok + 3);
            steps[n++] = st;
            continue;
        }
        st.at_ms = (uint32_t)(atof(tok) * 1000);
        char* what = c + 1;
        if (!strncmp(what, "shot=", 5)) {
            snprintf(st.shot, sizeof st.shot, "%s", what + 5);
        } else if (!strncmp(what, "click:", 6) || !strncmp(what, "move:", 5)) {
            st.click = what[0] == 'c';
            st.move = 1;
            sscanf(strchr(what, ':') + 1, "%d:%d", &st.x, &st.y);
        } else if (!(st.vk = keyscript_key(what))) {
            if (bad) *bad = what;
            return -1;
        }
        steps[n++] = st;
    }
    return n;
}
