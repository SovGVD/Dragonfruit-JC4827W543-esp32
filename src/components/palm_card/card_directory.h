#pragma once

/* POSIX directory calls on ESP-IDF/Linux; a small Win32 adapter keeps the
 * existing MSVC host build usable without a new third-party dependency. */
#ifndef _WIN32
#include <dirent.h>
#else
#include <windows.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#ifndef S_ISDIR
#define S_ISDIR(mode) (((mode) & _S_IFMT) == _S_IFDIR)
#endif
#ifndef S_ISREG
#define S_ISREG(mode) (((mode) & _S_IFMT) == _S_IFREG)
#endif
struct dirent { char d_name[MAX_PATH]; };
typedef struct {
    HANDLE search;
    WIN32_FIND_DATAA data;
    struct dirent entry;
    bool first;
    char pattern[512];
} DIR;
static DIR *opendir(const char *path)
{
    DIR *d = calloc(1, sizeof *d);
    if (!d) return NULL;
    if (snprintf(d->pattern, sizeof d->pattern, "%s/*", path) >= sizeof d->pattern) {
        free(d); return NULL;
    }
    d->search = FindFirstFileA(d->pattern, &d->data);
    if (d->search == INVALID_HANDLE_VALUE) { free(d); return NULL; }
    d->first = true;
    return d;
}
static struct dirent *readdir(DIR *d)
{
    if (d->search == INVALID_HANDLE_VALUE) return NULL;
    if (!d->first && !FindNextFileA(d->search, &d->data)) return NULL;
    d->first = false;
    snprintf(d->entry.d_name, sizeof d->entry.d_name, "%s", d->data.cFileName);
    return &d->entry;
}
static void rewinddir(DIR *d)
{
    if (d->search != INVALID_HANDLE_VALUE) FindClose(d->search);
    d->search = FindFirstFileA(d->pattern, &d->data);
    d->first = true;
}
static int closedir(DIR *d)
{
    int result = d->search == INVALID_HANDLE_VALUE || FindClose(d->search) ? 0 : -1;
    free(d); return result;
}
#endif
