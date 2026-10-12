#include "pcm.h"

#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__APPLE__) && !defined(ALOHA_USE_OSS)
#include <AudioToolbox/AudioToolbox.h>
#include <CoreFoundation/CoreFoundation.h>
#include <TargetConditionals.h>
#include <stdatomic.h>

enum { AUDIO_BUFFER_COUNT = 3, AUDIO_BUFFER_FRAMES = 4096 };

struct playback {
    const float *samples;
    size_t frames;
    atomic_size_t next_frame;
    atomic_int error;
    atomic_int draining;
    atomic_int finished;
    atomic_int cancelled;
    atomic_uint copying;
};

static void
queue_error(struct playback *p, OSStatus status)
{
    int empty = 0;
    if (status != noErr)
        atomic_compare_exchange_strong(&p->error, &empty, (int)status);
}

/* Queue callbacks reuse these small buffers; no synthesis or allocation occurs
 * on the audio thread. The atomic offset also avoids shared-counter races. */
static int
enqueue_next(struct playback *p, AudioQueueRef queue, AudioQueueBufferRef buffer)
{
    size_t first, count;
    OSStatus status;

    atomic_fetch_add(&p->copying, 1);
    if (atomic_load(&p->cancelled)) {
        atomic_fetch_sub(&p->copying, 1);
        return 0;
    }
    first = atomic_fetch_add(&p->next_frame, AUDIO_BUFFER_FRAMES);
    if (first >= p->frames) {
        atomic_fetch_sub(&p->copying, 1);
        return 0;
    }
    count = p->frames - first;
    if (count > AUDIO_BUFFER_FRAMES)
        count = AUDIO_BUFFER_FRAMES;
    /* This byte count is bounded by AUDIO_BUFFER_FRAMES * 2 * sizeof(float),
     * independent of total song length, and therefore fits UInt32. */
    buffer->mAudioDataByteSize = (UInt32)(count * 2 * sizeof(float));
    memcpy(buffer->mAudioData, p->samples + first * 2,
        buffer->mAudioDataByteSize);
    atomic_fetch_sub(&p->copying, 1);
    status = AudioQueueEnqueueBuffer(queue, buffer, 0, NULL);
    queue_error(p, status);
    return status == noErr;
}

static void
output_callback(void *opaque, AudioQueueRef queue, AudioQueueBufferRef buffer)
{
    struct playback *p = opaque;
    if (atomic_load(&p->cancelled) || atomic_load(&p->draining) ||
        atomic_load(&p->error))
        return;
    if (!enqueue_next(p, queue, buffer) && !atomic_load(&p->error) &&
        !atomic_load(&p->cancelled)) {
        int expected = 0;
        if (atomic_compare_exchange_strong(&p->draining, &expected, 1))
            queue_error(p, AudioQueueStop(queue, false));
    }
}

static void
running_changed(void *opaque, AudioQueueRef queue, AudioQueuePropertyID property)
{
    struct playback *p = opaque;
    UInt32 running = 1;
    UInt32 bytes = sizeof(running);
    OSStatus status;
    (void)property;
    if (atomic_load(&p->cancelled))
        return;
    status = AudioQueueGetProperty(queue, kAudioQueueProperty_IsRunning,
        &running, &bytes);
    queue_error(p, status);
    if (status == noErr && !running && atomic_load(&p->draining))
        atomic_store(&p->finished, 1);
}

