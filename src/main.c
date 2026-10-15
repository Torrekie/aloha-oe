#if defined(__linux__)
#define _DEFAULT_SOURCE  /* isatty, fileno and TIOCGWINSZ under -std=c11 */
#endif
#include "audio.h"
#include "score.h"
#include "synth.h"
#include "phoneme.h"
#include "options.h"

#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <direct.h>
#ifndef ENABLE_VIRTUAL_TERMINAL_PROCESSING
#define ENABLE_VIRTUAL_TERMINAL_PROCESSING 0x0004
#endif
#else
#include <sys/ioctl.h>
#include <unistd.h>
#endif

#define PI 3.14159265358979323846

static const char *const part_names[PART_COUNT] = {
    "guitar", "bass", "steel", "piano", "percussion", "vocal"
};

/* Lyric display. Each syllable's letters appear one by one from its onset.
 * On a colour terminal a phrase is first drawn unsung (grey), shortly before
 * it begins, and its letters are highlighted as they are sung. Without colour
 * (pipes, files, NO_COLOR, TERM=dumb, or a phrase wider than the terminal)
 * letters are simply appended, and unsung text is never shown. Long passages
 * without singing are marked as instrumental; the marker's letters advance
 * evenly through the passage, finishing as the next phrase is previewed. */
enum { LYRIC_LINE_BYTES = 256, LYRIC_LINE_CUES = 32 };
#define LYRIC_LETTER_SECONDS 0.08
#define LYRIC_PREVIEW_SECONDS 2.0
#define LYRIC_GAP_SECONDS 4.0
#define LYRIC_SUNG "\x1b[1;33m"
#define LYRIC_UNSUNG "\x1b[90m"
#define LYRIC_MARK "\x1b[36m"
#define LYRIC_RESET "\x1b[0m"
static const char instrumental_mark[] = "\xe2\x99\xaa instrumental \xe2\x99\xaa";

struct LyricStream {
    const struct Score *score;
    size_t next, first_frame, frames;
    unsigned rate;
    int color, failed;
    /* The phrase being shown: cues [next, last], its text and cue offsets,
     * or (marking) the instrumental marker timed from mark_from to mark_until. */
    int active, marking, drawn, written, gap_marked, trailing_marked, seeked;
    double mark_from, mark_until;
    size_t last, length, shown, printed, cues;
    size_t offset[LYRIC_LINE_CUES];
    char text[LYRIC_LINE_BYTES];
    double previous_end;
#ifdef _WIN32
    HANDLE console;
    DWORD console_mode;
    int restore_console;
#endif
};

static size_t
utf8_letters(const char *text, size_t length)
{
    size_t letters = 0;
    for (size_t i = 0; i < length; ++i)
        letters += ((unsigned char)text[i] & 0xC0) != 0x80;
    return letters;
}

/* Index of the last cue of the phrase beginning at cue `first`. */
static size_t
lyric_phrase_end(const struct Score *score, size_t first)
{
    size_t last = first;
    while (last + 1 < score->lyric_count && !score->lyrics[last].line_end)
        ++last;
    return last;
}

static int
lyric_build(struct LyricStream *stream)
{
    const struct LyricCue *lyrics = stream->score->lyrics;
    stream->length = 0;
    stream->cues = 0;
    for (size_t i = stream->next; i <= stream->last; ++i) {
        size_t space = i > stream->next && lyrics[i].word_start;
        if (stream->cues == LYRIC_LINE_CUES ||
            stream->length + space + lyrics[i].length >= LYRIC_LINE_BYTES)
            return -1;
        if (space)
            stream->text[stream->length++] = ' ';
        stream->offset[stream->cues++] = stream->length;
        memcpy(stream->text + stream->length, lyrics[i].text, lyrics[i].length);
        stream->length += lyrics[i].length;
    }
    return 0;
}

/* Bytes of the phrase revealed at `now`, counting only letters timed before
 * the end of the excerpt. Letters of a syllable are spread over at most half
 * its sung length, and at most LYRIC_LETTER_SECONDS apart. */
