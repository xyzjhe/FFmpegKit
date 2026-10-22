/* Standalone native regression. Link this and the production cache against
 * matching FFmpeg headers/libraries; no Apple framework or playback mock.
 */
#include "../Sources/FFmpegKit/include/http_range_cache.h"
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libavutil/mem.h>
#include <libavutil/sha.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MIB (1024 * 1024)
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); exit(1); } } while (0)

static AVIOContext *open_file(const char *path)
{
    AVIOContext *source = NULL;
    CHECK(avio_open2(&source, path, AVIO_FLAG_READ, NULL, NULL) == 0);
    return source;
}

static int cancelled;
static int check_interrupt(void *opaque) { return *(int *)opaque; }
static int read_calls;
static int short_read(void *opaque, uint8_t *buffer, int size)
{
    (void)opaque;
    (void)size;
    if (read_calls++ != 0)
        return AVERROR(EAGAIN);
    memset(buffer, 7, 512);
    return 512;
}
static int early_eof(void *opaque, uint8_t *buffer, int size)
{
    (void)opaque;
    (void)buffer;
    (void)size;
    return AVERROR_EOF;
}

static void unit_tests(const char *path)
{
    FILE *file = fopen(path, "wb");
    CHECK(file);
    uint8_t *data = malloc(MIB);
    CHECK(data);
    for (int i = 0; i < 10; i++) {
        memset(data, i, MIB);
        CHECK(fwrite(data, 1, MIB, file) == MIB);
    }
    free(data);
    CHECK(fclose(file) == 0);

    AVIOContext *source = open_file(path), *cache = NULL;
    AVIOInterruptCB interrupt = {check_interrupt, &cancelled};
    CHECK(ksplayer_http_range_cache_create(&cache, source, &interrupt) == 0);
    uint8_t bytes[2048];
    int64_t network_bytes = 0;
    for (int i = 0; i < 16; i++) {
        for (int region = 0; region < 2; region++) {
            CHECK(cache->seek(cache->opaque, region * 8LL * MIB + i * 1024, SEEK_SET) >= 0);
            CHECK(cache->read_packet(cache->opaque, bytes, 1024) == 1024);
            for (int j = 0; j < 1024; j++)
                CHECK(bytes[j] == region * 8);
        }
        if (!i) network_bytes = source->bytes_read;
        CHECK(source->bytes_read == network_bytes);
    }
    CHECK(ksplayer_http_range_cache_bytes(cache) == 2 * MIB);
    for (int i = 1; i < 8; i++) {
        CHECK(cache->seek(cache->opaque, i * (int64_t)MIB, SEEK_SET) == i * (int64_t)MIB);
        CHECK(cache->read_packet(cache->opaque, bytes, 1) == 1 && bytes[0] == i);
        CHECK(ksplayer_http_range_cache_bytes(cache) <= 4 * MIB);
    }
    CHECK(cache->seek(cache->opaque, 0, SEEK_SET) == 0);
    CHECK(cache->read_packet(cache->opaque, bytes, 1) == 1 && bytes[0] == 0);
    CHECK(cache->seek(cache->opaque, MIB - 10, SEEK_SET) == MIB - 10);
    CHECK(cache->read_packet(cache->opaque, bytes, 20) == 10);
    CHECK(cache->read_packet(cache->opaque, bytes, 10) == 10 && bytes[0] == 1);

    CHECK(cache->seek(cache->opaque, -13, SEEK_END) == 10LL * MIB - 13);
    CHECK(cache->read_packet(cache->opaque, bytes, 100) == 13);
    CHECK(cache->read_packet(cache->opaque, bytes, 1) == AVERROR_EOF);
    CHECK(cache->seek(cache->opaque, 0, SEEK_SET) == 0);
    CHECK(cache->read_packet(cache->opaque, bytes, 1) == 1 && bytes[0] == 0);
    CHECK(cache->seek(cache->opaque, -1, SEEK_SET) == AVERROR(EINVAL));
    CHECK(cache->seek(cache->opaque, INT64_MAX, SEEK_CUR) == AVERROR(EINVAL));
    CHECK(cache->seek(cache->opaque, 10LL * MIB + 1, SEEK_SET) == AVERROR(EINVAL));
    CHECK(cache->seek(cache->opaque, 0, AVSEEK_SIZE | AVSEEK_FORCE) == 10LL * MIB);
    cancelled = 1;
    CHECK(cache->read_packet(cache->opaque, bytes, 1) == AVERROR_EXIT);
    CHECK(cache->seek(cache->opaque, 0, SEEK_SET) == AVERROR_EXIT);
    cancelled = 0;
    ksplayer_http_range_cache_free(&cache);
    ksplayer_http_range_cache_free(&cache);
    CHECK(cache == NULL);

    source = open_file(path);
    CHECK(ksplayer_http_range_cache_create(&cache, source, &interrupt) == 0);
    cancelled = 1;
    CHECK(avio_read(cache, bytes, 16) == AVERROR_EXIT);
    cancelled = 0;
    CHECK(avio_seek(cache, 2LL * MIB, SEEK_SET) == 2LL * MIB);
    CHECK(avio_read(cache, bytes, 16) == 16 && bytes[0] == 2);
    CHECK(cache->error == 0);
    ksplayer_http_range_cache_free(&cache);

    source = open_file(path);
    source->seekable = 0;
    CHECK(ksplayer_http_range_cache_create(&cache, source, NULL) == AVERROR(ENOSYS));
    CHECK(cache == NULL && avio_read(source, bytes, 1) == 1);
    avio_closep(&source);
    source = open_file(path);
    int64_t (*seek_fn)(void *, int64_t, int) = source->seek;
    source->seek = NULL;
    CHECK(ksplayer_http_range_cache_create(&cache, source, NULL) == AVERROR(ENOSYS));
    source->seek = seek_fn;
    avio_closep(&source);

    source = open_file(path);
    CHECK(avio_seek(source, 100, SEEK_SET) == 100);
    CHECK(ksplayer_http_range_cache_create(&cache, source, NULL) == 0);
    CHECK(avio_tell(cache) == 100);
    CHECK(avio_seek(cache, 10LL * MIB, SEEK_SET) == 10LL * MIB);
    CHECK(avio_read(cache, bytes, 1) == AVERROR_EOF);
    CHECK(avio_seek(cache, 2LL * MIB, SEEK_SET) == 2LL * MIB);
    CHECK(avio_read(cache, bytes, 16) == 16 && bytes[0] == 2);
    ksplayer_http_range_cache_free(&cache);

    source = open_file(path);
    CHECK(ksplayer_http_range_cache_create(&cache, source, NULL) == 0);
    int (*read_fn)(void *, uint8_t *, int) = source->read_packet;
    source->read_packet = early_eof;
    CHECK(cache->read_packet(cache->opaque, bytes, 1) == AVERROR(EIO));
    source->read_packet = read_fn;
    ksplayer_http_range_cache_free(&cache);

    source = open_file(path);
    CHECK(ksplayer_http_range_cache_create(&cache, source, NULL) == 0);
    read_fn = source->read_packet;
    source->read_packet = short_read;
    CHECK(cache->read_packet(cache->opaque, bytes, 512) == 512 && bytes[0] == 7);
    network_bytes = source->bytes_read;
    CHECK(cache->read_packet(cache->opaque, bytes, 1) == AVERROR(EAGAIN));
    CHECK(source->bytes_read == network_bytes);
    source->read_packet = read_fn;
    CHECK(cache->seek(cache->opaque, 2LL * MIB, SEEK_SET) == 2LL * MIB);
    CHECK(cache->read_packet(cache->opaque, bytes, 1) == 1 && bytes[0] == 2);
    ksplayer_http_range_cache_free(&cache);

    source = open_file(path);
    CHECK(ksplayer_http_range_cache_create(&cache, source, NULL) == 0);
    AVFormatContext *format = avformat_alloc_context();
    CHECK(format);
    format->pb = cache;
    CHECK(avformat_open_input(&format, path, NULL, NULL) < 0);
    CHECK(format == NULL);
    avformat_close_input(&format);
    ksplayer_http_range_cache_free(&cache);
    CHECK(remove(path) == 0);
    puts("{\"unit_scenarios\":12,\"passed\":true}");
}

