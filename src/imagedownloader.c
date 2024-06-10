#include "imagedownloader.h"

#include "image.h"

#include <curl/curl.h>
#include <dirent.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define MAX_PATH_LEN 255

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

struct downloader_ctx {
    char* www_url;
    char* www_cache_dir;
    bool clean_cache_after_use;
    CURL* curl_handle;
    char* mem_buf;
    size_t mem_buf_sz;
    size_t img_download_cnt;
    char download_fname[255];
    FILE* fp;
};

static size_t write_data(void* curl_fp, size_t size, size_t nmemb, void* usr);

struct downloader_ctx* downloader_init(const char* www_url,
                                       const char* www_cache_dir,
                                       bool clean_cache_after_use)
{
    if (!www_url) {
        fprintf(
            stderr,
            "Missing www_url config entry; can't use www-source without URL\n");
        return NULL;
    }

    if (www_cache_dir) {
        {
            DIR* dirp = opendir(www_cache_dir);
            if (!dirp) {
                fprintf(stderr, "Can't open www_cache directory '%s'\n",
                        www_cache_dir);
                perror("opendir");
                return NULL;
            }
            closedir(dirp);
        }

        if (clean_cache_after_use) {
            clean_cache(www_cache_dir);
        }
    }

    struct downloader_ctx* ctx = malloc(sizeof(struct downloader_ctx));
    if (!ctx) {
        fprintf(stderr, "bad alloc\n");
        return NULL;
    }

    ctx->img_download_cnt = 0;
    ctx->mem_buf = NULL;
    ctx->mem_buf_sz = 0;
    ctx->curl_handle = NULL;
    ctx->clean_cache_after_use = false;
    ctx->fp = NULL;

    ctx->www_url = strdup(www_url);
    ctx->www_cache_dir = www_cache_dir ? strdup(www_cache_dir) : NULL;
    ctx->clean_cache_after_use = clean_cache_after_use;
    if (!ctx->www_url || (!ctx->www_cache_dir && www_cache_dir)) {
        fprintf(stderr, "bad alloc\n");
        downloader_free(ctx);
        return NULL;
    }

    {
        const int ret = curl_global_init(CURL_GLOBAL_ALL);
        if (ret != 0) {
            fprintf(stderr, "Failed to global init curl: %s\n",
                    curl_easy_strerror(ret));
            downloader_free(ctx);
            return NULL;
        }
    }

    ctx->curl_handle = curl_easy_init();
    if (!ctx->curl_handle) {
        fprintf(stderr, "Failed to create curl_handle\n");
        downloader_free(ctx);
        return NULL;
    }

    int ret = CURLE_OK;
    ret = ret | curl_easy_setopt(ctx->curl_handle, CURLOPT_VERBOSE, 0L);
    ret = ret | curl_easy_setopt(ctx->curl_handle, CURLOPT_NOPROGRESS, 1L);
    ret = ret |
        curl_easy_setopt(ctx->curl_handle, CURLOPT_WRITEFUNCTION, write_data);
    ret = ret | curl_easy_setopt(ctx->curl_handle, CURLOPT_WRITEDATA, ctx);
    ret = ret | curl_easy_setopt(ctx->curl_handle, CURLOPT_URL, ctx->www_url);
    if (ret != CURLE_OK) {
        fprintf(stderr, "Failed to setup curl: %s\n", curl_easy_strerror(ret));
        downloader_free(ctx);
        return NULL;
    }

    return ctx;
}

void downloader_free(struct downloader_ctx* ctx)
{
    if (!ctx) {
        return;
    }

    if (ctx->curl_handle) {
        curl_easy_cleanup(ctx->curl_handle);
    }

    curl_global_cleanup();

    if (ctx->www_cache_dir && ctx->clean_cache_after_use) {
        clean_cache(ctx->www_cache_dir);
    }

    free(ctx->www_url);
    free(ctx->www_cache_dir);
    free(ctx);
}

static size_t write_data(void* curl_fp, size_t size, size_t nmemb, void* usr)
{
    size_t chunk_sz = size * nmemb;
    struct downloader_ctx* ctx = usr;
    if (ctx->www_cache_dir) {
        size_t written = fwrite(curl_fp, size, nmemb, ctx->fp);
        if (!written) {
            fprintf(stderr, "Fail to copy image to disk, can't write\n");
        }
    }

    char* reallocd_mem = realloc(ctx->mem_buf, ctx->mem_buf_sz + chunk_sz + 1);
    if (!reallocd_mem) {
        fprintf(stderr, "Fail to download, bad alloc\n");
        return 0;
    }

    ctx->mem_buf = reallocd_mem;
    memcpy(&ctx->mem_buf[ctx->mem_buf_sz], curl_fp, chunk_sz);

    ctx->mem_buf_sz = ctx->mem_buf_sz + chunk_sz;
    ctx->mem_buf[ctx->mem_buf_sz] = 0;

    return chunk_sz;
}

struct image* downloader_get_one(void* usr)
{
    struct downloader_ctx* ctx = usr;
    if (ctx->www_cache_dir) {
        snprintf(ctx->download_fname, MAX_PATH_LEN, "%s/%ld_img.jpg",
                 ctx->www_cache_dir, ctx->img_download_cnt++);
        ctx->fp = fopen(ctx->download_fname, "wb");
        if (!ctx->fp) {
            perror("fopen");
            fprintf(stderr, "Can't open '%s' to download from remote...\n",
                    ctx->download_fname);
            return NULL;
        }
    }

    const CURLcode ret = curl_easy_perform(ctx->curl_handle);
    if (ret != CURLE_OK) {
        printf("Fail to download from %s: %s\n", ctx->www_url,
               curl_easy_strerror(ret));
        if (ctx->fp) {
            fclose(ctx->fp);
        }
        return NULL;
    }

    if (ctx->fp) {
        fclose(ctx->fp);
    }

    struct image* img =
        image_create("<mem>", (uint8_t*)ctx->mem_buf, ctx->mem_buf_sz);
    free(ctx->mem_buf);
    ctx->mem_buf_sz = 0;
    ctx->mem_buf = NULL;

    if (!img) {
        fprintf(
            stderr,
            "Successfuly downloaded image from '%s', but failed to decode it\n",
            ctx->www_url);
    }

    return img;
}