#if TARGET_OS_IPHONE
/* iOS mutes the default session category when the ring/silent switch is set
 * to silent. Media playback is not muted. The C AudioSession API is
 * deprecated but still controls the same session as AVAudioSession, without
 * linking anything beyond AudioToolbox. Failure here is not fatal. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
static int
playback_session(int active)
{
    UInt32 category = kAudioSessionCategory_MediaPlayback;
    OSStatus status;

    if (!active)
        return AudioSessionSetActive(false) == noErr;
    status = AudioSessionInitialize(NULL, NULL, NULL, NULL);
    if (status == noErr || status == kAudioSessionAlreadyInitialized)
        status = AudioSessionSetProperty(kAudioSessionProperty_AudioCategory,
            sizeof(category), &category);
    if (status == noErr)
        status = AudioSessionSetActive(true);
    if (status != noErr)
        fprintf(stderr, "audio: cannot select media playback session "
            "(OSStatus %d); the silent switch may mute playback\n", (int)status);
    return status == noErr;
}
#pragma GCC diagnostic pop
#endif

int
pcm_play(const float *stereo, size_t frames, unsigned rate,
    const char *device, AudioProgress progress, void *context)
{
    struct playback *p = calloc(1, sizeof(*p));
    AudioStreamBasicDescription format = { 0 };
    AudioQueueRef queue = NULL;
    CFStringRef uid = NULL;
    OSStatus status;
    int listener_added = 0, result = 1;
#if TARGET_OS_IPHONE
    int session_active;
#endif
    double start, deadline;
    size_t last_position = 0;
    int position_started = 0;
    const char *operation = "AudioQueueNewOutput";

    if (p == NULL) {
        fprintf(stderr, "audio: cannot allocate playback state\n");
        return 1;
    }
    p->samples = stereo;
    p->frames = frames;
    atomic_init(&p->next_frame, 0);
    atomic_init(&p->error, 0);
    atomic_init(&p->draining, 0);
    atomic_init(&p->finished, 0);
    atomic_init(&p->cancelled, 0);
    atomic_init(&p->copying, 0);
    format.mSampleRate = rate;
    format.mFormatID = kAudioFormatLinearPCM;
    format.mFormatFlags = kAudioFormatFlagsNativeFloatPacked;
    format.mBytesPerPacket = 2 * sizeof(float);
    format.mFramesPerPacket = 1;
    format.mBytesPerFrame = 2 * sizeof(float);
    format.mChannelsPerFrame = 2;
    format.mBitsPerChannel = CHAR_BIT * sizeof(float);
#if TARGET_OS_IPHONE
    session_active = playback_session(1);
#endif
    status = AudioQueueNewOutput(&format, output_callback, p, NULL, NULL, 0,
        &queue);
    if (status != noErr)
        goto status_error;
    if (device != NULL && device[0] != '\0' && strcmp(device, "default")) {
        uid = CFStringCreateWithCString(NULL, device, kCFStringEncodingUTF8);
        if (uid == NULL) {
            fprintf(stderr, "audio: invalid or unallocatable output device UID\n");
            goto cleanup;
        }
        operation = "AudioQueueSetProperty(CurrentDevice)";
        status = AudioQueueSetProperty(queue, kAudioQueueProperty_CurrentDevice,
            &uid, sizeof(uid));
        if (status != noErr)
            goto status_error;
    }
    operation = "AudioQueueAddPropertyListener(IsRunning)";
    status = AudioQueueAddPropertyListener(queue, kAudioQueueProperty_IsRunning,
        running_changed, p);
    if (status != noErr)
        goto status_error;
    listener_added = 1;
    for (unsigned i = 0; i < AUDIO_BUFFER_COUNT; ++i) {
        AudioQueueBufferRef buffer = NULL;
        if (atomic_load(&p->next_frame) >= frames)
            break;
        operation = "AudioQueueAllocateBuffer";
        status = AudioQueueAllocateBuffer(queue,
            AUDIO_BUFFER_FRAMES * 2 * sizeof(float), &buffer);
        if (status != noErr)
            goto status_error;
        if (!enqueue_next(p, queue, buffer)) {
            operation = "AudioQueueEnqueueBuffer";
            status = (OSStatus)atomic_load(&p->error);
            goto status_error;
        }
    }
    start = pcm_now();
    if (start < 0.0) {
        fprintf(stderr, "audio: cannot read monotonic clock: %s\n", strerror(errno));
        goto cleanup;
    }
    deadline = start + (double)frames / rate + 10.0;
    operation = "AudioQueueStart";
    status = AudioQueueStart(queue, NULL);
    if (status != noErr)
        goto status_error;
    while (!atomic_load(&p->finished)) {
        double now;
        status = (OSStatus)atomic_load(&p->error);
        if (status != noErr) {
            operation = "AudioQueue callback";
            goto status_error;
        }
        if (progress != NULL) {
            AudioTimeStamp position = {0};
            status = AudioQueueGetCurrentTime(queue, NULL, &position, NULL);
            /* The queue may still be starting on the first poll. Never use
             * submitted buffers or wall time as an audible playback position. */
            if (status != noErr && status != kAudioQueueErr_InvalidRunState) {
                operation = "AudioQueueGetCurrentTime";
                goto status_error;
            }
            if (status == noErr && (position.mFlags & kAudioTimeStampSampleTimeValid)
                && isfinite(position.mSampleTime) && position.mSampleTime >= 0) {
                size_t played = position.mSampleTime >= (double)frames ? frames :
                    (size_t)position.mSampleTime;
                if (!position_started || played > last_position) {
                    progress(context, played);
                    last_position = played;
                    position_started = 1;
                }
            }
        }
        /* Property polling supplements the listener if a short queue completes
         * before its property notification is delivered. Stop(false), not a
         * returned buffer, determines when the hardware tail is drained. */
        if (atomic_load(&p->draining)) {
            UInt32 running = 1, bytes = sizeof(running);
            operation = "AudioQueueGetProperty(IsRunning)";
            status = AudioQueueGetProperty(queue, kAudioQueueProperty_IsRunning,
                &running, &bytes);
            if (status != noErr)
                goto status_error;
            if (!running)
                break;
        }
        now = pcm_now();
        if (now < 0.0 || now > deadline) {
            fprintf(stderr, "audio: playback did not finish before its deadline\n");
            goto cleanup;
        }
        pcm_pause();
    }
    status = (OSStatus)atomic_load(&p->error);
    if (status != noErr) {
        operation = "AudioQueue callback";
        goto status_error;
    }
    if (progress != NULL && (!position_started || last_position < frames))
        progress(context, frames);
    result = 0;
    goto cleanup;

status_error:
    fprintf(stderr, "audio: %s failed (OSStatus %d)\n", operation, (int)status);
cleanup:
    atomic_store(&p->cancelled, 1);
    if (queue != NULL) {
        if (listener_added) {
            status = AudioQueueRemovePropertyListener(queue,
                kAudioQueueProperty_IsRunning, running_changed, p);
            if (status != noErr) {
                fprintf(stderr, "audio: removing playback listener failed (%d)\n",
                    (int)status);
                result = 1;
            }
        }
        /* On success the asynchronous stop has already drained. Immediate
         * disposal now only frees resources; on error it cancels playback. */
        status = AudioQueueDispose(queue, true);
        if (status != noErr) {
            fprintf(stderr, "audio: disposing output queue failed (%d)\n",
                (int)status);
            result = 1;
            /* A failed disposal cannot promise callbacks have ceased. Retain
             * their cancelled context, and wait out any sample copy so the
             * caller can safely release its PCM. Each copy is at most 32 KiB. */
            while (atomic_load(&p->copying) != 0)
                pcm_pause();
            p = NULL;
        }
    }
    if (uid != NULL)
        CFRelease(uid);
#if TARGET_OS_IPHONE
    /* Let audio interrupted by this session resume. */
    if (session_active)
        playback_session(0);
#endif
    free(p);
    return result;
}


#endif
