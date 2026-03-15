/*
 * Minimal Winamp General Purpose Plugin SDK header.
 * Based on the public Winamp SDK (gen.h).
 */

#ifndef WINAMP_GEN_H
#define WINAMP_GEN_H

#include <windows.h>

#define GPPHDR_VER 0x10

typedef struct {
    int version;              /* GPPHDR_VER */
    const char *description;  /* plugin description */
    int (*init)(void);        /* called on plugin init, return 0 on success */
    void (*config)(void);     /* configuration dialog */
    void (*quit)(void);       /* called on plugin unload */
    HWND hwndParent;          /* set by host: main window handle */
    HINSTANCE hDllInstance;   /* set by host: plugin DLL instance */
} winampGeneralPurposePlugin;

/* Export this function from your DLL */
typedef winampGeneralPurposePlugin* (*winampGeneralPurposePluginGetter)(void);

#endif /* WINAMP_GEN_H */
