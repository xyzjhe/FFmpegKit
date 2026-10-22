#ifndef KSPLAYER_HTTP_RANGE_CACHE_H
#define KSPLAYER_HTTP_RANGE_CACHE_H

#include <stdint.h>
#if defined(__APPLE__)
#include <Libavformat/avio.h>
#else
#include <libavformat/avio.h>
#endif

/* Takes ownership of source only on success. Known-size, seekable inputs only.
 * All callbacks/free run on the demux thread; interrupt must be thread-safe.
 * Retained range data is limited to four 1 MiB windows, independent of file size.
 */
int ksplayer_http_range_cache_create(AVIOContext **output, AVIOContext *source,
                                    const AVIOInterruptCB *interrupt);

/* Call after avformat_close_input; frees both the wrapper and its native source.
 * The caller must retain output separately if avformat_open_input fails.
 */
void ksplayer_http_range_cache_free(AVIOContext **context);
int64_t ksplayer_http_range_cache_bytes(const AVIOContext *context);

#endif
