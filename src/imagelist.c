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



#include <pthread.h>
#if 0

#include <semaphore.h>

struct preloader_ctx {
    pthread_t thread;
    sem_t prefetched_count;
    size_t prefetched_count_tgt;
    pthread_cond_t condvar;
};

static void* preloader_thread(void* usr)
{
    struct preloader_ctx* pctx = usr;
    (void)pctx;
    while (true) {
        printf("I'm fetching an image (block)\n");
        int cnt = sem_getvalue(&pctx->prefetched_count, &cnt);
        printf("Target is %lu, current is %d images\n",
               pctx->prefetched_count_tgt, cnt);
        for (size_t i = 0; i + cnt < pctx->prefetched_count_tgt; ++i) {
            printf("I'm fetching ONE image...\n");
            // sleep(1);
            printf("GOT ONE image...\n");
            sem_post(&pctx->prefetched_count);
        }

        printf("Done fetching imgs, will sleep/block\n");
        pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);
        pthread_mutex_lock(&pctx->cv_mut);
        pthread_cond_wait(&pctx->condvar, &pctx->cv_mut);
        pthread_mutex_unlock(&pctx->cv_mut);
        pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
        printf("I'm woken up\n");
    }

    return NULL;
}

void preloader_init(struct preloader_ctx* pctx)
{
    if (pctx->thread) {
        fprintf(stderr, "preloader_init double init\n");
        abort();
    }

    if (pthread_create(&pctx->thread, NULL, preloader_thread, pctx) != 0) {
        perror("Fail pthread_create");
        abort();
    }
    if (sem_init(&pctx->prefetched_count, /*pshared=thread_only*/ 0,
                 /*value=*/0) != 0) {
        perror("Fail pthread_create");
        abort();
    }
    if (pthread_cond_init(&pctx->condvar, NULL) != 0) {
        perror("Fail pthread_cond_init");
        abort();
    }
    if (pthread_mutex_init(&pctx->cv_mut, NULL) != 0) {
        perror("Fail pthread_mutex_init");
        abort();
    }

    pctx->prefetched_count_tgt = 5;
}

void preloader_free(struct preloader_ctx* pctx)
{
    // if (pthread_cond_broadcast(&pctx->condvar) != 0) {
    // perror("pthread_cond_broadcast"); }
    if (pthread_cancel(pctx->thread) != 0) {
        perror("pthread_cancel");
    }
    if (pthread_join(pctx->thread, NULL) != 0) {
        perror("pthread_join");
    }
    if (pthread_cond_destroy(&pctx->condvar) != 0) {
        perror("Fail pthread_cond_destroy");
    }
    if (pthread_mutex_destroy(&pctx->cv_mut) != 0) {
        perror("Fail pthread_mutex_destroy");
    }
    pctx->thread = 0;
}

void* preloader_get(struct preloader_ctx* pctx)
{
    if (pthread_cond_broadcast(&pctx->condvar) != 0) {
        perror("pthread_cond_broadcast");
    }

    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) != -1) {
        perror("clock_gettime");
        return NULL;
    }

    ts.tv_sec += 5;
    while (true) {
        const int ret = sem_timedwait(&pctx->prefetched_count, &ts);
        if (ret == 0) {
            printf("Found image, will return\n");
            return NULL;
        }

        if (ret == EINTR) {
            continue;
        }

        if (ret == ETIMEDOUT) {
            printf("Timeout on image load\n");
            return NULL;
        }

        perror("sem_timedwait");
        abort();
    };
}





#endif








struct image_list {
    char* www_url;
    bool www_url_is_set;
    char* www_cache;

    CURL* curl_handle;
    struct {
        char download_fname[255];
        FILE* fp;
    } active_rq;

    size_t index;
    struct image* no_img;
    size_t cache_limit;

    pthread_t thread;

    pthread_mutex_t cv_mut;
    struct image** image_cache;
    size_t image_cache_size;
    size_t prefetch_n;
    size_t image_cache_r;
    size_t image_cache_w;
};

static struct image_list ctx = {
    .index = 0,
    .no_img = NULL,
    .www_url = NULL,
    .www_url_is_set = false,
    .www_cache = NULL,
    .cache_limit = 10,
    .curl_handle = NULL,
    .active_rq = {
        .download_fname = {0},
        .fp = NULL,
    },

