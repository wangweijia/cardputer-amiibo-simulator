#ifndef LV_CONF_H
#define LV_CONF_H

#define LV_COLOR_DEPTH 16
/* The default 64 KiB pool is too small for framebuffer and CJK glyph buffers. */
#define LV_USE_STDLIB_MALLOC LV_STDLIB_CLIB
#define LV_USE_LINUX_FBDEV 1
#define LV_USE_FREETYPE 1
#define LV_USE_SPINNER 1
#define LV_USE_LOG 1
#define LV_LOG_PRINTF 1
#define LV_LOG_LEVEL LV_LOG_LEVEL_ERROR

#endif
