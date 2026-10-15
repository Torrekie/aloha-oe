#include "pcm.h"

#if defined(__linux__) && !defined(ALOHA_USE_OSS)
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <sound/asound.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

/* The kernel PCM interface: no libasound, plug-ins or sound-server client. */
static int
alsa_ioctl(int fd, unsigned long request, void *arg)
{
    int result;

    do {
        result = ioctl(fd, request, arg);
    } while (result < 0 && errno == EINTR);
    return result;
}

static struct snd_interval *
hw_interval(struct snd_pcm_hw_params *hw, unsigned parameter)
{
    return &hw->intervals[parameter - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL];
}

static void
hw_range(struct snd_pcm_hw_params *hw, unsigned parameter,
    unsigned low, unsigned high)
{
    struct snd_interval *range = hw_interval(hw, parameter);

    memset(range, 0, sizeof(*range));
    range->min = low;
    range->max = high;
    range->integer = 1;
}

static void
hw_mask(struct snd_pcm_hw_params *hw, unsigned parameter, unsigned value)
{
    struct snd_mask *mask;

    mask = &hw->masks[parameter - SNDRV_PCM_HW_PARAM_FIRST_MASK];
    memset(mask, 0, sizeof(*mask));
    mask->bits[value / 32] = 1U << (value % 32);
}

static int
alsa_configure(int fd, unsigned rate)
{
    struct snd_pcm_hw_params hw;
    struct snd_pcm_sw_params sw;
    struct snd_interval *range;
    unsigned parameter, period, buffer;

    memset(&hw, 0, sizeof(hw));
    memset(hw.masks, 0xff, sizeof(hw.masks));
    for (parameter = SNDRV_PCM_HW_PARAM_FIRST_INTERVAL;
        parameter <= SNDRV_PCM_HW_PARAM_LAST_INTERVAL; ++parameter)
        hw_interval(&hw, parameter)->max = UINT_MAX;

    hw_mask(&hw, SNDRV_PCM_HW_PARAM_ACCESS, SNDRV_PCM_ACCESS_RW_INTERLEAVED);
    hw_mask(&hw, SNDRV_PCM_HW_PARAM_FORMAT, SNDRV_PCM_FORMAT_S16_LE);
    hw_mask(&hw, SNDRV_PCM_HW_PARAM_SUBFORMAT, SNDRV_PCM_SUBFORMAT_STD);
    hw_range(&hw, SNDRV_PCM_HW_PARAM_SAMPLE_BITS, 16, 16);
    hw_range(&hw, SNDRV_PCM_HW_PARAM_FRAME_BITS, 32, 32);
    hw_range(&hw, SNDRV_PCM_HW_PARAM_CHANNELS, 2, 2);
    hw_range(&hw, SNDRV_PCM_HW_PARAM_RATE, rate, rate);
    /* 5--40 ms periods and a 50--250 ms buffer leave room for scheduling. */
    hw_range(&hw, SNDRV_PCM_HW_PARAM_PERIOD_SIZE,
        rate / 200 ? rate / 200 : 1, rate / 25 ? rate / 25 : 1);
    hw_range(&hw, SNDRV_PCM_HW_PARAM_BUFFER_SIZE,
        rate / 20 ? rate / 20 : 1, rate / 4 ? rate / 4 : 1);
    hw_range(&hw, SNDRV_PCM_HW_PARAM_PERIODS, 2, UINT_MAX);
    hw.flags = SNDRV_PCM_HW_PARAMS_NORESAMPLE;
    hw.rmask = UINT_MAX;
    if (alsa_ioctl(fd, SNDRV_PCM_IOCTL_HW_REFINE, &hw) < 0 ||
        alsa_ioctl(fd, SNDRV_PCM_IOCTL_HW_PARAMS, &hw) < 0)
        return -1;

    range = hw_interval(&hw, SNDRV_PCM_HW_PARAM_RATE);
    if (range->min != rate || range->max != rate ||
        range->openmin || range->openmax || range->empty ||
        (hw.rate_den != 0 &&
        (uint64_t)hw.rate_num != (uint64_t)rate * hw.rate_den)) {
        errno = EINVAL;
        return -1;
    }
    period = hw_interval(&hw, SNDRV_PCM_HW_PARAM_PERIOD_SIZE)->min;
    buffer = hw_interval(&hw, SNDRV_PCM_HW_PARAM_BUFFER_SIZE)->min;
    if (period == 0 || buffer < period || buffer > rate) {
        errno = EINVAL;
        return -1;
    }

    memset(&sw, 0, sizeof(sw));
    sw.tstamp_mode = SNDRV_PCM_TSTAMP_NONE;
    sw.period_step = 1;
    sw.avail_min = period;
    sw.xfer_align = 1;
    sw.start_threshold = buffer;
    sw.stop_threshold = buffer;
    /* Silence insertion is disabled; an underrun must remain an error. */
    if (alsa_ioctl(fd, SNDRV_PCM_IOCTL_SW_PARAMS, &sw) < 0 ||
        alsa_ioctl(fd, SNDRV_PCM_IOCTL_PREPARE, NULL) < 0)
        return -1;
    return 0;
}