static size_t
lyric_revealed(const struct LyricStream *stream, double now, double limit)
{
    const struct LyricCue *lyrics = stream->score->lyrics;
    size_t revealed = 0;
    if (stream->marking) {
        size_t letters = utf8_letters(stream->text, stream->length), k = 0;
        for (size_t i = 0; i < stream->length; ++i) {
            if (((unsigned char)stream->text[i] & 0xC0) == 0x80)
                continue;
            double at = stream->mark_from + (stream->mark_until - stream->mark_from)
                * (double)k++ / (double)letters;
            if (at > now || at >= limit)
                return revealed;
            revealed = i + 1;
            while (revealed < stream->length &&
                ((unsigned char)stream->text[revealed] & 0xC0) == 0x80)
                ++revealed;
        }
        return revealed;
    }
    for (size_t c = 0; c < stream->cues; ++c) {
        const struct LyricCue *cue = &lyrics[stream->next + c];
        size_t letters = utf8_letters(cue->text, cue->length), k = 0;
        double span = fmin(0.5 * (cue->end - cue->start),
            LYRIC_LETTER_SECONDS * (double)letters);
        for (size_t i = 0; i < cue->length; ++i) {
            if (((unsigned char)cue->text[i] & 0xC0) == 0x80)
                continue;
            double at = cue->start + span * (double)k++ / (double)letters;
            if (at > now || at >= limit)
                return revealed;
            /* Include this letter's continuation bytes. */
            size_t next = i + 1;
            while (next < cue->length &&
                ((unsigned char)cue->text[next] & 0xC0) == 0x80)
                ++next;
            revealed = stream->offset[c] + next;
        }
    }
    return revealed;
}

static void
lyric_draw(struct LyricStream *stream, size_t revealed)
{
    fputs(stream->marking ? "\r" LYRIC_MARK : "\r" LYRIC_SUNG, stdout);
    fwrite(stream->text, 1, revealed, stdout);
    fputs(LYRIC_UNSUNG, stdout);
    fwrite(stream->text + revealed, 1, stream->length - revealed, stdout);
    fputs(LYRIC_RESET "\x1b[K", stdout);
    stream->drawn = 1;
    stream->shown = revealed;
}

/* Mark a gap of at least LYRIC_GAP_SECONDS from `from` to `to` once. Returns
 * 0 if there is nothing (more) to mark, 1 while the marker waits for `now`,
 * and 2 when the marker has become the active line. Its letters are timed
 * from the start of the gap, so a seek into it shows the elapsed part. */
static int
lyric_gap(struct LyricStream *stream, int *marked, double from, double to,
    double now, double begin, double limit)
{
    double at = fmax(from, begin);
    if (*marked || to - from < LYRIC_GAP_SECONDS || at >= to || at >= limit)
        return 0;
    if (now < at)
        return 1;
    stream->length = sizeof(instrumental_mark) - 1;
    memcpy(stream->text, instrumental_mark, stream->length);
    stream->cues = 0;
    stream->mark_from = from;
    stream->mark_until = to - LYRIC_PREVIEW_SECONDS;
    stream->active = stream->marking = *marked = 1;
    stream->drawn = stream->written = 0;
    stream->shown = stream->printed = 0;
    return 2;
}

/* Called on the playback control thread from the device's sample position.
 * Output is flushed after every change, so pipes and terminals alike see
 * each letter as it is sung. A seek into a phrase shows (or, without colour,
 * prints from the start of the current word) what has already been sung;
 * completed phrases are skipped. */
