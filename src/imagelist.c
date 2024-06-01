// SPDX-License-Identifier: MIT
// List of images.
// Copyright (C) 2022 Artem Senichev <artemsen@gmail.com>

#include "imagelist.h"

#include "buildcfg.h"
#include "config.h"
#include "str.h"

#include <sys/types.h>
#include <dirent.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <curl/curl.h>

#define TEST_IMG2 "/home/batman/Photos/ToTag/0518 London Eye/DSC_4790.JPG"
#define TEST_IMG "/home/batman/Downloads/img.jpg"

struct image_list {
  size_t index;
  struct image* current;
  char* www_url;
  char* www_cache;
  size_t cache_limit;
  size_t prefetch_n;
  CURL* curl_handle;
  struct {
      char download_fname[255];
      FILE* fp;
  } active_rq;
};

static struct image_list ctx = {
    .index = 0,
    .current = NULL,
    .www_url = NULL,
    .www_cache = NULL,
    .cache_limit = 10,
    .prefetch_n = 5,
    .curl_handle = NULL,
    .active_rq = {
        .download_fname = {0},
        .fp = NULL,
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
            ctx.cache_limit = num;
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

static size_t write_data(void *curl_fp, size_t size, size_t nmemb, void *usr)
{
  struct image_list* ctx = usr;
  size_t written = fwrite(curl_fp, size, nmemb, ctx->active_rq.fp);
  return written;
}

void image_list_init()
{
  if (ctx.curl_handle) {
      fprintf(stderr, "Double init of image list requested\n");
      abort();
  }

  // register configuration loader
  config_add_loader(IMGLIST_CFG_SECTION, load_config);

  // TODO error handling
  curl_global_init(CURL_GLOBAL_ALL);
  ctx.curl_handle = curl_easy_init();
  curl_easy_setopt(ctx.curl_handle, CURLOPT_VERBOSE, 0L);
  curl_easy_setopt(ctx.curl_handle, CURLOPT_NOPROGRESS, 1L);
  curl_easy_setopt(ctx.curl_handle, CURLOPT_WRITEFUNCTION, write_data);
  curl_easy_setopt(ctx.curl_handle, CURLOPT_WRITEDATA, &ctx);
}

void image_list_free(void)
{
  image_free(ctx.current);
  curl_easy_cleanup(ctx.curl_handle);
  curl_global_cleanup();
  free(ctx.www_url);
  ctx.curl_handle = NULL;
  ctx.curl_handle = NULL;
}

bool image_list_scan(const char** files, size_t num)
{
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

  // TODO error handling
  curl_easy_setopt(ctx.curl_handle, CURLOPT_URL, ctx.www_url);

    (void) files;
    (void) num;
    printf("LOAD TEST IMG\n");
    struct image* img = image_from_file(TEST_IMG);
    ctx.current = img;
    
    return true;
}

size_t image_list_size(void)
{
    return -1;
}

struct image_entry image_list_current(void)
{
    struct image_entry entry = { .index = ctx.index, .image = ctx.current };
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
    switch (jump) {
      case jump_first_file: // fallthrough
      case jump_last_file: // fallthrough
      case jump_next_dir: // fallthrough
      case jump_prev_dir:
        return false;
      case jump_next_file:
        {
          snprintf(ctx.active_rq.download_fname, 255, "%ld_img.jpg", ++ctx.index);
          printf("Created fname '%s' ...\n", ctx.active_rq.download_fname);
          ctx.active_rq.fp = fopen(ctx.active_rq.download_fname, "wb");
          printf("fp open '%s' ...\n", ctx.active_rq.download_fname);
          if (!ctx.active_rq.fp) {
              printf("Can't open '%s' to download from remote...\n", ctx.active_rq.download_fname);
              return false;
            } else {
              printf("Will try to download '%s'\n", ctx.active_rq.download_fname);
              curl_easy_perform(ctx.curl_handle);
              printf("downl'd '%s' ...\n", ctx.active_rq.download_fname);
              fclose(ctx.active_rq.fp);
              printf("img free '%s' ...\n", ctx.active_rq.download_fname);
              image_free(ctx.current);
              printf("open file '%s' ...\n", ctx.active_rq.download_fname);
              ctx.current = image_from_file(ctx.active_rq.download_fname);
          }
        }
        return true;
      case jump_prev_file:
        return true;
    }
    abort();
}
