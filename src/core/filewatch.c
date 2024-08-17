/*
* Copyright (c) 2024 Calvin Rose
*
* Permission is hereby granted, free of charge, to any person obtaining a copy
* of this software and associated documentation files (the "Software"), to
* deal in the Software without restriction, including without limitation the
* rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
* sell copies of the Software, and to permit persons to whom the Software is
* furnished to do so, subject to the following conditions:
*
* The above copyright notice and this permission notice shall be included in
* all copies or substantial portions of the Software.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
* IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
* AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
* LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
* FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
* IN THE SOFTWARE.
*/

#ifndef JANET_AMALG
#include "features.h"
#include <janet.h>
#include "util.h"
#endif

#ifdef JANET_EV
#ifdef JANET_FILEWATCH

#ifdef JANET_LINUX
#include <sys/inotify.h>
#include <unistd.h>
#endif

#ifdef JANET_WINDOWS
#include <windows.h>
#endif

typedef struct {
    const char *name;
    uint32_t flag;
} JanetWatchFlagName;

typedef struct {
#ifndef JANET_WINDOWS
    JanetStream *stream;
#endif
    JanetTable watch_descriptors;
    JanetChannel *channel;
    uint32_t default_flags;
} JanetWatcher;

/* Reject certain filename events without sending anything to the channel
 * to make things faster and not waste time and memory creating events. This
 * should also let us watch only certain file names, patterns, etc. */
static int janet_watch_filter(JanetWatcher *watcher, Janet filename, int wd) {
    /* TODO - add filtering */
    (void) watcher;
    (void) filename;
    (void) wd;
    return 0;
}

#ifdef JANET_LINUX

#include <sys/inotify.h>
#include <unistd.h>

static const JanetWatchFlagName watcher_flags_linux[] = {
    {"access", IN_ACCESS},
    {"all", IN_ALL_EVENTS},
    {"attrib", IN_ATTRIB},
    {"close-nowrite", IN_CLOSE_NOWRITE},
    {"close-write", IN_CLOSE_WRITE},
    {"create", IN_CREATE},
    {"delete", IN_DELETE},
    {"delete-self", IN_DELETE_SELF},
    {"ignored", IN_OPEN},
    {"modify", IN_MODIFY},
    {"move-self", IN_MOVE_SELF},
    {"moved-from", IN_MOVED_FROM},
    {"moved-to", IN_MOVED_TO},
    {"open", IN_OPEN},
    {"q-overflow", IN_Q_OVERFLOW},
    {"unmount", IN_UNMOUNT},
};

static uint32_t decode_watch_flags(const Janet *options, int32_t n) {
    uint32_t flags = 0;
    for (int32_t i = 0; i < n; i++) {
        if (!(janet_checktype(options[i], JANET_KEYWORD))) {
            janet_panicf("expected keyword, got %v", options[i]);
        }
        JanetKeyword keyw = janet_unwrap_keyword(options[i]);
        const JanetWatchFlagName *result = janet_strbinsearch(watcher_flags_linux,
                sizeof(watcher_flags_linux) / sizeof(JanetWatchFlagName),
                sizeof(JanetWatchFlagName),
                keyw);
        if (!result) {
            janet_panicf("unknown inotify flag %v", options[i]);
        }
        flags |= result->flag;
    }
    return flags;
}

static void janet_watcher_init(JanetWatcher *watcher, JanetChannel *channel, uint32_t default_flags) {
    int fd;
    do {
        fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    } while (fd == -1 && errno == EINTR);
    if (fd == -1) {
        janet_panicv(janet_ev_lasterr());
    }
    janet_table_init_raw(&watcher->watch_descriptors, 0);
    watcher->channel = channel;
    watcher->default_flags = default_flags;
    watcher->stream = janet_stream(fd, JANET_STREAM_READABLE, NULL);
}

static void janet_watcher_add(JanetWatcher *watcher, const char *path, uint32_t flags) {
    if (watcher->stream == NULL) janet_panic("watcher closed");
    int result;
    do {
        result = inotify_add_watch(watcher->stream->handle, path, flags);
    } while (result == -1 && errno == EINTR);
    if (result == -1) {
        janet_panicv(janet_ev_lasterr());
    }
    Janet name = janet_cstringv(path);
    Janet wd = janet_wrap_integer(result);
    janet_table_put(&watcher->watch_descriptors, name, wd);
    janet_table_put(&watcher->watch_descriptors, wd, name);
}