static void
lyric_progress(void *context, size_t played_frames)
{
    struct LyricStream *stream = context;
    const struct LyricCue *lyrics = stream->score->lyrics;
    if (stream->failed)
        return;
    double begin = (double)stream->first_frame / stream->rate;
    double now = (double)(stream->first_frame + played_frames) / stream->rate;
    double limit = (double)(stream->first_frame + stream->frames) / stream->rate;
    int changed = 0;
    for (;;) {
        if (!stream->active) {
            if (stream->next >= stream->score->lyric_count) {
                if (lyric_gap(stream, &stream->trailing_marked, stream->previous_end,
                    stream->score->seconds, now, begin, limit) == 2)
                    continue;
                break;
            }
            size_t last = lyric_phrase_end(stream->score, stream->next);
            double first_start = lyrics[stream->next].start;
            if (lyrics[last].end <= begin) {
                stream->previous_end = lyrics[last].end;
                stream->next = last + 1;
                continue;
            }
            int gap = lyric_gap(stream, &stream->gap_marked,
                stream->previous_end, first_start, now, begin, limit);
            if (gap == 2)
                continue;
            if (gap == 1 || first_start >= limit)
                break;
            double appear = stream->color ?
                fmax(first_start - LYRIC_PREVIEW_SECONDS, stream->previous_end) :
                first_start;
            if (now < appear)
                break;
            stream->last = last;
            if (lyric_build(stream) != 0) {
                fputs("lyrics: phrase too long to display\n", stderr);
                stream->failed = 1;
                return;
            }
            stream->active = 1;
            stream->drawn = stream->written = 0;
            stream->shown = stream->printed = 0;
            if (!stream->seeked) {
                /* Without colour, a seek prints from the current word on. */
                size_t c = stream->next;
                while (c < last && lyrics[c].end <= begin)
                    ++c;
                while (c > stream->next && !lyrics[c].word_start)
                    --c;
                stream->printed = stream->offset[c - stream->next];
                stream->seeked = 1;
            }
        }
        size_t revealed = lyric_revealed(stream, now, limit);
        if (stream->color) {
            if (!stream->drawn || revealed != stream->shown) {
                lyric_draw(stream, revealed);
                changed = 1;
            }
        } else if (revealed > stream->printed) {
            fwrite(stream->text + stream->printed, 1,
                revealed - stream->printed, stdout);
            stream->printed = revealed;
            stream->written = changed = 1;
        }
        if (revealed < stream->length)
            break;
        fputc('\n', stdout);
        changed = 1;
        stream->active = 0;
        if (stream->marking) {
            stream->marking = 0;  /* The phrase that follows is still due. */
            continue;
        }
        stream->previous_end = lyrics[stream->last].end;
        stream->next = stream->last + 1;
        stream->gap_marked = 0;
    }
    if (changed && (fflush(stdout) == EOF || ferror(stdout)))
        stream->failed = 1;
}

/* Choose colour output for an interactive terminal wide enough for every
 * phrase. NO_COLOR (https://no-color.org) and TERM=dumb disable it. */
static void
lyric_start(struct LyricStream *stream)
{
    const char *no_color = getenv("NO_COLOR"), *term = getenv("TERM");
    size_t widest = 0, columns = 0;
    if (no_color && no_color[0])
        return;
    for (size_t i = 0; i < stream->score->lyric_count;) {
        size_t last = lyric_phrase_end(stream->score, i), width = 0;
        for (size_t c = i; c <= last; ++c)
            width += utf8_letters(stream->score->lyrics[c].text,
                stream->score->lyrics[c].length)
                + (c > i && stream->score->lyrics[c].word_start);
        if (width > widest)
            widest = width;
        i = last + 1;
    }
#ifdef _WIN32
    CONSOLE_SCREEN_BUFFER_INFO info;
    stream->console = GetStdHandle(STD_OUTPUT_HANDLE);
    if (!GetConsoleMode(stream->console, &stream->console_mode) ||
        !GetConsoleScreenBufferInfo(stream->console, &info))
        return;
    columns = (size_t)(info.srWindow.Right - info.srWindow.Left + 1);
    if (widest >= columns)
        return;
    if (!(stream->console_mode & ENABLE_VIRTUAL_TERMINAL_PROCESSING)) {
        if (!SetConsoleMode(stream->console,
            stream->console_mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING))
            return;
        stream->restore_console = 1;
    }
    (void)term;
#else
    struct winsize size;
    if (!isatty(STDOUT_FILENO) || (term && !strcmp(term, "dumb")))
        return;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) == 0)
        columns = size.ws_col;
    if (columns == 0)
        columns = 80;
    if (widest >= columns)
        return;