static void probe(const char *mode, const char *url)
{
    AVFormatContext *format = avformat_alloc_context();
    AVIOContext *source = NULL, *cache = NULL;
    CHECK(format);
    AVDictionary *options = NULL;
    av_dict_set(&options, "headers", "X-KSPlayer-Fixture: allowed\r\n", 0);
    av_dict_set(&options, "rw_timeout", "5000000", 0);
    av_dict_set(&options, "short_seek_size", "32768", 0);
    if (!strcmp(mode, "cached")) {
        CHECK(avio_open2(&source, url, AVIO_FLAG_READ, NULL, &options) == 0);
        CHECK(ksplayer_http_range_cache_create(&cache, source, NULL) == 0);
        format->pb = cache;
    }
    CHECK(avformat_open_input(&format, url, NULL, &options) == 0);
    av_dict_free(&options);
    CHECK(avformat_find_stream_info(format, NULL) >= 0);
    double duration = (double)format->duration / AV_TIME_BASE;
    CHECK(duration > 0);
    double targets[] = {duration * .5, 30, duration - 1, duration * .2, duration * .8, duration - 1, duration * .25};
    AVPacket *packet = av_packet_alloc();
    struct AVSHA *hash = av_sha_alloc();
    CHECK(packet && hash);
    printf("{\"duration\":%.6f,\"windows\":[", duration);
    for (int window = 0; window < 7; window++) {
        int64_t target = (int64_t)(targets[window] * AV_TIME_BASE);
        CHECK(avformat_seek_file(format, -1, INT64_MIN, target, INT64_MAX, AVSEEK_FLAG_BACKWARD) >= 0);
        av_sha_init(hash, 256);
        int count = 0, result = 0;
        double first_video = -1;
        for (; count < 200; count++) {
            result = av_read_frame(format, packet);
            if (result < 0)
                break;
            av_sha_update(hash, (const uint8_t *)&packet->stream_index, sizeof(packet->stream_index));
            av_sha_update(hash, packet->data, packet->size);
            AVStream *stream = format->streams[packet->stream_index];
            if (first_video < 0 && stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO)
                first_video = packet->pts * av_q2d(stream->time_base);
            av_packet_unref(packet);
            CHECK(ksplayer_http_range_cache_bytes(cache) <= 4 * MIB);
        }
        CHECK(count > 0);
        CHECK(result >= 0 || result == AVERROR_EOF);
        uint8_t digest[32];
        av_sha_final(hash, digest);
        printf("%s{\"target\":%.6f,\"first_video\":%.6f,\"packets\":%d,\"eof\":%s,\"sha256\":\"",
               window ? "," : "", targets[window], first_video, count, result == AVERROR_EOF ? "true" : "false");
        for (int i = 0; i < 32; i++) printf("%02x", digest[i]);
        printf("\"}");
    }
    printf("],\"cached_bytes\":%lld}\n", (long long)ksplayer_http_range_cache_bytes(cache));
    av_free(hash);
    av_packet_free(&packet);
    avformat_close_input(&format);
    ksplayer_http_range_cache_free(&cache);
}

int main(int argc, char **argv)
{
    av_log_set_level(getenv("KS_RANGE_TRACE") ? AV_LOG_TRACE : AV_LOG_ERROR);
    CHECK(argc == 3 || argc == 4);
    if (!strcmp(argv[1], "unit") && argc == 3)
        unit_tests(argv[2]);
    else if (!strcmp(argv[1], "probe") && argc == 4)
        probe(argv[2], argv[3]);
    else
        return 2;
    return 0;
}