static void janet_watcher_remove(JanetWatcher *watcher, const char *path) {
    if (watcher->stream == NULL) janet_panic("watcher closed");
    Janet check = janet_table_get(&watcher->watch_descriptors, janet_cstringv(path));
    janet_assert(janet_checktype(check, JANET_NUMBER), "bad watch descriptor");
    int watch_handle = janet_unwrap_integer(check);
    int result;
    do {
        result = inotify_rm_watch(watcher->stream->handle, watch_handle);
    } while (result != -1 && errno == EINTR);
    if (result == -1) {
        janet_panicv(janet_ev_lasterr());
    }
}

static void watcher_callback_read(JanetFiber *fiber, JanetAsyncEvent event) {
    JanetStream *stream = fiber->ev_stream;
    JanetWatcher *watcher = (JanetWatcher *) fiber->ev_state;
    char buf[1024];
    switch (event) {
        default:
            break;
        case JANET_ASYNC_EVENT_MARK:
            janet_mark(janet_wrap_abstract(watcher));
            break;
        case JANET_ASYNC_EVENT_CLOSE:
            janet_schedule(fiber, janet_wrap_nil());
            fiber->ev_state = NULL;
            janet_async_end(fiber);
            break;
        case JANET_ASYNC_EVENT_ERR:
            {
                janet_schedule(fiber, janet_wrap_nil());
                fiber->ev_state = NULL;
                janet_async_end(fiber);
                break;
            }
    read_more:
    case JANET_ASYNC_EVENT_HUP:
    case JANET_ASYNC_EVENT_INIT:
    case JANET_ASYNC_EVENT_READ:
            {
                Janet name = janet_wrap_nil();

                /* Assumption - read will never return partial events *
                 * From documentation:
                 *
                 * The behavior when the buffer given to read(2) is too small to
                 * return information about the next event depends on the kernel
                 * version: before Linux 2.6.21, read(2) returns 0; since Linux
                 * 2.6.21, read(2) fails with the error EINVAL.  Specifying a buffer
                 * of size
                 *
                 *     sizeof(struct inotify_event) + NAME_MAX + 1
                 *
                 * will be sufficient to read at least one event. */
                ssize_t nread;
                do {
                    nread = read(stream->handle, buf, sizeof(buf));
                } while (nread == -1 && errno == EINTR);

                /* Check for errors - special case errors that can just be waited on to fix */
                if (nread == -1) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK) {
                        break;
                    }
                    janet_cancel(fiber, janet_ev_lasterr());
                    fiber->ev_state = NULL;
                    janet_async_end(fiber);
                    break;
                }
                if (nread < (ssize_t) sizeof(struct inotify_event)) break;

                /* Iterate through all events read from the buffer */
                char *cursor = buf;
                while (cursor < buf + nread) {
                    struct inotify_event inevent;
                    memcpy(&inevent, cursor, sizeof(inevent));
                    cursor += sizeof(inevent);
                    /* Read path of inevent */
                    if (inevent.len) {
                        name = janet_cstringv(cursor);
                        cursor += inevent.len;
                    }

                    /* Filter events by pattern */
                    if (!janet_watch_filter(watcher, name, inevent.wd)) continue;

                    /* Got an event */
                    Janet path = janet_table_get(&watcher->watch_descriptors, janet_wrap_integer(inevent.wd));
                    JanetKV *event = janet_struct_begin(6);
                    janet_struct_put(event, janet_ckeywordv("wd"), janet_wrap_integer(inevent.wd));
                    janet_struct_put(event, janet_ckeywordv("wd-path"), path);
                    janet_struct_put(event, janet_ckeywordv("mask"), janet_wrap_integer(inevent.mask));
                    janet_struct_put(event, janet_ckeywordv("path"), name);
                    janet_struct_put(event, janet_ckeywordv("cookie"), janet_wrap_integer(inevent.cookie));
                    Janet etype = janet_ckeywordv("type");
                    const JanetWatchFlagName *wfn_end = watcher_flags_linux + sizeof(watcher_flags_linux) / sizeof(watcher_flags_linux[0]);
                    for (const JanetWatchFlagName *wfn = watcher_flags_linux; wfn < wfn_end; wfn++) {
                        if ((inevent.mask & wfn->flag) == wfn->flag) janet_struct_put(event, etype, janet_ckeywordv(wfn->name));
                    }
                    Janet eventv = janet_wrap_struct(janet_struct_end(event));

                    janet_channel_give(watcher->channel, eventv);
                }

                /* Read some more if possible */
                goto read_more;
            }
            break;
    }
}

static void janet_watcher_listen(JanetWatcher *watcher) {
    janet_async_start(watcher->stream, JANET_ASYNC_LISTEN_READ, watcher_callback_read, watcher);
}

#elif JANET_WINDOWS

