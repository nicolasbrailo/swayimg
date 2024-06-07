// SPDX-License-Identifier: MIT
//
// Prefetcher: will keep a list of images ready to be used, together with a
// history of the last N previously used images. It can be used to provide quick
// loading, but also a limited history to scroll back to.
//
// Copyright (C) 2022 Artem Senichev <artemsen@gmail.com>

#pragma once

#include <stdbool.h>

// fwd decls
struct image;
struct downloader_ctx;

struct downloader_ctx* downloader_init(const char* www_url,
                                       const char* www_cache_dir,
                                       bool clean_cache_after_use);
void downloader_free(struct downloader_ctx* ctx);
struct image* downloader_get_one(void* usr);
