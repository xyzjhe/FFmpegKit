#include "include/http_range_cache.h"

#if defined(__APPLE__)
#include <Libavutil/error.h>
#include <Libavutil/mem.h>
#else
#include <libavutil/error.h>
#include <libavutil/mem.h>
#endif
#include <errno.h>
#include <stdio.h>
#include <string.h>

#define RANGE_SIZE (1024 * 1024)
#define RANGE_COUNT 4

typedef struct Range {
    int64_t offset;
    int length;
    uint8_t *data;
} Range;

typedef struct RangeCache {
    AVIOContext *source;
    AVIOContext *context;
    AVIOInterruptCB interrupt;
    int64_t size;
    int64_t position;
    Range ranges[RANGE_COUNT]; /* oldest first */
    int count;
} RangeCache;

static int interrupted(const RangeCache *cache)
{
    return cache->interrupt.callback &&
           cache->interrupt.callback(cache->interrupt.opaque);
}

static int find_range(const RangeCache *cache, int64_t position)
{
    for (int i = 0; i < cache->count; i++) {
        const Range *r = &cache->ranges[i];
        if (position >= r->offset && position - r->offset < r->length)
            return i;
    }
    return -1;
}

static int range_read(void *opaque, uint8_t *buffer, int length)
{
    RangeCache *cache = opaque;
    if (interrupted(cache))
        return AVERROR_EXIT;
    if (length <= 0)
        return AVERROR(EINVAL);
    if (cache->position == cache->size)
        return AVERROR_EOF;

    int index = find_range(cache, cache->position);
    Range range;
    if (index >= 0) {
        range = cache->ranges[index];
        memmove(&cache->ranges[index], &cache->ranges[index + 1],
                (cache->count - index - 1) * sizeof(Range));
        cache->count--;
    } else {
        if (cache->count == RANGE_COUNT) {
            av_free(cache->ranges[0].data);
            memmove(&cache->ranges[0], &cache->ranges[1],
                    (RANGE_COUNT - 1) * sizeof(Range));
            cache->count--;
        }
        /* Do not align backwards after a short read: that could repeat the
         * already-returned prefix forever. Read exactly at the logical cursor.
         */
        if (avio_tell(cache->source) != cache->position) {
            int64_t result = avio_seek(cache->source, cache->position, SEEK_SET);
            if (result < 0)
                return (int)result;
            cache->source->error = 0;
        }
        int wanted = (int)((cache->size - cache->position) < RANGE_SIZE ?
                          (cache->size - cache->position) : RANGE_SIZE);
        range.data = av_malloc(wanted);
        if (!range.data)
            return AVERROR(ENOMEM);
        int result = avio_read(cache->source, range.data, wanted);
        if (interrupted(cache) || result <= 0) {
            av_free(range.data);
            if (interrupted(cache))
                return AVERROR_EXIT;
            /* EOF before the advertised end is a truncated/error response,
             * never successful playback completion.
             */
            return result == AVERROR_EOF || result == 0 ? AVERROR(EIO) : result;
        }
        range.offset = cache->position;
        range.length = result;
    }
    cache->ranges[cache->count++] = range;
    int offset = (int)(cache->position - range.offset);
    int count = range.length - offset;
    if (count > length)
        count = length;
    memcpy(buffer, range.data + offset, count);
    cache->position += count;
    return count;
}

static int64_t range_seek(void *opaque, int64_t offset, int whence)
{
    RangeCache *cache = opaque;
    if (interrupted(cache))
        return AVERROR_EXIT;
    whence &= ~AVSEEK_FORCE;
    if (whence == AVSEEK_SIZE)
        return cache->size;
    int64_t base;
    switch (whence) {
    case SEEK_SET: base = 0; break;
    case SEEK_CUR: base = cache->position; break;
    case SEEK_END: base = cache->size; break;
    default: return AVERROR(EINVAL);
    }
    if ((offset > 0 && base > INT64_MAX - offset) ||
        (offset < 0 && base < INT64_MIN - offset))
        return AVERROR(EINVAL);
    int64_t target = base + offset;
    if (target < 0 || target > cache->size)
        return AVERROR(EINVAL);
    /* Validate a cache miss with the real transport before publishing the seek.
     * Cached bytes and the exact EOF position need no network access.
     */
    if (target != cache->size && find_range(cache, target) < 0) {
        int64_t result = avio_seek(cache->source, target, SEEK_SET);
        if (result < 0)
            return result;
    }
    cache->source->error = 0;
    cache->source->eof_reached = 0;
    // avio_seek clears EOF but leaves a previous read error latched. A
    // successful user seek must not carry an old cancellation into later EOF.
    cache->context->error = 0;
    cache->position = target;
    return target;
}

int ksplayer_http_range_cache_create(AVIOContext **output, AVIOContext *source,
                                    const AVIOInterruptCB *interrupt)
{
    if (!output || *output || !source)
        return AVERROR(EINVAL);
    if (!(source->seekable & AVIO_SEEKABLE_NORMAL))
        return AVERROR(ENOSYS);
    int64_t size = avio_size(source);
    int64_t position = avio_tell(source);
    if (size <= 0 || position < 0 || position > size)
        return AVERROR(ENOSYS);
    RangeCache *cache = av_mallocz(sizeof(*cache));
    uint8_t *buffer = av_malloc(32768);
    if (!cache || !buffer) {
        av_free(cache);
        av_free(buffer);
        return AVERROR(ENOMEM);
    }
    cache->source = source;
    cache->size = size;
    cache->position = position;
    if (interrupt)
        cache->interrupt = *interrupt;
    AVIOContext *context = avio_alloc_context(buffer, 32768, 0, cache,
                                              range_read, NULL, range_seek);
    if (!context) {
        av_free(cache);
        av_free(buffer);
        return AVERROR(ENOMEM);
    }
    context->seekable = source->seekable;
    context->pos = position;
    cache->context = context;
    *output = context;
    return 0;
}

void ksplayer_http_range_cache_free(AVIOContext **context)
{
    if (!context || !*context)
        return;
    RangeCache *cache = (*context)->opaque;
    for (int i = 0; i < cache->count; i++)
        av_free(cache->ranges[i].data);
    avio_closep(&cache->source);
    av_free(cache);
    av_freep(&(*context)->buffer);
    avio_context_free(context);
}

int64_t ksplayer_http_range_cache_bytes(const AVIOContext *context)
{
    if (!context)
        return 0;
    const RangeCache *cache = context->opaque;
    int64_t bytes = 0;
    for (int i = 0; i < cache->count; i++)
        bytes += cache->ranges[i].length;
    return bytes;
}