static const JanetWatchFlagName watcher_flags_windows[] = {
    {"file-name", FILE_NOTIFY_CHANGE_FILE_NAME},
    {"dir-name", FILE_NOTIFY_CHANGE_DIR_NAME},
    {"attributes", FILE_NOTIFY_CHANGE_ATTRIBUTES},
    {"size", FILE_NOTIFY_CHANGE_SIZE},
    {"last-write", FILE_NOTIFY_CHANGE_LAST_WRITE},
    {"last-access", FILE_NOTIFY_CHANGE_LAST_ACCESS},
    {"creation", FILE_NOTIFY_CHANGE_CREATION},
    {"security", FILE_NOTIFY_CHANGE_SECURITY}
};

static uint32_t decode_watch_flags(const Janet *options, int32_t n) {
    uint32_t flags = 0;
    for (int32_t i = 0; i < n; i++) {
        if (!(janet_checktype(options[i], JANET_KEYWORD))) {
            janet_panicf("expected keyword, got %v", options[i]);
        }
        JanetKeyword keyw = janet_unwrap_keyword(options[i]);
        const JanetWatchFlagName *result = janet_strbinsearch(watcher_flags_windows,
                sizeof(watcher_flags_windows) / sizeof(JanetWatchFlagName),
                sizeof(JanetWatchFlagName),
                keyw);
        if (!result) {
            janet_panicf("unknown windows filewatch flag %v", options[i]);
        }
        flags |= result->flag;
    }
    return flags;
}

static void janet_watcher_init(JanetWatcher *watcher, JanetChannel *channel, uint32_t default_flags) {
    janet_table_init_raw(&watcher->watch_descriptors, 0);
    watcher->channel = channel;
    watcher->default_flags = default_flags;
}

typedef struct {
    JanetStream stream;
    OVERLAPPED overlapped;
    uint32_t flags;
    FILE_NOTIFY_INFORMATION fni;
} OverlappedWatch;

static void janet_watcher_add(JanetWatcher *watcher, const char *path, uint32_t flags) {
    HANDLE handle = CreateFileA(path, GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            NULL,
            OPEN_EXISTING,
            FILE_FLAG_OVERLAPPED | FILE_ATTRIBUTE_NORMAL,
            0);
    if (handle == INVALID_HANDLE_VALUE) {
        janet_panicv(janet_ev_lasterr());
    }
    JanetStream *stream = janet_stream_ext(handle, 0, NULL, sizeof(OverlappedWatch));
    Janet pathv = janet_cstringv(path);
    stream->flags = flags | watcher->default_flags;
    Janet streamv = janet_wrap_abstract(stream);
    janet_table_put(&watcher->watch_descriptors, pathv, streamv);
    janet_table_put(&watcher->watch_descriptors, streamv, pathv);
    /* TODO - if listening, also listen for this new path */
}

static void janet_watcher_remove(JanetWatcher *watcher, const char *path) {
    Janet pathv = janet_cstringv(path);
    Janet streamv = janet_table_get(&watcher->watch_descriptors, pathv);
    if (janet_checktype(streamv, JANET_NIL)) {
        janet_panicf("path %v is not being watched", pathv);
    }
    janet_table_remove(&watcher->watch_descriptors, pathv);
    janet_table_remove(&watcher->watch_descriptors, streamv);
    OverlappedWatch *ow = janet_unwrap_abstract(streamv);
    janet_stream_close((JanetStream *) ow);
}

static void watcher_callback_read(JanetFiber *fiber, JanetAsyncEvent event) {
    JanetWatcher *watcher = (JanetWatcher *) fiber->ev_state;
    char buf[1024];
    switch (event) {
        default:
            break;
        case JANET_ASYNC_EVENT_MARK:
            janet_mark(janet_wrap_abstract(watcher));
            break;
        case JANET_ASYNC_EVENT_CLOSE:
            janet_schedule(fiber, janet_wrap_nil());
            fiber->ev_state = NULL;
            janet_async_end(fiber);
            break;
        case JANET_ASYNC_EVENT_ERR:
            {
                janet_schedule(fiber, janet_wrap_nil());
                fiber->ev_state = NULL;
                janet_async_end(fiber);
                break;
            }
            break;
    }
}

static void janet_watcher_listen(JanetWatcher *watcher) {
    for (int32_t i = 0; i < watcher->watch_descriptors.capacity; i++) {
        const JanetKV *kv = watcher->watch_descriptors.items + i;
        if (!janet_checktype(kv->key, JANET_POINTER)) continue;
        OverlappedWatch *ow = janet_unwrap_pointer(kv->key);
        Janet pathv = kv->value;
        BOOL result = ReadDirecoryChangesW(ow->handle,
                &ow->fni,
                sizeof(FILE_NOTIFY_INFORMATION),
                TRUE,
                ow->flags,
                NULL,
                &ow->overlapped,
                NULL);
        if (!result) {
            janet_panicv(janet_ev_lasterr());
        }
    }
}

