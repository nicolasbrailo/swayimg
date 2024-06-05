// SPDX-License-Identifier: MIT
// List of images.
// Copyright (C) 2022 Artem Senichev <artemsen@gmail.com>

#include "imagelist.h"

#include "buildcfg.h"
#include "config.h"
#include "str.h"

#include <sys/types.h>
#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <curl/curl.h>

static void clean_cache(const char *path) {
    DIR *dirp = opendir(path);
    if (!dirp) {
        return;
    }

    const size_t path_len = strlen(path);
    struct dirent *dent;
    while ((dent = readdir(dirp))) {
        if ((strcmp(dent->d_name, ".") == 0) || (strcmp(dent->d_name, "..") == 0)) {
            continue;
        }

        const size_t fpath_len = path_len + strlen(dent->d_name) + 2; // $BASE/$FNAME\0
        char *fpath = malloc(fpath_len);
        snprintf(fpath, fpath_len, "%s/%s", path, dent->d_name);
        struct stat statbuf;
        if (stat(fpath, &statbuf) == 0) {
            if (S_ISDIR(statbuf.st_mode)) {
                fprintf(stderr, "Found unexpected directory '%s' in cache path '%s'\n", fpath, path);
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


typedef struct image* (*downloader_cb)(void*);

struct image_prefetcher_ctx {
    downloader_cb downloader_impl;
    void* downloader_impl_usr;
    pthread_t thread;
    pthread_cond_t condvar;
    pthread_mutex_t cv_mut;
    bool shutdown_thread;
    bool www_url_is_set;
    size_t cache_limit;
    struct image** image_cache;
    size_t image_cache_size;
    size_t prefetch_n;
    size_t image_cache_r;
    size_t image_cache_w;
};

static void* image_cache_thread(void* usr);

void image_cache_init(struct image_prefetcher_ctx* ctx, downloader_cb cb,
                      void* downloader_impl_usr)
{
    if (ctx->thread) {
        fprintf(stderr, "Double init of image list requested\n");
        abort();
    }

  ctx->downloader_impl = cb;
  ctx->downloader_impl_usr = downloader_impl_usr;
  ctx->image_cache_size = 10;
  ctx->prefetch_n = 3;
  ctx->image_cache_r = 0;
  ctx->image_cache_w = 0;
  ctx->image_cache = malloc(sizeof(void*) * ctx->image_cache_size);
  memset(ctx->image_cache, 0, sizeof(void*) * ctx->image_cache_size);

    if (pthread_cond_init(&ctx->condvar, NULL) != 0) {
        perror("Fail pthread_cond_init");
        abort();
    }
    if (pthread_mutex_init(&ctx->cv_mut, NULL) != 0) {
        perror("Fail pthread_mutex_init");
        abort();
    }

  if (pthread_create(&ctx->thread, NULL, image_cache_thread, ctx) != 0) {
      perror("Fail pthread_create");
      abort();
  }
}

void image_cache_free(struct image_prefetcher_ctx* ctx)
{
    ctx->shutdown_thread = true;

            if (pthread_cond_broadcast(&ctx->condvar) != 0) {
                perror("pthread_cond_broadcast");
            }


    // TODO struct timespec timeout;
    // TODO clock_gettime(CLOCK_MONOTONIC, &timeout);
    // TODO timeout.tv_sec += 3;
    // TODO const int thread_join_ret = pthread_timedjoin_np(ctx->thread, NULL, &timeout);
    // TODO if (thread_join_ret == ETIMEDOUT) {
    // TODO     printf("Timeout terminating downloader thread\n");
    // TODO }

    if (pthread_cond_destroy(&ctx->condvar) != 0) {
        perror("Fail pthread_cond_destroy");
    }
    if (pthread_mutex_destroy(&ctx->cv_mut) != 0) {
        perror("Fail pthread_mutex_destroy");
    }

    free(ctx->image_cache);
}

void* image_cache_thread(void* usr)
{
    struct image_prefetcher_ctx* ctx = usr;

    // Wait for cfg
    bool cfg_ready = false;
    while (!cfg_ready) {
      pthread_mutex_lock(&ctx->cv_mut);
      cfg_ready = ctx->www_url_is_set;
      pthread_mutex_unlock(&ctx->cv_mut);
      // TODO use a cv
    }

    while (!ctx->shutdown_thread) {
        size_t cnt = (ctx->image_cache_w >= ctx->image_cache_r)?
                        ctx->image_cache_w - ctx->image_cache_r :
                        ctx->image_cache_w + ctx->image_cache_size - ctx->image_cache_r;

        for (size_t i=cnt; i < ctx->prefetch_n; ++i) {
            struct image* img = ctx->downloader_impl(ctx->downloader_impl_usr);
            pthread_mutex_lock(&ctx->cv_mut);
            struct image* old_img = ctx->image_cache[ctx->image_cache_w];
            ctx->image_cache[ctx->image_cache_w] = img;
            ctx->image_cache_w =
                (ctx->image_cache_w + 1) % ctx->image_cache_size;
            pthread_mutex_unlock(&ctx->cv_mut);

            if (old_img) {
                printf("Expiring cached image from cache[%lu] = %p\n", ctx->image_cache_w, (void*)ctx->image_cache[ctx->image_cache_w]);
                image_free(old_img);
              }
        }

        pthread_mutex_lock(&ctx->cv_mut);
        pthread_cond_wait(&ctx->condvar, &ctx->cv_mut);
        pthread_mutex_unlock(&ctx->cv_mut);
    }

    return NULL;
}

static struct image* jump_next(struct image_prefetcher_ctx* ctx)
{
    bool can_move = false;
    pthread_mutex_lock(&ctx->cv_mut);
    size_t next_idx = (ctx->image_cache_r + 1) % ctx->image_cache_size;
    if (ctx->image_cache_w == ctx->image_cache_r) {
        printf("No images, cache empty\n");
    } else if (!ctx->image_cache[next_idx]) {
        printf("Fail reading NEXT file: cache R %lu=%p, W=%lu\n",
               ctx->image_cache_r, (void*)ctx->image_cache[ctx->image_cache_r],
               ctx->image_cache_w);
    } else {
        printf("NEXT file: Set ptr to next image from cache R idx %lu = %p\n",
               ctx->image_cache_r, (void*)ctx->image_cache[ctx->image_cache_r]);
        ctx->image_cache_r = next_idx;
        can_move = true;
    }
    pthread_mutex_unlock(&ctx->cv_mut);
    if (pthread_cond_broadcast(&ctx->condvar) != 0) {
        perror("pthread_cond_broadcast");
    }
    return can_move ? ctx->image_cache[ctx->image_cache_r] : NULL;
}

static struct image* jump_prev(struct image_prefetcher_ctx* ctx)
{
    pthread_mutex_lock(&ctx->cv_mut);
    size_t maybe_prev = ctx->image_cache_r == 0 ? ctx->image_cache_size - 1
                                                : ctx->image_cache_r - 1;
    if (maybe_prev == ctx->image_cache_w || !ctx->image_cache[maybe_prev]) {
        printf("No more history\n");
        pthread_mutex_unlock(&ctx->cv_mut);
        if (pthread_cond_broadcast(&ctx->condvar) != 0) {
            perror("pthread_cond_broadcast");
        }
        return NULL;
    }
    ctx->image_cache_r = maybe_prev;
    printf("Prev file: Read image from cache R idx %lu = (void*)%p\n",
           ctx->image_cache_r, (void*)ctx->image_cache[ctx->image_cache_r]);
    pthread_mutex_unlock(&ctx->cv_mut);
    if (pthread_cond_broadcast(&ctx->condvar) != 0) {
        perror("pthread_cond_broadcast");
    }
    return ctx->image_cache[ctx->image_cache_r];
}

struct image_list {
    char* www_url;
    char* www_cache;
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
    .www_cache = NULL,
    .image_prefetcher = {
        .downloader_impl = NULL,
        .downloader_impl_usr = NULL,
        .cache_limit = 10,
        .www_url_is_set = false,
        .image_cache = NULL,
        .shutdown_thread = false,
        .image_cache_size = 0,
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
        ctx.www_cache = strdup(value);
        return cfgst_ok;
    }

    if (strcmp(key, IMGLIST_WWW_CACHE_LIMIT) == 0) {
        long num = 0;
        if (str_to_num(value, 0, &num, 0) && num > 0) {
            ctx.image_prefetcher.cache_limit = num;
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
    char download_fname[255];
    FILE* fp;
    CURL* curl_handle;
};
struct ctx_curl_active_rq curl_active_rq;

static size_t write_data(void *curl_fp, size_t size, size_t nmemb, void *usr)
{
  struct ctx_curl_active_rq* ctx = usr;
  size_t written = fwrite(curl_fp, size, nmemb, ctx->fp);
  return written;
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
    snprintf(curl_active_rq.download_fname, 255, "%s/%ld_img.jpg", ctx->www_cache, maybe_new_idx);
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
      struct image* img = image_from_file(curl_active_rq.download_fname);
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
    image_cache_init(&ctx.image_prefetcher, downloader_get_one, &ctx);
}

void image_list_free(void)
{

    image_cache_free(&ctx.image_prefetcher);
    downloader_free();

    clean_cache(ctx.www_cache);
    image_free(ctx.no_img);
    free(ctx.www_url);
    free(ctx.www_cache);
}

bool image_list_scan(const char** files, size_t num)
{
  clean_cache(ctx.www_cache);

  if (!ctx.www_cache) {
      fprintf(stderr, "Missing www_cache config entry; can't use www-source without cache location\n");
      abort();
  }

  if (!ctx.www_url) {
      fprintf(stderr, "Missing www_url config entry; can't use www-source without URL\n");
      abort();
  }

  {
      DIR* dirp = opendir(ctx.www_cache);
      if (!dirp) {
          fprintf(stderr, "Can't open www_cache directory '%s'\n", ctx.www_cache);
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
    pthread_mutex_lock(&ctx.image_prefetcher.cv_mut);
    ctx.image_prefetcher.www_url_is_set = true;
    pthread_mutex_unlock(&ctx.image_prefetcher.cv_mut);

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
          img = jump_next(&ctx.image_prefetcher);
          ctx.current = img ? img : ctx.no_img;
          return img != NULL;
      case jump_prev_file:
          img = jump_prev(&ctx.image_prefetcher);
          ctx.current = img ? img : ctx.no_img;
          return img != NULL;
    }
    abort();
}