    .image_cache = NULL,
    .image_cache_size = 0,
    .prefetch_n = 3,
    .image_cache_r = 0,
    .image_cache_w = 0,
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

static void* downloader_thread(void* usr)
{
    struct image_list* ctx = usr;

    curl_global_init(CURL_GLOBAL_ALL);
    CURL* curl_handle = curl_easy_init();
    curl_easy_setopt(curl_handle, CURLOPT_VERBOSE, 0L);
    curl_easy_setopt(curl_handle, CURLOPT_NOPROGRESS, 1L);
    curl_easy_setopt(curl_handle, CURLOPT_WRITEFUNCTION, write_data);
    curl_easy_setopt(curl_handle, CURLOPT_WRITEDATA, ctx);

    // Wait for cfg
    bool cfg_ready = false;
    while (!cfg_ready) {
      pthread_mutex_lock(&ctx->cv_mut);
      cfg_ready = ctx->www_url_is_set;
      pthread_mutex_unlock(&ctx->cv_mut);
      // TODO use a cv
    }

    // TODO error handling
    curl_easy_setopt(curl_handle, CURLOPT_URL, ctx->www_url);

    while (true) {
        size_t cnt = (ctx->image_cache_w >= ctx->image_cache_r)?
                        ctx->image_cache_w - ctx->image_cache_r :
                        ctx->image_cache_w + ctx->image_cache_size - ctx->image_cache_r;

        for (size_t i=cnt; i < ctx->prefetch_n; ++i) {
            size_t maybe_new_idx = ctx->index + 1;
            snprintf(ctx->active_rq.download_fname, 255, "%s/%ld_img.jpg", ctx->www_cache, maybe_new_idx);
            ctx->active_rq.fp = fopen(ctx->active_rq.download_fname, "wb");
            if (!ctx->active_rq.fp) {
                fprintf(stderr, "Can't open '%s' to download from remote...\n", ctx->active_rq.download_fname);
                return false;
              }

              CURLcode ret = curl_easy_perform(curl_handle);
              if (ret != CURLE_OK) {
                  printf("Fail to download from %s: %s\n", ctx->www_url, curl_easy_strerror(ret));
                  abort();
                  // TODO continue;
              }

              fclose(ctx->active_rq.fp);
              ctx->index = maybe_new_idx;
              struct image* img = image_from_file(ctx->active_rq.download_fname);
              if (!img) {
                  printf("Fail to create img %s\n", ctx->active_rq.download_fname);
                  abort();
                  // TODO continue;
              }

              printf("Created image wIdx=%lu %p '%s'\n", ctx->image_cache_w, (void*)img, ctx->active_rq.download_fname);
              pthread_mutex_lock(&ctx->cv_mut);
              struct image* old_img = ctx->image_cache[ctx->image_cache_w];
              ctx->image_cache[ctx->image_cache_w] = img;
              ctx->image_cache_w = (ctx->image_cache_w + 1) % ctx->image_cache_size;
              pthread_mutex_unlock(&ctx->cv_mut);

              if (old_img) {
                printf("Expiring cached image from cache[%lu] = %p\n", ctx->image_cache_w, (void*)ctx->image_cache[ctx->image_cache_w]);
                image_free(old_img);
              }
        }

        // TODO use a CV
        sleep(1);
    }

    return NULL;
}



void image_list_init()
{
  if (ctx.curl_handle) {
      fprintf(stderr, "Double init of image list requested\n");
      abort();
  }

  // register configuration loader
  config_add_loader(IMGLIST_CFG_SECTION, load_config);

  ctx.image_cache_size = 10;
  ctx.prefetch_n = 3;
  ctx.image_cache_r = 0;
  ctx.image_cache_w = 0;
  ctx.image_cache = malloc(sizeof(void*) * ctx.image_cache_size);
  memset(ctx.image_cache, 0, sizeof(void*) * ctx.image_cache_size);

  if (pthread_create(&ctx.thread, NULL, downloader_thread, &ctx) != 0) {
      perror("Fail pthread_create");
      abort();
  }
}

void image_list_free(void)
{
    // TODO Join threasd
  clean_cache(ctx.www_cache);
  image_free(ctx.no_img);
  curl_easy_cleanup(ctx.curl_handle);
  curl_global_cleanup();
  free(ctx.www_url);
  free(ctx.www_cache);
  free(ctx.image_cache);
  ctx.curl_handle = NULL;
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

  pthread_mutex_lock(&ctx.cv_mut);
  ctx.www_url_is_set = true;
  pthread_mutex_unlock(&ctx.cv_mut);

    return true;
}

size_t image_list_size(void)
{
    return -1;
}

struct image_entry image_list_current(void)
{
    pthread_mutex_lock(&ctx.cv_mut);
    struct image_entry entry = {
        .index = ctx.index,
        .image = ctx.image_cache[ctx.image_cache_r],
    };
    if (!entry.image) {
        entry.image = ctx.no_img;
    }
    pthread_mutex_unlock(&ctx.cv_mut);
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
      case jump_prev_dir: // fallthrough
          return false;
      case jump_next_file:
        {
            bool can_move = false;
            pthread_mutex_lock(&ctx.cv_mut);
            size_t next_idx = (ctx.image_cache_r + 1) % ctx.image_cache_size;
            if (ctx.image_cache_w == ctx.image_cache_r) {
                printf("No images, cache empty\n");
            } else if (!ctx.image_cache[next_idx]) {
                printf("Fail reading NEXT file: cache R %lu=%p, W=%lu\n", ctx.image_cache_r, (void*)ctx.image_cache[ctx.image_cache_r], ctx.image_cache_w);
            } else {
                printf("NEXT file: Set ptr to next image from cache R idx %lu = %p\n",ctx.image_cache_r, (void*)ctx.image_cache[ctx.image_cache_r]);
                ctx.image_cache_r = next_idx;
                can_move = true;
            }
            pthread_mutex_unlock(&ctx.cv_mut);
            return can_move;
        }
        return true;
      case jump_prev_file: {
        pthread_mutex_lock(&ctx.cv_mut);
        size_t maybe_prev = ctx.image_cache_r == 0? ctx.image_cache_size -1 : ctx.image_cache_r - 1;
        if (maybe_prev == ctx.image_cache_w || !ctx.image_cache[maybe_prev]) {
            printf("No more history\n");
            pthread_mutex_unlock(&ctx.cv_mut);
            return false;
        }
        ctx.image_cache_r = maybe_prev;
        printf("Prev file: Read image from cache R idx %lu = (void*)%p\n",ctx.image_cache_r, (void*)ctx.image_cache[ctx.image_cache_r]);
        pthread_mutex_unlock(&ctx.cv_mut);
        return true;
      }
    }
    abort();
}
