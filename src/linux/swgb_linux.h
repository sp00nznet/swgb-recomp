/* What the Linux and macOS host's files share (src/linux). */
#ifndef SWGB_LINUX_H
#define SWGB_LINUX_H

extern char swgb_cd_letter;   /* the play disc's drive letter, or 0 */
extern int  swgb_test_run;    /* --headless, --keys or --watchdog: boxes answer themselves */

/* shims.c: the game's own answers, registered ahead of win32hle's. */
void swgb_register_shims(void);

/* main.c: a file open the game made (--files, --keys on:NAME). */
void swgb_file_seen(const char* name, int ok);

#endif
