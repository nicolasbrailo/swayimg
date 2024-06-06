#include "imageprefetcher.h"

#include "image.h"

#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// bg thread for the prefetcher
static void* image_prefetcher_thread(void* usr);

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

/**
 * Creates a prefetcher and returns a handle. Will return NULL if creating a
 * prefetcher failed
 */
struct image_prefetcher_ctx* image_prefetcher_init(downloader_cb cb,
                                                   void* downloader_impl_usr)
{
    struct image_prefetcher_ctx* ctx =
        malloc(sizeof(struct image_prefetcher_ctx));
    if (!ctx) {
        return NULL;
    }

    ctx->downloader_impl = cb;
    ctx->downloader_impl_usr = downloader_impl_usr;

    ctx->thread = 0;

    ctx->image_cache_size = 0;
    ctx->image_cache_r = 0;
    ctx->image_cache_w = 0;
    ctx->prefetch_n = 0;
    ctx->image_cache = NULL;

    if (pthread_cond_init(&ctx->condvar, NULL) != 0) {
        perror("pthread_cond_init");
        free(ctx);
        return NULL;
    }

    if (pthread_mutex_init(&ctx->cv_mut, NULL) != 0) {
        perror("pthread_mutex_init");
        pthread_cond_destroy(&ctx->condvar);
        free(ctx);
        return NULL;
    }

    return ctx;
}

bool image_prefetcher_start(struct image_prefetcher_ctx* ctx, size_t cache_size,
                            size_t prefetch_n)
{
    ctx->image_cache_size = cache_size;
    ctx->prefetch_n = prefetch_n;

    if (ctx->thread || ctx->image_cache_r != 0 || ctx->image_cache_w != 0) {
        fprintf(stderr,
                "BUG: image_prefetcher used before start or started twice\n");
        abort();
    }

    if (ctx->image_cache_size == 0) {
        fprintf(stderr, "Can't use prefetcher with cache size = 0\n");
        return false;
    }

    if (ctx->prefetch_n == 0) {
        fprintf(stderr, "Can't use prefetcher with prefetch count = 0\n");
        return false;
    }

    if (ctx->prefetch_n > ctx->image_cache_size) {
        fprintf(stderr,
                "Prefetcher has prefetch count = %lu and max cache size = %lu. "
                "Will set max prefetch to cache size.\n",
                ctx->prefetch_n, ctx->image_cache_size);
        ctx->prefetch_n = ctx->image_cache_size;
    }

    ctx->image_cache = malloc(sizeof(void*) * ctx->image_cache_size);
    if (!ctx->image_cache) {
        fprintf(stderr, "Bad alloc, can't create prefetcher\n");
        return false;
    }

    memset(ctx->image_cache, 0, sizeof(void*) * ctx->image_cache_size);

    if (pthread_create(&ctx->thread, NULL, image_prefetcher_thread, ctx) != 0) {
        perror("pthread_create");
        free(ctx->image_cache);
        ctx->image_cache = NULL;
        return false;
    }

    return true;
}

void image_prefetcher_free(struct image_prefetcher_ctx* ctx)
{
    if (!ctx) {
        return;
    }

    if (ctx->thread) {
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
    }

    for (size_t i = 0; i < ctx->image_cache_size; ++i) {
        image_free(ctx->image_cache[i]);
    }

    free(ctx->image_cache);
    free(ctx);
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
        const size_t cnt = image_prefetcher_get_cached(ctx);
        for (size_t i = cnt; i < ctx->prefetch_n; ++i) {
            pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);

            // Download one (may be slow)
            struct image* img = ctx->downloader_impl(ctx->downloader_impl_usr);

            // Critical section: update buffer
            pthread_mutex_lock(&ctx->cv_mut);
            struct image* old_img = ctx->image_cache[ctx->image_cache_w];
            const size_t old_w_pos = ctx->image_cache_w;
            ctx->image_cache[ctx->image_cache_w] = img;
            ctx->image_cache_w =
                (ctx->image_cache_w + 1) % ctx->image_cache_size;
            pthread_mutex_unlock(&ctx->cv_mut);

            // If the old position in the ringbuffer had an image, relase it
            if (old_img) {
                printf("Expire cached image w_old=%lu [%p]\n", old_w_pos,
                       (void*)old_img);
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

size_t image_prefetcher_get_cached(struct image_prefetcher_ctx* ctx)
{
    if (pthread_mutex_lock(&ctx->cv_mut) != 0) {
        return 0;
    }

    const size_t cnt = (ctx->image_cache_w >= ctx->image_cache_r)
        ? ctx->image_cache_w - ctx->image_cache_r
        : ctx->image_cache_w + ctx->image_cache_size - ctx->image_cache_r;
    pthread_mutex_unlock(&ctx->cv_mut);

    return cnt;
}

struct image* image_prefetcher_jump_next(struct image_prefetcher_ctx* ctx)
{
    if (pthread_mutex_lock(&ctx->cv_mut) != 0) {
        perror("pthread_mutex_lock");
        return NULL;
    }

    struct image* ret = NULL;
    const size_t next_idx = (ctx->image_cache_r + 1) % ctx->image_cache_size;
    const bool buffer_empty = ctx->image_cache_w == ctx->image_cache_r;
    const bool would_move_over_write_ptr = next_idx == ctx->image_cache_w;
    if (buffer_empty) {
        printf("No more images available, cache empty.\n");
    } else if (would_move_over_write_ptr) {
        // We reached the last available image, and the downloader hasn't had
        // time to catch up yet. Stay in the current image.
        ret = ctx->image_cache[ctx->image_cache_r];
        fprintf(stderr,
                "Reached last available image, waiting for more cache "
                "R=%lu=%p, N=%lu=%p, W=%lu\n",
                ctx->image_cache_r, (void*)ctx->image_cache[ctx->image_cache_r],
                next_idx, (void*)ctx->image_cache[next_idx],
                ctx->image_cache_w);
    } else {
        ctx->image_cache_r = next_idx;
        ret = ctx->image_cache[next_idx];
    }

    if (pthread_mutex_unlock(&ctx->cv_mut)) {
        perror("pthread_mutex_unlock");
    }

    // Wake up the background thread (it may or may not need to download new
    // files)
    if (pthread_cond_broadcast(&ctx->condvar) != 0) {
        perror("pthread_cond_broadcast");
    }

    return ret;
}

struct image* image_prefetcher_jump_prev(struct image_prefetcher_ctx* ctx)
{
    if (pthread_mutex_lock(&ctx->cv_mut) != 0) {
        perror("pthread_mutex_lock");
        return NULL;
    }

    const size_t maybe_prev = ctx->image_cache_r == 0
        ? ctx->image_cache_size - 1
        : ctx->image_cache_r - 1;
    if (maybe_prev == ctx->image_cache_w || !ctx->image_cache[maybe_prev]) {
        printf("No more images available, reached oldest image in history.\n");
        pthread_mutex_unlock(&ctx->cv_mut);
        return NULL;
    }

    ctx->image_cache_r = maybe_prev;
    if (pthread_mutex_unlock(&ctx->cv_mut) != 0) {
        perror("pthread_mutex_unlock");
    }

    // We don't signal the condvar because it should never be necessary to
    // prefetch a new image
    return ctx->image_cache[maybe_prev];
}