#endif
    stream->color = 1;
}

static int
lyric_finish(struct LyricStream *stream)
{
    if (stream->active && !stream->failed) {
        if (stream->color && stream->shown == 0)
            fputs("\r\x1b[K", stdout);  /* Erase a phrase never reached. */
        else if (stream->color || stream->written)
            fputc('\n', stdout);
        stream->active = 0;
        if (fflush(stdout) == EOF || ferror(stdout))
            stream->failed = 1;
    }
#ifdef _WIN32
    if (stream->restore_console)
        SetConsoleMode(stream->console, stream->console_mode);
#endif
    if (stream->failed) {
        fputs("lyrics: writing to standard output failed\n", stderr);
        return 1;
    }
    return 0;
}

static int
note_part(enum Instrument instrument)
{
    if (instrument >= INST_GUITAR && instrument <= INST_SHAKER)
        return (int)instrument;
    if (instrument >= INST_VOICE_A && instrument <= INST_VOICE_U)
        return PART_VOCAL;
    return -1;
}

static double
audio_peak(const struct Audio *audio)
{
    double peak = 0.0;
    for (size_t i = 0; i < audio->frames * 2; ++i)
        peak = fmax(peak, fabs(audio->samples[i]));
    return peak;
}

static void
audio_scale(struct Audio *audio, double gain)
{
    for (size_t i = 0; i < audio->frames * 2; ++i)
        audio->samples[i] = (float)(audio->samples[i] * gain);
}

/* Render from the beginning so an offset keeps instrument envelopes and room
 * history intact. Apply the same linear fades to both mix and stem windows. */
static int
render_window(const struct Note *notes, size_t count, struct Audio *audio,
    double end, unsigned rate, int wet, size_t first, int preview)
{
    if (synth_render_raw(notes, count, audio, end, rate, wet) != 0)
        return -1;
    if (first >= audio->frames) {
        synth_free(audio);
        errno = EINVAL;
        return -1;
    }
    if (first != 0) {
        audio->frames -= first;
        memmove(audio->samples, audio->samples + first * 2,
            audio->frames * 2 * sizeof(float));
        size_t fade = (size_t)(0.020 * rate);
        if (fade > audio->frames)
            fade = audio->frames;
        for (size_t i = 0; i < fade; ++i) {
            double gain = (double)i / fade;
            audio->samples[i * 2] *= (float)gain;
            audio->samples[i * 2 + 1] *= (float)gain;
        }
    }
    if (preview) {
        size_t fade = (size_t)(0.080 * rate);
        if (fade > audio->frames)
            fade = audio->frames;
        for (size_t i = 0; i < fade; ++i) {
            double gain = (double)i / fade;
            size_t frame = audio->frames - i - 1;
            audio->samples[frame * 2] *= (float)gain;
            audio->samples[frame * 2 + 1] *= (float)gain;
        }
    }
    return 0;
}

