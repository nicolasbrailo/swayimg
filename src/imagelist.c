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



#if 0

#include <pthread.h>
#include <semaphore.h>

struct preloader_ctx {
    pthread_t thread;
    sem_t prefetched_count;
    size_t prefetched_count_tgt;
    pthread_cond_t condvar;
    pthread_mutex_t cv_mut;
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
    char* www_cache;

    CURL* curl_handle;
    struct {
        char download_fname[255];
        FILE* fp;
    } active_rq;

    size_t index;
    struct image* current;
    size_t cache_limit;

    struct image** image_cache;
    size_t image_cache_size;
    size_t prefetch_n;
    size_t image_cache_r;
    size_t image_cache_w;
};

static struct image_list ctx = {
    .index = 0,
    .current = NULL,
    .www_url = NULL,
    .www_cache = NULL,
    .cache_limit = 10,
    .curl_handle = NULL,
    .active_rq = {
        .download_fname = {0},
        .fp = NULL,
    },

    .image_cache = NULL,
    .image_cache_size = 0,
    .prefetch_n = 0,
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

  ctx.image_cache_size = 10;
  ctx.prefetch_n = 5;
  ctx.image_cache_r = 0;
  ctx.image_cache_w = 0;
  ctx.image_cache = malloc(sizeof(void*) * ctx.image_cache_size);
  memset(ctx.image_cache, 0, sizeof(void*) * ctx.image_cache_size);
}

void image_list_free(void)
{
  clean_cache(ctx.www_cache);
  image_free(ctx.current);
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

  // TODO error handling
  curl_easy_setopt(ctx.curl_handle, CURLOPT_URL, ctx.www_url);

    (void) files;
    (void) num;
    printf("LOAD TEST IMG\n");
    struct image* img = image_from_file("/media/batman/pCloudBCK/pCloud/Fotos/2015/Holanda/KitKat/20151107.030117.DSC_1506.JPG");
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
      case jump_prev_dir: // fallthrough
          return false;
      case jump_next_file:
        {
            size_t cnt = (ctx.image_cache_w >= ctx.image_cache_r)?
                            ctx.image_cache_w - ctx.image_cache_r :
                            ctx.image_cache_w + ctx.image_cache_size - ctx.image_cache_r;
            printf("There are %lu images cached, I need %lu\n", cnt, ctx.prefetch_n);

            for (size_t i=cnt; i < ctx.prefetch_n; ++i) {
                size_t maybe_new_idx = ctx.index + 1;
                snprintf(ctx.active_rq.download_fname, 255, "%s/%ld_img.jpg", ctx.www_cache, maybe_new_idx);
                ctx.active_rq.fp = fopen(ctx.active_rq.download_fname, "wb");
                if (!ctx.active_rq.fp) {
                    fprintf(stderr, "Can't open '%s' to download from remote...\n", ctx.active_rq.download_fname);
                    return false;
                  }
                  printf("Will try to download '%s'\n", ctx.active_rq.download_fname);
                  curl_easy_perform(ctx.curl_handle);
                  printf("downl'd '%s' ...\n", ctx.active_rq.download_fname);
                  fclose(ctx.active_rq.fp);
                  ctx.index = maybe_new_idx;
                  struct image* img = image_from_file(ctx.active_rq.download_fname);
                    if (ctx.image_cache[ctx.image_cache_w]) {
                      printf("Expiring cached image from windex %lu\n", ctx.image_cache_w);
                      image_free(ctx.current);
                    }
                  ctx.image_cache[ctx.image_cache_w] = img;
                  printf("Stored '%s' to write index %lu\n", ctx.active_rq.download_fname, ctx.image_cache_w);
                  ctx.image_cache_w = (ctx.image_cache_w + 1) % ctx.image_cache_size;
            }

            ctx.current = ctx.image_cache[ctx.image_cache_r];
            printf("Read image from cache R idx %lu\n",ctx.image_cache_r);
            ctx.image_cache_r = (ctx.image_cache_r + 1) % ctx.image_cache_size;
        }
        return true;
      case jump_prev_file: {
          if (ctx.index == 0)  {
            return false;
          }
          size_t maybe_prev_idx = ctx.index - 1;
          snprintf(ctx.active_rq.download_fname, 255, "%s/%ld_img.jpg", ctx.www_cache, maybe_prev_idx);
          struct image* maybe_prev_img = image_from_file(ctx.active_rq.download_fname);
          if (!maybe_prev_img) {
              printf("No prev img %s\n", ctx.active_rq.download_fname);
              return false;
          }
          printf("Found prev img %s\n", ctx.active_rq.download_fname);
          image_free(ctx.current);
          ctx.current = maybe_prev_img;
          ctx.index = maybe_prev_idx;
          return true;
        return true;
      }
    }
    abort();
}
