// SPDX-License-Identifier: MIT
// List of images.
// Copyright (C) 2022 Artem Senichev <artemsen@gmail.com>

#include "imagelist.h"

#include "buildcfg.h"
#include "config.h"
#include "image.h"
#include "imageprefetcher.h"
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
    struct image_prefetcher_ctx* image_prefetcher;
    size_t index;
    struct image* no_img;
    struct image* current;
    size_t image_cache_size; ///< Maximum number of images to cache
    size_t prefetch_n;       ///< Number of images to prefetch
};

static struct image_list ctx = {
    .index = 0,
    .no_img = NULL,
    .current = NULL,
    .www_url = NULL,
    .www_cache_dir = NULL,
    .image_prefetcher = NULL,
    .image_cache_size = 10,
    .prefetch_n = 3,
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
    ctx.image_prefetcher = image_prefetcher_init(downloader_get_one, &ctx);
}

void image_list_free(void)
{
    image_prefetcher_free(ctx.image_prefetcher);
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

    if (!image_prefetcher_start(ctx.image_prefetcher, ctx.image_cache_size,
                                ctx.prefetch_n)) {
        return false;
    }

    while (image_prefetcher_get_cached(ctx.image_prefetcher) == 0) {
        // TODO timeout
    }
    ctx.current = image_prefetcher_jump_next(ctx.image_prefetcher);

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