/* Return 1 only after an asynchronous drain reaches SETUP; -1 on failure. */
static int
alsa_position(int fd, size_t sent, size_t total, int draining,
    AudioProgress progress, void *context, size_t *last, size_t *played)
{
    struct snd_pcm_status status;
    size_t pending;

    memset(&status, 0, sizeof(status));
    if (alsa_ioctl(fd, SNDRV_PCM_IOCTL_STATUS, &status) < 0)
        return -1;
    switch (status.state) {
    case SNDRV_PCM_STATE_PREPARED:
        *played = 0;
        break;
    case SNDRV_PCM_STATE_RUNNING:
    case SNDRV_PCM_STATE_DRAINING:
        pending = status.delay > 0 ? (size_t)status.delay : 0;
        *played = sent > pending ? sent - pending : 0;
        break;
    case SNDRV_PCM_STATE_SETUP:
        if (draining)
            return 1;
        errno = EBADFD;
        return -1;
    case SNDRV_PCM_STATE_XRUN:
        errno = EPIPE;
        return -1;
    case SNDRV_PCM_STATE_SUSPENDED:
        errno = ESTRPIPE;
        return -1;
    case SNDRV_PCM_STATE_DISCONNECTED:
        errno = ENODEV;
        return -1;
    default:
        errno = EBADFD;
        return -1;
    }
    /* The final position belongs to drain completion, not buffer submission. */
    if (*played >= total)
        *played = total - 1;
    pcm_progress(progress, context, *played, last);
    return 0;
}

/* The default device is the first playback PCM listed by the kernel that is
 * not a digital display output, so laptops whose card 0 is HDMI still play
 * through their speakers. Without such a device, use the first playback PCM,
 * then card 0 device 0. */
static const char *
default_device(char *path, size_t size)
{
    char line[256];
    int found = 0, card, number;
    FILE *list = fopen("/proc/asound/pcm", "r");

    snprintf(path, size, "/dev/snd/pcmC0D0p");
    if (list == NULL)
        return path;
    while (fgets(line, sizeof(line), list) != NULL) {
        if (sscanf(line, "%d-%d:", &card, &number) != 2 ||
            strstr(line, ": playback ") == NULL)
            continue;
        int display = strstr(line, "HDMI") != NULL ||
            strstr(line, "DisplayPort") != NULL || strstr(line, " DP") != NULL;
        if (!found || !display)
            snprintf(path, size, "/dev/snd/pcmC%dD%dp", card, number);
        found = 1;
        if (!display)
            break;
    }
    fclose(list);
    return path;
}

