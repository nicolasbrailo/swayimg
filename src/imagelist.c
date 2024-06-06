// SPDX-License-Identifier: MIT
// List of images.
// Copyright (C) 2022 Artem Senichev <artemsen@gmail.com>

#include "image.h"

#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/**
 * Callback to fetch an image in-memory. Caller takes ownership of memory.
 */
typedef struct image* (*downloader_cb)(void*);
// bg thread for the prefetcher
static void* image_prefetcher_thread(void* usr);

/**
 * Prefetcher running state; the prefetcher will keep a list of images ready to
 * be used, together with a history of the last N previously used images. It can
 * be used to provide quick loading, but also a limited history to scroll back
 * to.
 */
struct image_prefetcher_ctx {
    downloader_cb downloader_impl; ///< Callback to fetch a picture
    void* downloader_impl_usr;     ///< User data for callback

    pthread_t thread;       ///< Background thread to invoke the cb
    pthread_cond_t condvar; ///< Signal for the prefetcher thread to wakeup
    pthread_mutex_t cv_mut; ///< Protects shared state of image ringbuffer

    size_t image_cache_size;    ///< Maximum number of images to cache
    struct image** image_cache; ///< Image ringbuffer
    size_t image_cache_r;       ///< Ringbuffer read ptr
    size_t image_cache_w;       ///< Ringbuffer write ptr
    size_t prefetch_n;          ///< Number of images to prefetch
};

void image_prefetcher_init(struct image_prefetcher_ctx* ctx, downloader_cb cb,
                           void* downloader_impl_usr)
{
    ctx->downloader_impl = cb;
    ctx->downloader_impl_usr = downloader_impl_usr;

    ctx->image_cache_size = 0;
    ctx->image_cache_r = 0;
    ctx->image_cache_w = 0;
    ctx->prefetch_n = 0;
    ctx->image_cache = NULL;

    if (ctx->thread) {
        fprintf(stderr, "Double init of image prefetcher requested\n");
        abort();
    }

    if (pthread_cond_init(&ctx->condvar, NULL) != 0) {
        perror("pthread_cond_init");
        abort();
    }

    if (pthread_mutex_init(&ctx->cv_mut, NULL) != 0) {
        perror("pthread_mutex_init");
        abort();
    }
}

/**
 * Mark prefetcher as ready to start; this implies the downloader is also configured
 * and ready to be invoked.
 *
 * @param cache_size: maximum number of images to cache, before expiring old ones
 * @param prefetch_n: number of images to prefetch ahead of latest requested by user
 */
void image_prefetcher_start(struct image_prefetcher_ctx* ctx, size_t cache_size, size_t prefetch_n)
{
    ctx->image_cache_size = cache_size;
    ctx->prefetch_n = prefetch_n;

    if (ctx->image_cache_r != 0 || ctx->image_cache_w != 0) {
        fprintf(stderr, "BUG: image_prefetcher used before start\n");
        abort();
    }

    if (ctx->image_cache_size == 0) {
        fprintf(stderr, "Can't use prefetcher with cache size = 0\n");
        abort();
    }

    if (ctx->prefetch_n == 0) {
        fprintf(stderr, "Can't use prefetcher with prefetch count = 0\n");
        abort();
    }

    if (ctx->prefetch_n > ctx->image_cache_size) {
        fprintf(stderr,
                "Prefetcher has prefetch count = %lu and max cache size = %lu. "
                "Will set max prefetch to cache size.\n",
                ctx->prefetch_n, ctx->image_cache_size);
        ctx->prefetch_n = ctx->image_cache_size;
        abort();
    }

    ctx->image_cache = malloc(sizeof(void*) * ctx->image_cache_size);
    if (!ctx->image_cache) {
        fprintf(stderr, "Bad alloc, can't create prefetcher\n");
        abort();
    }

    memset(ctx->image_cache, 0, sizeof(void*) * ctx->image_cache_size);

    if (pthread_create(&ctx->thread, NULL, image_prefetcher_thread, ctx) != 0) {
        perror("pthread_create");
        abort();
    }
}

