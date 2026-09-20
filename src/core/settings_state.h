#ifndef SETTINGS_STATE_H
#define SETTINGS_STATE_H

#include <stdint.h>

/* Global settings for Aurion OS */
typedef struct {
    uint32_t bg_color;         /* 0=Blue (default), 1=Green, 2=Red, 3=White, 4=Black */
    int window_style;          /* 0=MacOS (default), 1=Windows */
    int startup_app_idx;       /* -1=None; else index into desktop_apps[] */
    int dock_magnification;    /* 1=Enabled (default), 0=Disabled */
    int dock_transparent;      /* 1=Transparent (default), 0=Opaque */
    int resolution;            /* 0=1280x720, 1=1280x1024, 2=1440x900, 3=1600x900, 4=1920x1080 (def), 5=2560x1440 */
    int wallpaper;             /* 0=Night Ridge, 1=Misty Conifer (default), 2=Gate B14, 3=Sunday */
    int cursor_size;           /* 0=Small 1x (default), 1=Large 2x, 2=XLarge 3x */
} OSSettings;

/* The global settings instance */
extern OSSettings g_settings;

/* Predefined colors for background */
static const uint32_t g_bg_colors[] = {
    0xFF002D5A, /* Deep Midnight Blue (Default) */
    0xFF0A160A, /* Green */
    0xFF160A0A, /* Red */
    0xFFF0F0F0, /* White */
    0xFF000000  /* Pure Black */
};

#endif
