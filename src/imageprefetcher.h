// SPDX-License-Identifier: MIT
//
// Prefetcher: will keep a list of images ready to be used, together with a
// history of the last N previously used images. It can be used to provide quick
// loading, but also a limited history to scroll back to.
//
// Copyright (C) 2022 Artem Senichev <artemsen@gmail.com>

#pragma once

#include <stdbool.h>
#include <stddef.h>

// fwd decls
struct image;
struct image_prefetcher_ctx;

/**
 * Callback to fetch an image in-memory. Caller takes ownership of memory.
 */
typedef struct image* (*downloader_cb)(void*);

/**
 * Prefetcher running state; the prefetcher will keep a list of images ready to
 * be used, together with a history of the last N previously used images. It can
 * be used to provide quick loading, but also a limited history to scroll back
 * to.
 */
struct image_prefetcher_ctx;

/**
 * Creates a prefetcher and returns a handle. Will return NULL if creating a
 * prefetcher failed
 */
struct image_prefetcher_ctx* image_prefetcher_init(downloader_cb cb,
                                                   void* downloader_impl_usr);

/**
 * Mark prefetcher as ready to start; this implies the downloader is also
 * configured and ready to be invoked. Returns true if the prefetcher is
 * running, false if it failed to start. It's possible to try to start a
 * prefetcher again if it fails.
 *
 * @param cache_size: maximum number of images to cache, before expiring old
 * ones
 * @param prefetch_n: number of images to prefetch ahead of latest requested by
 * user
 */
bool image_prefetcher_start(struct image_prefetcher_ctx* ctx, size_t cache_size,
                            size_t prefetch_n);

void image_prefetcher_free(struct image_prefetcher_ctx* ctx);

/**
 * Returns the number of currently cached and available images
 */
size_t image_prefetcher_get_cached(struct image_prefetcher_ctx* ctx);

/**
 * Return the next image in the cache.
 * If no image is avaialble in the cache (eg requesting beyond the prefetched
 * limit, before the background thread has had a chance to catch up) it will
 * return NULL. The ownership of the memory is retained by the prefetcher, the
 * caller shouldn't free this image.
 */
struct image* image_prefetcher_jump_next(struct image_prefetcher_ctx* ctx);

/**
 * Move back one image in the history. Returns null if no more images are
 * available (ie reached the first cached image).
 */
struct image* image_prefetcher_jump_prev(struct image_prefetcher_ctx* ctx);