void image_prefetcher_free(struct image_prefetcher_ctx* ctx)
{
    if (!ctx) {
        return;
    }

    if (pthread_cancel(ctx->thread) != 0) {
        perror("pthread_cancel, threads may leak");
    }

    if (pthread_join(ctx->thread, NULL) != 0) {
        perror("pthread_join, threads may leak");
    }

    if (pthread_cond_destroy(&ctx->condvar) != 0) {
        perror("pthread_cond_destroy");
    }

    // May return an error if thread is destroyed while locked
    pthread_mutex_destroy(&ctx->cv_mut);

    for (size_t i = 0; i < ctx->image_cache_size; ++i) {
        image_free(ctx->image_cache[i]);
    }

    free(ctx->image_cache);
    ctx->thread = 0;
}

/** Image prefetching, background thread */
void* image_prefetcher_thread(void* usr)
{
    struct image_prefetcher_ctx* ctx = usr;

    while (true) {
        // Figure out how many images we need to prefetch. If the user requests
        // a new image while we're prefetching this count will be less than it
        // should be, but it will eventually catch up on the next wakeup (as
        // long as prefetch num is high enough).
        pthread_mutex_lock(&ctx->cv_mut);
        const size_t cnt = (ctx->image_cache_w >= ctx->image_cache_r)
            ? ctx->image_cache_w - ctx->image_cache_r
            : ctx->image_cache_w + ctx->image_cache_size - ctx->image_cache_r;
        pthread_mutex_unlock(&ctx->cv_mut);

        for (size_t i=cnt; i < ctx->prefetch_n; ++i) {
            pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);

            // Download one (may be slow)
            struct image* img = ctx->downloader_impl(ctx->downloader_impl_usr);

            // Critical section: update buffer
            pthread_mutex_lock(&ctx->cv_mut);
            struct image* old_img = ctx->image_cache[ctx->image_cache_w];
            size_t old_w_pos = ctx->image_cache_w;
            ctx->image_cache[ctx->image_cache_w] = img;
            ctx->image_cache_w =
                (ctx->image_cache_w + 1) % ctx->image_cache_size;
            pthread_mutex_unlock(&ctx->cv_mut);

            // If the old position in the ringbuffer had an image, relase it
            if (old_img) {
                printf("Expire cached image w_old=%lu [%p]\n", old_w_pos, (void*)old_img);
                image_free(old_img);
            }

            pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);
        }

        pthread_mutex_lock(&ctx->cv_mut);
        pthread_cond_wait(&ctx->condvar, &ctx->cv_mut);
        pthread_mutex_unlock(&ctx->cv_mut);
    }

    return NULL;
}

/**
 * Return the next image in the cache.
 * If no image is avaialble in the cache (eg requesting beyond the prefetched
 * limit, before the background thread has had a chance to catch up) it will
 * return NULL. The ownership of the memory is retained by the prefetcher, the
 * caller shouldn't free this image.
 */
static struct image*
image_prefetcher_jump_next(struct image_prefetcher_ctx* ctx)
{
    bool can_move = false;

    pthread_mutex_lock(&ctx->cv_mut);
    const size_t next_idx = (ctx->image_cache_r + 1) % ctx->image_cache_size;
    if (ctx->image_cache_w == ctx->image_cache_r) {
        printf("No more images available, cache empty.\n");
    } else if (!ctx->image_cache[next_idx]) {
        // This shouldn't happen
        fprintf(
            stderr,
            "BUG! Race reading next image: cache R=%lu=%p, N=%lu=%p, W=%lu\n",
            ctx->image_cache_r, (void*)ctx->image_cache[ctx->image_cache_r],
            next_idx, (void*)ctx->image_cache[next_idx], ctx->image_cache_w);
    } else {
        ctx->image_cache_r = next_idx;
        can_move = true;
    }
    pthread_mutex_unlock(&ctx->cv_mut);

    // Wake up the background thread (it may or may not need to download new
    // files)
    if (pthread_cond_broadcast(&ctx->condvar) != 0) {
        perror("pthread_cond_broadcast");
    }

    return can_move ? ctx->image_cache[next_idx] : NULL;
}

/**
 * Move back one image in the history. Returns null if no more images are
 * available (ie reached the first cached image).
 */
static struct image*
image_prefetcher_jump_prev(struct image_prefetcher_ctx* ctx)
{
    pthread_mutex_lock(&ctx->cv_mut);
    const size_t maybe_prev = ctx->image_cache_r == 0
        ? ctx->image_cache_size - 1
        : ctx->image_cache_r - 1;
    if (maybe_prev == ctx->image_cache_w || !ctx->image_cache[maybe_prev]) {
        printf("No more images available, reached oldest image in history.\n");
        pthread_mutex_unlock(&ctx->cv_mut);
        return NULL;
    }

    ctx->image_cache_r = maybe_prev;
    pthread_mutex_unlock(&ctx->cv_mut);

    // We don't signal the condvar because it should never be necessary to
    // prefetch a new image
    return ctx->image_cache[maybe_prev];
}