int
pcm_play(const float *stereo, size_t frames, unsigned rate, const char *device,
    AudioProgress progress, void *context)
{
    char default_path[64];
    unsigned char samples[4096];
    struct snd_xferi transfer;
    const char *operation = "opening PCM device";
    size_t sent = 0, played = 0, previous = 0, last = SIZE_MAX, count;
    double active, now;
    int fd, result, saved_errno;

    if (device == NULL || *device == '\0' || strcmp(device, "default") == 0)
        device = default_device(default_path, sizeof(default_path));
    fd = open(device, O_WRONLY | O_NONBLOCK);
    if (fd < 0)
        goto fail_open;
    operation = "configuring stereo S16_LE PCM at the requested rate";
    if (alsa_configure(fd, rate) < 0) {
        saved_errno = errno;
        fprintf(stderr, "audio: %s must support %u Hz stereo S16_LE and "
            "a 50--250 ms buffer; select --rate or --device if needed\n",
            device, rate);
        errno = saved_errno;
        goto fail;
    }
    operation = "reading the monotonic clock";
    active = pcm_now();
    if (active < 0)
        goto fail;

    while (sent < frames) {
        operation = "reading PCM playback position";
        if (alsa_position(fd, sent, frames, 0, progress, context, &last,
            &played) < 0)
            goto fail;
        if (played > previous) {
            active = pcm_now();
            previous = played;
        }
        count = frames - sent;
        if (count > sizeof(samples) / 4)
            count = sizeof(samples) / 4;
        pcm_s16le(samples, stereo + sent * 2, count * 2);
        memset(&transfer, 0, sizeof(transfer));
        transfer.buf = samples;
        transfer.frames = (snd_pcm_uframes_t)count;
        operation = "writing PCM frames";
        result = alsa_ioctl(fd, SNDRV_PCM_IOCTL_WRITEI_FRAMES, &transfer);
        if (result < 0 && errno != EAGAIN)
            goto fail;
        if (result == 0 && transfer.result < 0) {
            errno = (int)-transfer.result;
            if (errno != EAGAIN)
                goto fail;
        } else if (result == 0 && transfer.result > 0) {
            if ((snd_pcm_uframes_t)transfer.result > transfer.frames) {
                errno = EIO;
                goto fail;
            }
            sent += (size_t)transfer.result;
            active = pcm_now();
            continue;
        }
        operation = "waiting for PCM playback";
        now = pcm_now();
        if (now < 0 || active < 0)
            goto fail;
        if (now - active > 3.0) {
            errno = ETIMEDOUT;
            goto fail;
        }
        pcm_pause();
    }

    /* DRAIN also starts a short clip which did not fill start_threshold. */
    operation = "starting PCM drain";
    result = alsa_ioctl(fd, SNDRV_PCM_IOCTL_DRAIN, NULL);
    if (result < 0 && errno != EAGAIN)
        goto fail;
    active = pcm_now();
    for (;;) {
        operation = "waiting for PCM drain";
        result = alsa_position(fd, sent, frames, 1, progress, context,
            &last, &played);
        if (result < 0)
            goto fail;
        if (result > 0)
            break;
        now = pcm_now();
        if (now < 0 || active < 0)
            goto fail;
        if (now - active > 3.0) {
            errno = ETIMEDOUT;
            goto fail;
        }
        pcm_pause();
    }
    pcm_progress(progress, context, frames, &last);
    if (close(fd) < 0) {
        fprintf(stderr, "audio: closing %s: %s\n", device, strerror(errno));
        return 1;
    }
    return 0;

fail:
    saved_errno = errno;
    (void)alsa_ioctl(fd, SNDRV_PCM_IOCTL_DROP, NULL);
    (void)close(fd);
    errno = saved_errno;
fail_open:
    saved_errno = errno;
    fprintf(stderr, "audio: %s (%s): %s\n", operation, device,
        strerror(saved_errno));
    if (saved_errno == EBUSY)
        fputs("audio: direct ALSA access is exclusive; a desktop sound "
            "server may already own this device\n", stderr);
    else if (saved_errno == EPIPE)
        fputs("audio: PCM underrun; playback stopped to avoid losing "
            "audio or desynchronizing lyrics\n", stderr);
    else if (saved_errno == ESTRPIPE)
        fputs("audio: PCM device was suspended; restart playback after "
            "the device resumes\n", stderr);
    return 1;
}
#endif
