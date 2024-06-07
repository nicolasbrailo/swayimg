// SPDX-License-Identifier: MIT
// List of images.
// Copyright (C) 2022 Artem Senichev <artemsen@gmail.com>

#include "imagelist.h"

#include "buildcfg.h"
#include "config.h"
#include "image.h"
#include "imagedownloader.h"
#include "imageprefetcher.h"
#include "str.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct image_list {
    char* www_url;
    char* www_cache_dir;
    struct downloader_ctx* downloader;
    struct image_prefetcher_ctx* image_prefetcher;
    size_t index;
    struct image* no_img;
    struct image* current;
    size_t image_cache_size; ///< Maximum number of images to cache
    size_t prefetch_n;       ///< Number of images to prefetch
    bool save_download_images_to_file;
    bool cleanup_downloaded_images_on_start_and_exit;
};

static struct image_list ctx = {
    .index = 0,
    .no_img = NULL,
    .current = NULL,
    .www_url = NULL,
    .www_cache_dir = NULL,
    .downloader = NULL,
    .image_prefetcher = NULL,
    .image_cache_size = 10,
    .prefetch_n = 3,
    .save_download_images_to_file = false,
    .cleanup_downloaded_images_on_start_and_exit = false,
};

/**
 * Custom section loader, see `config_loader` for details.
 */
static enum config_status load_config(const char* key, const char* value)
{
    if (strcmp(key, IMGLIST_SRC) == 0) {
        if (strcmp(value, IMGLIST_SRC_WWW) == 0) {
            return cfgst_ok;
        }
        return cfgst_invalid_value;
    }

    if (strcmp(key, IMGLIST_WWW_URL) == 0) {
        ctx.www_url = strdup(value);
        return cfgst_ok;
    }

    if (strcmp(key, IMGLIST_NO_IMAGE_ICON) == 0) {
        ctx.no_img = image_from_file(value);
        if (ctx.no_img) {
            return cfgst_ok;
        } else {
            fprintf(stderr, "Failed to load no-image asset at '%s'\n", value);
            return cfgst_invalid_value;
        }
    }

    if (strcmp(key, IMGLIST_WWW_CACHE) == 0) {
        ctx.www_cache_dir = strdup(value);
        return cfgst_ok;
    }

    if (strcmp(key, IMGLIST_WWW_CACHE_LIMIT) == 0) {
        long num = 0;
        if (str_to_num(value, 0, &num, 0) && num > 0) {
            ctx.image_cache_size = num;
            return cfgst_ok;
        }
        return cfgst_invalid_value;
    }

    if (strcmp(key, IMGLIST_WWW_PREFETCH_N) == 0) {
        long num = 0;
        if (str_to_num(value, 0, &num, 0) && num > 0) {
            ctx.prefetch_n = num;
            return cfgst_ok;
        }
        return cfgst_invalid_value;
    }

    if (strcmp(key, IMGLIST_WWW_SAVE_TO_FILE) == 0) {
        if (str_to_bool(value, 0, &ctx.save_download_images_to_file)) {
            return cfgst_ok;
        }
        return cfgst_invalid_value;
    }

    if (strcmp(key, IMGLIST_WWW_CLEANUP_CACHE) == 0) {
        if (str_to_bool(value, 0,
                        &ctx.cleanup_downloaded_images_on_start_and_exit)) {
            return cfgst_ok;
        }
        return cfgst_invalid_value;
    }

    return cfgst_invalid_key;
}

void image_list_init()
{
    // register configuration loader
    config_add_loader(IMGLIST_CFG_SECTION, load_config);
}

void image_list_free(void)
{
    image_prefetcher_free(ctx.image_prefetcher);
    downloader_free(ctx.downloader);
    image_free(ctx.no_img);
    free(ctx.www_url);
    free(ctx.www_cache_dir);
}

bool image_list_scan(const char** files, size_t num)
{
    (void) files;
    (void)num;

    ctx.downloader =
        downloader_init(ctx.www_url, ctx.www_cache_dir,
                        ctx.cleanup_downloaded_images_on_start_and_exit);
    if (!ctx.downloader) {
        return false;
    }

    ctx.image_prefetcher =
        image_prefetcher_init(downloader_get_one, ctx.downloader);
    if (!image_prefetcher_start(ctx.image_prefetcher, ctx.image_cache_size,
                                ctx.prefetch_n)) {
        return false;
    }

    if (ctx.no_img) {
        ctx.current = ctx.no_img;
    } else {
        while (image_prefetcher_get_cached(ctx.image_prefetcher) == 0) {
            // TODO timeout
        }
        ctx.current = image_prefetcher_jump_next(ctx.image_prefetcher);
    }

    return true;
}

size_t image_list_size(void)
{
    return -1;
}

struct image_entry image_list_current(void)
{
    struct image_entry entry = {
        .index = 42,
        .image = ctx.current,
    };
    return entry;
}

bool image_list_skip(void)
{
    return false;
}

bool image_list_reset(void)
{
    return false;
}

bool image_list_jump(enum list_jump jump)
{
    struct image* img = NULL;
    switch (jump) {
      case jump_first_file: // fallthrough
      case jump_last_file: // fallthrough
      case jump_next_dir: // fallthrough
      case jump_prev_dir: // fallthrough
          return false;
      case jump_next_file:
          img = image_prefetcher_jump_next(ctx.image_prefetcher);
          ctx.current = img ? img : ctx.no_img;
          return img != NULL;
      case jump_prev_file:
          img = image_prefetcher_jump_prev(ctx.image_prefetcher);
          ctx.current = img ? img : ctx.no_img;
          return img != NULL;
    }
    abort();
}