#include "buildcfg.h"
#include "config.h"
#include "imagelist.h"
#include "str.h"

#include <curl/curl.h>
#include <dirent.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

static void clean_cache(const char* path)
{
    DIR* dirp = opendir(path);
    if (!dirp) {
        return;
    }

    const size_t path_len = strlen(path);
    struct dirent* dent;
    while ((dent = readdir(dirp))) {
        if ((strcmp(dent->d_name, ".") == 0) ||
            (strcmp(dent->d_name, "..") == 0)) {
            continue;
        }

        const size_t fpath_len =
            path_len + strlen(dent->d_name) + 2; // $BASE/$FNAME\0
        char* fpath = malloc(fpath_len);
        snprintf(fpath, fpath_len, "%s/%s", path, dent->d_name);
        struct stat statbuf;
        if (stat(fpath, &statbuf) == 0) {
            if (S_ISDIR(statbuf.st_mode)) {
                fprintf(stderr,
                        "Found unexpected directory '%s' in cache path '%s'\n",
                        fpath, path);
            } else {
                if (unlink(fpath) != 0) {
                    fprintf(stderr, "Failed to clean cache file '%s'\n", fpath);
                    perror("Can't delete file");
                }
            }
        }
        free(fpath);
    }
}

struct image_list {
    char* www_url;
    char* www_cache_dir;
    struct image_prefetcher_ctx image_prefetcher;
    size_t index;
    struct image* no_img;
    struct image* current;
};

static struct image_list ctx = {
    .index = 0,
    .no_img = NULL,
    .current = NULL,
    .www_url = NULL,
    .www_cache_dir = NULL,
    .image_prefetcher = {
        .downloader_impl = NULL,
        .downloader_impl_usr = NULL,
        .image_cache_size = 10,
        .image_cache = NULL,
        .prefetch_n = 3,
        .image_cache_r = 0,
        .image_cache_w = 0,
    },
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

    if (strcmp(key, IMGLIST_WWW_CACHE) == 0) {
        ctx.www_cache_dir = strdup(value);
        return cfgst_ok;
    }

    if (strcmp(key, IMGLIST_WWW_CACHE_LIMIT) == 0) {
        long num = 0;
        if (str_to_num(value, 0, &num, 0) && num > 0) {
            ctx.image_prefetcher.image_cache_size = num;
            return cfgst_ok;
        }
        return cfgst_invalid_value;
    }

    if (strcmp(key, IMGLIST_WWW_PREFETCH_N) == 0) {
        long num = 0;
        if (str_to_num(value, 0, &num, 0) && num > 0) {
            ctx.image_prefetcher.prefetch_n = num;
            return cfgst_ok;
        }
        return cfgst_invalid_value;
    }

    return cfgst_invalid_key;
}

struct ctx_curl_active_rq {
    CURL* curl_handle;
    char* mem_buf;
    size_t mem_buf_sz;
    bool copy_to_file;
    char download_fname[255];
    FILE* fp;
};
struct ctx_curl_active_rq curl_active_rq;

static size_t write_data(void *curl_fp, size_t size, size_t nmemb, void *usr)
{
  size_t chunk_sz = size * nmemb;
  struct ctx_curl_active_rq* ctx = usr;
  if (ctx->copy_to_file) {
      size_t written = fwrite(curl_fp, size, nmemb, ctx->fp);
      if (!written) {
          fprintf(stderr, "Fail to download, can't write");
      }
  }
  char* reallocd_mem = realloc(ctx->mem_buf, ctx->mem_buf_sz + chunk_sz + 1);
  if (!reallocd_mem) {
      fprintf(stderr, "Fail to download, bad alloc");
      return 0;
  }
  ctx->mem_buf = reallocd_mem;
  memcpy(&ctx->mem_buf[ctx->mem_buf_sz], curl_fp, chunk_sz);

  ctx->mem_buf_sz = ctx->mem_buf_sz + chunk_sz;
  ctx->mem_buf[ctx->mem_buf_sz] = 0;

  return chunk_sz;
}