static int
stem_directory(const char *path)
{
    struct stat st;
    int status;

#ifdef _WIN32
    status = _mkdir(path);
#else
    status = mkdir(path, 0777);
#endif
    if (status != 0 && errno != EEXIST) {
        perror(path);
        return -1;
    }
    if (stat(path, &st) != 0) {
        perror(path);
        return -1;
    }
#ifdef _WIN32
    if ((st.st_mode & _S_IFMT) != _S_IFDIR) {
#else
    if (!S_ISDIR(st.st_mode)) {
#endif
        fprintf(stderr, "Stem destination is not a directory: %s\n", path);
        return -1;
    }
    return 0;
}

static int
stem_notes(const struct Score *score, struct Note *notes, int part)
{
    int audible = 0;
    for (size_t i = 0; i < score->count; ++i) {
        notes[i] = score->notes[i];
        if (note_part(notes[i].instrument) != part)
            notes[i].velocity = 0.0;
        else if (notes[i].velocity != 0.0)
            audible = 1;
    }
    return audible;
}

static char *
stem_path(const char *directory, int part)
{
    size_t dir_length = strlen(directory), name_length = strlen(part_names[part]);
    if (dir_length > SIZE_MAX - name_length - 6) {
        errno = EOVERFLOW;
        return NULL;
    }
    size_t length = dir_length + name_length + 6;
    char *path = malloc(length);
    if (path != NULL)
        snprintf(path, length, "%s/%s.wav", directory, part_names[part]);
    return path;
}

static int
put16(FILE *file, uint16_t n)
{
    unsigned char bytes[2] = { n & 255, (n >> 8) & 255 };
    return fwrite(bytes, 1, 2, file) == 2 ? 0 : -1;
}

static int
put32(FILE *file, uint32_t n)
{
    unsigned char bytes[4] = { n & 255, (n >> 8) & 255, (n >> 16) & 255, (n >> 24) & 255 };
    return fwrite(bytes, 1, 4, file) == 4 ? 0 : -1;
}

static int
write_wav(const struct Audio *audio, const char *path)
{
    if (audio->frames > (UINT32_MAX - 36) / 4) {
        fprintf(stderr, "WAV exceeds the RIFF size limit\n");
        return -1;
    }
    FILE *file = fopen(path, "wb");
    if (!file) {
        perror(path);
        return -1;
    }
    uint32_t bytes = (uint32_t)audio->frames * 4;
    int failed = fwrite("RIFF", 1, 4, file) != 4 || put32(file, 36 + bytes) ||
        fwrite("WAVEfmt ", 1, 8, file) != 8 || put32(file, 16) ||
        put16(file, 1) || put16(file, 2) || put32(file, audio->rate) ||
        put32(file, audio->rate * 4) || put16(file, 4) || put16(file, 16) ||
        fwrite("data", 1, 4, file) != 4 || put32(file, bytes);
    for (size_t i = 0; !failed && i < audio->frames * 2; ++i) {
        double sample = fmax(-1.0, fmin(1.0, audio->samples[i]));
        int16_t pcm = (int16_t)lrint(sample * 32767.0);
        failed = put16(file, (uint16_t)pcm) != 0;
    }
    if (fclose(file) != 0)
        failed = 1;
    if (failed) {
        fprintf(stderr, "Cannot finish WAV: %s\n", path);
        return -1;
    }
    fprintf(stderr, "Saved %s (%.3f s, %u Hz stereo)\n", path,
        (double)audio->frames / audio->rate, audio->rate);
    return 0;
}

/* Test a rendered fundamental, rather than just the MIDI-to-Hz formula.
 * A Hann-windowed local frequency search avoids other harmonics and estimates
 * pitch to a fraction of a cent for these fixed-frequency dry instruments. */
static double
pitch_cents(const struct Audio *audio, double target)
{
    size_t begin = (size_t)(audio->rate * 0.15);
    size_t count = (size_t)(audio->rate * 1.5);
    if (begin + count > audio->frames)
        return NAN;
    double best = -1.0, best_cents = 0.0;
    for (int q = -80; q <= 80; ++q) {
        double cents = q * 0.25;
        double frequency = target * exp2(cents / 1200.0);
        double step = 2 * PI * frequency / audio->rate;
        double cs = cos(step), sn = sin(step), real = 1, imag = 0;
        double re = 0, im = 0;
        for (size_t i = 0; i < count; ++i) {
            double window = 0.5 - 0.5 * cos(2 * PI * i / (count - 1));
            double sample = (audio->samples[(begin+i)*2] +
                audio->samples[(begin+i)*2+1]) * window;
            re += sample * real;
            im += sample * imag;
            double next = real * cs - imag * sn;
            imag = real * sn + imag * cs;
            real = next;
        }
        double power = re * re + im * im;
        if (power > best) { best = power; best_cents = cents; }
    }
    return best_cents;
}

static int
check_all(void)
{
    if (score_check() != 0)
        return 1;
    const enum Instrument instruments[] = { INST_GUITAR, INST_BASS, INST_PIANO };
    const int pitches[] = { 69, 40, 64 };
    for (size_t k = 0; k < sizeof(pitches)/sizeof(pitches[0]); ++k) {
        struct Audio probe = {0};
        struct Note note = { .start = 0, .duration = 2.0, .midi = pitches[k],
            .velocity = 0.8, .pan = 0, .instrument = instruments[k] };
        if (synth_note_render(&note, &probe, 2.5, 48000, 0.65) != 0) {
            perror("tuning probe");
            return 1;
        }
        double cents = pitch_cents(&probe, synth_frequency(pitches[k]));
        synth_free(&probe);
        printf("%s MIDI %d: rendered fundamental %.2f cents from target\n",
            synth_instrument_name(instruments[k]), pitches[k], cents);
        if (!isfinite(cents) || fabs(cents) > 2.0)
            return 1;
    }
    struct Score score = {0};
    struct Audio audio = {0};
    if (score_build(&score, SECTION_FULL, 49, 0, 0, MODE_ENSEMBLE) != 0) {
        perror("full-song score"); return 1;
    }
    size_t singers = 0, strings = 0;
    for (size_t i = 0; i < score.count; ++i) {
        int part = note_part(score.notes[i].instrument);
        if (part == PART_VOCAL) ++singers;
        if (part == PART_STEEL) ++strings;
        if (part == PART_PIANO || part == PART_PERCUSSION)
            score.notes[i].velocity = 0;
    }
    if (!singers || !strings ||
        synth_render(score.notes, score.count, &audio, score.seconds, 48000, 0.65, 1) != 0) {
        perror("full-song check");
        score_free(&score);
        return 1;
    }
    double peak = 0, energy = 0;
    for (size_t i = 0; i < audio.frames * 2; ++i) {
        double sample = audio.samples[i];
        if (!isfinite(sample) || fabs(sample) > 0.650001) {
            fprintf(stderr, "Signal bounds failed at sample %zu\n", i);
            synth_free(&audio); score_free(&score); return 1;
        }
        if (fabs(sample) > peak) peak = fabs(sample);
        energy += sample * sample;
    }
    double rms = sqrt(energy / (audio.frames * 2));
    if (rms < 0.001 || fabs((double)audio.frames / audio.rate - score.seconds) > 1.0/audio.rate) {
        fputs("Signal or duration check failed\n", stderr);
        synth_free(&audio); score_free(&score); return 1;
    }
    printf("Full ensemble: %zu events (%zu vocal, %zu steel), %.3f s, peak %.4f, RMS %.4f; finite and unclipped\n",
        score.count, singers, strings, (double)audio.frames / audio.rate, peak, rms);
    synth_free(&audio); score_free(&score);
    puts("All checks passed.");
    return 0;
}

static int
program_main(int argc, char **argv)
{
    struct Options opts;
    enum OptionsResult parsed;

    parsed = options_parse(argc, argv, &opts);
    if (parsed != OPTIONS_OK)
        return parsed == OPTIONS_HELP ? 0 : 2;
    if (opts.check)
        return check_all();
    struct Score score = {0};
    struct Audio audio = {0};
    struct Note *filtered = NULL;
    int result = 1;
    if (opts.ipa) {
        double duration = opts.seconds > 0 ? opts.seconds : 6.0;
        struct PhoneSequence check_ipa;
        double spoken = fmax(0.03, duration-0.16);
        if (phoneme_plan(opts.ipa, spoken, &check_ipa) != 0) {
            fputs("Invalid or unsupported UTF-8 IPA sequence\n", stderr); return 2;
        }
        score.notes = calloc(1, sizeof(*score.notes));
        if (!score.notes) { perror("IPA note"); return 1; }
        opts.mode = MODE_VOCAL;
        score.count = 1;
        score.seconds = duration;
        score.bpm = opts.bpm;
        score.notes[0] = (struct Note){.duration=spoken, .midi=52+opts.transpose,
            .velocity=.40, .instrument=INST_VOICE_A, .ipa=opts.ipa, .voice_style=opts.voice_style};
    } else if (score_build(&score, opts.section, opts.bpm, opts.transpose, opts.lead_only, opts.mode) != 0) {
        perror("building score"); return 1;
    }
    if (opts.offset >= score.seconds) {
        fprintf(stderr, "Offset must be less than the selected section's %.3f-second duration\n",
            score.seconds);
        result = 2;
        goto cleanup;
    }
    double available = score.seconds - opts.offset;
    double length = opts.seconds > 0 && opts.seconds < available ? opts.seconds : available;
    double end = opts.offset + length;
    size_t first = (size_t)llround(opts.offset * opts.rate);
    int preview = length < 3.0 || (opts.seconds > 0 && opts.seconds <= available);
    if (first >= (size_t)ceil(end * opts.rate)) {
        fputs("Offset leaves no audio frames to render\n", stderr);
        result = 2;
        goto cleanup;
    }
    for (size_t i = 0; i < score.count; ++i) {
        int part = note_part(score.notes[i].instrument);
        if (part >= 0)
            score.notes[i].velocity *= opts.part_gain[part];
        score.notes[i].voice_style = opts.voice_style;
    }
    if (opts.csv && score_csv(&score, opts.csv) != 0)
        goto cleanup;
    if (opts.stems && stem_directory(opts.stems) != 0)
        goto cleanup;
    const char *mode_name = opts.mode == MODE_INSTRUMENTAL ? "instrumental" :
        opts.mode == MODE_VOCAL ? "vocal" : "ensemble";
    fprintf(stderr, "Aloha 'Oe: E major %+d semitones; quarter %.2f / eighth %.2f BPM\n"
        "Rendering %s, %zu events, %.3f seconds from %.3f s; all sounds are synthesized.\n",
        opts.transpose, opts.bpm, opts.bpm*2, mode_name, score.count, length, opts.offset);
    if (opts.mode != MODE_INSTRUMENTAL)
        fprintf(stderr, "Synthetic %s IPA phonemes accompany the arrangement.\n",
            opts.voice_style == VOICE_RETRO ? "retro" : "baritone");
    if (render_window(score.notes, score.count, &audio, end, opts.rate, opts.wet, first, preview) != 0) {
        perror("synthesis");
        goto cleanup;
    }
    double master_peak = audio_peak(&audio), stem_peaks[PART_COUNT] = {0};
    if (opts.stems) {
        if (score.count > SIZE_MAX / sizeof(*filtered)) {
            errno = EOVERFLOW;
            perror("stem events");
            goto cleanup;
        }
        filtered = malloc((score.count ? score.count : 1) * sizeof(*filtered));
        if (filtered == NULL) {
            perror("stem events");
            goto cleanup;
        }
        /* Keep only one raw stem in memory at a time. Peak prepass prevents
         * cancellation in the full mix from hiding an over-range stem. */
        for (int part = 0; part < PART_COUNT; ++part) {
            struct Audio stem = {0};
            int audible = stem_notes(&score, filtered, part);
            if (render_window(filtered, score.count, &stem, end, opts.rate,
                opts.wet && audible, first, preview) != 0) {
                perror("stem synthesis");
                goto cleanup;
            }
            stem_peaks[part] = audio_peak(&stem);
            master_peak = fmax(master_peak, stem_peaks[part]);
            synth_free(&stem);
        }
    }
    double master_gain = master_peak > 0.0 ? opts.volume / master_peak : 0.0;
    audio_scale(&audio, master_gain);
    if (opts.stems) {
        fprintf(stderr, "Shared master gain %.6f (largest raw mix/track peak %.6f)\n",
            master_gain, master_peak);
        for (int part = 0; part < PART_COUNT; ++part) {
            struct Audio stem = {0};
            int audible = stem_notes(&score, filtered, part);
            char *path = stem_path(opts.stems, part);
            if (path == NULL) {
                perror("stem path");
                goto cleanup;
            }
            if (render_window(filtered, score.count, &stem, end, opts.rate,
                opts.wet && audible, first, preview) != 0) {
                perror("stem synthesis");
                free(path);
                goto cleanup;
            }
            audio_scale(&stem, master_gain);
            int failed = write_wav(&stem, path);
            free(path);
            synth_free(&stem);
            if (failed)
                goto cleanup;
            fprintf(stderr, "  %s: raw peak %.6f, part gain %.3f\n",
                part_names[part], stem_peaks[part], opts.part_gain[part]);
        }
    }
    result = 0;
    if (opts.wav && write_wav(&audio, opts.wav) != 0)
        result = 1;
    if (!result && opts.play) {
        fputs("Playing through the system audio output...\n", stderr);
        int show_lyrics = opts.lyrics >= 0 ? opts.lyrics :
            (opts.mode != MODE_INSTRUMENTAL && opts.part_gain[PART_VOCAL] > 0 && opts.volume > 0);
        struct LyricStream stream = { .score=&score, .first_frame=first,
            .frames=audio.frames, .rate=audio.rate };
        AudioProgress progress = show_lyrics && score.lyric_count ? lyric_progress : NULL;
        if (progress != NULL)
            lyric_start(&stream);
        result = play_audio_with_progress(audio.samples, audio.frames, audio.rate,
            opts.device, progress, &stream) != 0;
        if (progress != NULL && lyric_finish(&stream))
            result = 1;
        if (!result) fputs("Playback completed.\n", stderr);
    }
cleanup:
    free(filtered);
    score_free(&score);
    synth_free(&audio);
    return result;
}

#ifdef _WIN32
/* The CRT has already split and unquoted the wide command line. Keep UTF-8
 * inside the program, including the IPA parser and redirected lyric output. */
int
wmain(int argc, wchar_t **wide_argv)
{
    char **argv;
    DWORD console_mode;
    UINT saved_codepage = 0;
    int i, result = 1;

    if (argc < 0 || (argc != 0 && wide_argv == NULL) ||
        (size_t)argc > SIZE_MAX / sizeof(*argv) - 1) {
        fputs("aloha-oe: invalid argument vector\n", stderr);
        return 1;
    }
    argv = calloc((size_t)argc + 1, sizeof(*argv));
    if (argv == NULL) {
        perror("aloha-oe: argument vector");
        return 1;
    }
    for (i = 0; i < argc; ++i) {
        int bytes;

        bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
            wide_argv[i], -1, NULL, 0, NULL, NULL);
        if (bytes == 0) {
            fprintf(stderr, "aloha-oe: argument %d is not valid Unicode "
                "(Windows error %lu)\n", i, (unsigned long)GetLastError());
            goto out;
        }
        argv[i] = malloc((size_t)bytes);
        if (argv[i] == NULL) {
            perror("aloha-oe: argument conversion");
            goto out;
        }
        if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
            wide_argv[i], -1, argv[i], bytes, NULL, NULL) != bytes) {
            fprintf(stderr, "aloha-oe: argument conversion failed "
                "(Windows error %lu)\n", (unsigned long)GetLastError());
            goto out;
        }
    }
    /* File/pipe output stays UTF-8. Only an attached console needs its
     * code page changed, and the caller's setting is restored on return. */
    if (GetConsoleMode(GetStdHandle(STD_OUTPUT_HANDLE), &console_mode)) {
        UINT codepage = GetConsoleOutputCP();

        if (codepage == 0 || !SetConsoleOutputCP(CP_UTF8)) {
            fprintf(stderr, "aloha-oe: cannot select UTF-8 console output "
                "(Windows error %lu)\n", (unsigned long)GetLastError());
            goto out;
        }
        saved_codepage = codepage;
    }
    result = program_main(argc, argv);
    if (fflush(stdout) == EOF) {
        perror("aloha-oe: standard output");
        result = 1;
    }

out:
    if (saved_codepage != 0 && !SetConsoleOutputCP(saved_codepage)) {
        fprintf(stderr, "aloha-oe: cannot restore console output code page "
            "(Windows error %lu)\n", (unsigned long)GetLastError());
        result = 1;
    }
    for (i = 0; i < argc; ++i)
        free(argv[i]);
    free(argv);
    return result;
}
#else
int
main(int argc, char **argv)
{
    return program_main(argc, argv);
}
#endif
