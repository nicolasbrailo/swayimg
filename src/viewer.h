// SPDX-License-Identifier: MIT
// Business logic of application and UI event handlers.
// Copyright (C) 2020 Artem Senichev <artemsen@gmail.com>

#pragma once

#include "keybind.h"
#include "pixmap.h"

// Configuration parameters
#define VIEWER_CFG_SLIDESHOW      "slideshow"
#define VIEWER_CFG_SLIDESHOW_TIME "slideshow_time"
#define VIEWER_CFG_INFO_TIMEOUT       "image_info_timeout"
#define VIEWER_DISPLAY_SYSTEM_CMD     "show_system_cmd"
#define VIEWER_DISPLAY_SYSTEM_CMD_POS "show_system_cmd_pos"

/**
 * Initialize viewer context.
 */
void viewer_init(void);

/**
 * Free viewer context.
 */
void viewer_free(void);

/**
 * Reset state: reload image file, set initial scale etc.
 */
void viewer_reset(void);

/**
 * Redraw handler.
 * @param window pixel map of window
 */
void viewer_on_redraw(struct pixmap* window);

/**
 * Window resize handler.
 * @param width,height new window size
 * @param scale window scale factor
 */
void viewer_on_resize(size_t width, size_t height, size_t scale);

/**
 * Key press handler.
 * @param key code of key pressed
 * @param mods key modifires (ctrl/alt/shift)
 */
void viewer_on_keyboard(xkb_keysym_t key, uint8_t mods);

/**
 * Image drap handler.
 * @param dx,dy delta to move viewpoint
 */
void viewer_on_drag(int dx, int dy);