static void downloader_init(struct image_list* ctx)
{
    curl_global_init(CURL_GLOBAL_ALL);
    curl_active_rq.curl_handle = curl_easy_init();
    curl_easy_setopt(curl_active_rq.curl_handle, CURLOPT_VERBOSE, 0L);
    curl_easy_setopt(curl_active_rq.curl_handle, CURLOPT_NOPROGRESS, 1L);
    curl_easy_setopt(curl_active_rq.curl_handle, CURLOPT_WRITEFUNCTION, write_data);
    curl_easy_setopt(curl_active_rq.curl_handle, CURLOPT_WRITEDATA, &curl_active_rq);
    curl_easy_setopt(curl_active_rq.curl_handle, CURLOPT_URL, ctx->www_url);

    curl_active_rq.mem_buf = NULL;
    curl_active_rq.mem_buf_sz = 0;
    curl_active_rq.copy_to_file = false;
}

static void downloader_free()
{
    curl_easy_cleanup(curl_active_rq.curl_handle);
    curl_global_cleanup();
}

static struct image* downloader_get_one(void* usr)
{
    struct image_list* ctx = usr;
    size_t maybe_new_idx = ctx->index + 1;
    snprintf(curl_active_rq.download_fname, 255, "%s/%ld_img.jpg",
             ctx->www_cache_dir, maybe_new_idx);
    curl_active_rq.fp = fopen(curl_active_rq.download_fname, "wb");
    if (!curl_active_rq.fp) {
        fprintf(stderr, "Can't open '%s' to download from remote...\n", curl_active_rq.download_fname);
        return NULL;
      }

      CURLcode ret = curl_easy_perform(curl_active_rq.curl_handle);
      if (ret != CURLE_OK) {
          printf("Fail to download from %s: %s\n", ctx->www_url, curl_easy_strerror(ret));
          abort();
          // TODO continue;
      }

      fclose(curl_active_rq.fp);
      ctx->index = maybe_new_idx;
      // struct image* img = image_from_file(curl_active_rq.download_fname);
      struct image* img = image_create("<mem>", (uint8_t*)curl_active_rq.mem_buf, curl_active_rq.mem_buf_sz);

      free(curl_active_rq.mem_buf);
      curl_active_rq.mem_buf_sz = 0;
      curl_active_rq.mem_buf = NULL;

      if (!img) {
          printf("Fail to create img %s\n", curl_active_rq.download_fname);
          abort();
          // TODO continue;
      }

      return img;
}


void image_list_init()
{
    // register configuration loader
    config_add_loader(IMGLIST_CFG_SECTION, load_config);
    image_prefetcher_init(&ctx.image_prefetcher, downloader_get_one, &ctx);
}

void image_list_free(void)
{

    image_prefetcher_free(&ctx.image_prefetcher);
    downloader_free();

    clean_cache(ctx.www_cache_dir);
    image_free(ctx.no_img);
    free(ctx.www_url);
    free(ctx.www_cache_dir);
}

bool image_list_scan(const char** files, size_t num)
{
    clean_cache(ctx.www_cache_dir);

    if (!ctx.www_cache_dir) {
        fprintf(stderr,
                "Missing www_cache config entry; can't use www-source without "
                "cache location\n");
        abort();
    }

  if (!ctx.www_url) {
      fprintf(stderr, "Missing www_url config entry; can't use www-source without URL\n");
      abort();
  }

  {
      DIR* dirp = opendir(ctx.www_cache_dir);
      if (!dirp) {
          fprintf(stderr, "Can't open www_cache directory '%s'\n",
                  ctx.www_cache_dir);
          perror("Can't open www_cache directory");
          abort();
      }
      closedir(dirp);
  }

    (void) files;
    (void) num;
    printf("LOAD TEST IMG\n");
    struct image* img = image_from_file("/media/batman/pCloudBCK/pCloud/Fotos/2015/Holanda/KitKat/20151107.030117.DSC_1506.JPG");
    ctx.no_img = img;
    ctx.current = ctx.no_img;

    downloader_init(&ctx);
    image_prefetcher_start(&ctx.image_prefetcher, 10, 3);

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
          img = image_prefetcher_jump_next(&ctx.image_prefetcher);
          ctx.current = img ? img : ctx.no_img;
          return img != NULL;
      case jump_prev_file:
          img = image_prefetcher_jump_prev(&ctx.image_prefetcher);
          ctx.current = img ? img : ctx.no_img;
          return img != NULL;
    }
    abort();
}