#else

/* Default implementation */

static uint32_t decode_watch_flags(const Janet *options, int32_t n) {
    (void) options;
    (void) n;
    return 0;
}

static void janet_watcher_init(JanetWatcher *watcher, JanetChannel *channel, uint32_t default_flags) {
    (void) watcher;
    (void) channel;
    (void) default_flags;
    janet_panic("filewatch not supported on this platform");
}

static void janet_watcher_add(JanetWatcher *watcher, const char *path, uint32_t flags) {
    (void) watcher;
    (void) flags;
    (void) path;
    janet_panic("nyi");
}

static void janet_watcher_remove(JanetWatcher *watcher, const char *path) {
    (void) watcher;
    (void) path;
    janet_panic("nyi");
}

static void janet_watcher_listen(JanetWatcher *watcher) {
    (void) watcher;
    janet_panic("nyi");
}

#endif

/* C Functions */

static int janet_filewatch_mark(void *p, size_t s) {
    JanetWatcher *watcher = (JanetWatcher *) p;
    (void) s;
    if (watcher->stream == NULL) return 0; /* Incomplete initialization */
#ifndef JANET_WINDOWS
    janet_mark(janet_wrap_abstract(watcher->stream));
#endif
    janet_mark(janet_wrap_abstract(watcher->channel));
    janet_mark(janet_wrap_table(&watcher->watch_descriptors));
    return 0;
}

static int janet_filewatch_gc(void *p, size_t s) {
    JanetWatcher *watcher = (JanetWatcher *) p;
    if (watcher->stream == NULL) return 0; /* Incomplete initialization */
    (void) s;
    janet_table_deinit(&watcher->watch_descriptors);
    return 0;
}

static const JanetAbstractType janet_filewatch_at = {
    "filewatch/watcher",
    janet_filewatch_gc,
    janet_filewatch_mark,
    JANET_ATEND_GCMARK
};

JANET_CORE_FN(cfun_filewatch_make,
        "(filewatch/make channel &opt default-flags)",
        "Create a new filewatcher that will give events to a channel channel.") {
    janet_arity(argc, 1, -1);
    JanetChannel *channel = janet_getchannel(argv, 0);
    JanetWatcher *watcher = janet_abstract(&janet_filewatch_at, sizeof(JanetWatcher));
    uint32_t default_flags = decode_watch_flags(argv + 1, argc - 1);
    janet_watcher_init(watcher, channel, default_flags);
    return janet_wrap_abstract(watcher);
}

JANET_CORE_FN(cfun_filewatch_add,
        "(filewatch/add watcher path &opt flags)",
        "Add a path to the watcher.") {
    janet_arity(argc, 2, -1);
    JanetWatcher *watcher = janet_getabstract(argv, 0, &janet_filewatch_at);
    const char *path = janet_getcstring(argv, 1);
    uint32_t flags = watcher->default_flags | decode_watch_flags(argv + 2, argc - 2);
    janet_watcher_add(watcher, path, flags);
    return argv[0];
}

JANET_CORE_FN(cfun_filewatch_remove,
        "(filewatch/remove watcher path)",
        "Remove a path from the watcher.") {
    janet_fixarity(argc, 2);
    JanetWatcher *watcher = janet_getabstract(argv, 0, &janet_filewatch_at);
    const char *path = janet_getcstring(argv, 1);
    janet_watcher_remove(watcher, path);
    return argv[0];
}

JANET_CORE_FN(cfun_filewatch_listen,
        "(filewatch/listen watcher)",
        "Listen for changes in the watcher.") {
    janet_fixarity(argc, 1);
    JanetWatcher *watcher = janet_getabstract(argv, 0, &janet_filewatch_at);
    janet_watcher_listen(watcher);
    return janet_wrap_nil();
}

/* Module entry point */
void janet_lib_filewatch(JanetTable *env) {
    JanetRegExt cfuns[] = {
        JANET_CORE_REG("filewatch/make", cfun_filewatch_make),
        JANET_CORE_REG("filewatch/add", cfun_filewatch_add),
        JANET_CORE_REG("filewatch/remove", cfun_filewatch_remove),
        JANET_CORE_REG("filewatch/listen", cfun_filewatch_listen),
        JANET_REG_END
    };
    janet_core_cfuns_ext(env, NULL, cfuns);
}

#endif
#endif
