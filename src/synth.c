/* Algorithmic instrument sketches: no samples, soundfonts, or runtime DSP libs.
 *
 * Harmonic oscillators are 2D rotations (Julius O. Smith, PASP):
 * https://ccrma.stanford.edu/~jos/pasp/Digital_Sinusoid_Generators.html
 * Plucked strings can be decomposed into damped harmonic modes; this deliberately
 * simplified model omits measured body responses and coupled polarizations:
 * https://www.its.caltech.edu/~politzer/plucked-string-AJP.pdf
 * Karplus-Strong was considered, but its loop/filter phase delay requires tuning
 * compensation. Directly generated harmonics keep this toy's score tuning clear:
 * https://musicweb.ucsd.edu/~trsmyth/papers/KSExtensions.pdf
 *
 * Timbre coefficients and room parameters below are our own approximations.
 * The steel lead suggests a sustained Hawaiian string/reed; it is not a vocal
 * model and does not impersonate a singer's recorded voice.
 * Articulated vowels use our raised-cosine glottal-flow pulse, its band-limited
 * derivative, moving formant resonators, aspiration and consonant sketches.
 * Low formants and mild source roughness suggest a low country singing voice.
 * They do not reproduce any singer. Bass formant centers/bandwidths are from
 * the official Csound reference tables, without using Csound code or libraries:
 * https://csound.com/docs/manual/MiscFormants.html
 * Source-filter background (Jurafsky/Martin, Acoustic Phonetics and Signals):
 * https://web.stanford.edu/~jurafsky/slp3/old_aug24/H.pdf
 * Glottal source, aspiration and formant synthesis background (KTH):
 * https://www.speech.kth.se/~rolf/papers/wwnasqpsr.pdf
 * https://www.speech.kth.se/qpsr/1991/1991_32_2-3_001-010.pdf
 */
#include "synth.h"
#include "phoneme.h"

#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define PI 3.14159265358979323846264338327950288
#define LEGACY_PARTIALS 16
#define PARTIALS 64
#define SOURCE_QUADRATURE 1024

static double clamp(double x, double lo, double hi)
{
    return x < lo ? lo : (x > hi ? hi : x);
}

static double smooth(double x)
{
    x = clamp(x, 0.0, 1.0);
    return x * x * (3.0 - 2.0 * x);
}

double synth_frequency(int midi)
{
    return 440.0 * exp2(((double)midi - 69.0) / 12.0);
}

const char *synth_instrument_name(enum Instrument instrument)
{
    static const char *const names[] = {
        "guitar", "bass", "steel", "piano", "shaker",
        "voice-a", "voice-e", "voice-i", "voice-o", "voice-u"
    };
    return instrument >= INST_GUITAR && instrument <= INST_VOICE_U
        ? names[(int)instrument] : "unknown";
}

static int is_voice(enum Instrument instrument)
{
    return instrument >= INST_VOICE_A && instrument <= INST_VOICE_U;
}

static const double vowel_centers[5][3] = {
        {600.0, 1040.0, 2250.0},
        {400.0, 1620.0, 2400.0},
        {250.0, 1750.0, 2600.0},
        {400.0,  750.0, 2400.0},
        {350.0,  600.0, 2400.0}
};
static const double vowel_widths[5][3] = {
        {60.0, 70.0, 110.0},
        {40.0, 80.0, 100.0},
        {60.0, 90.0, 100.0},
        {40.0, 80.0, 100.0},
        {40.0, 80.0, 100.0}
};

struct VoiceSource {
    double cosine[PARTIALS], sine[PARTIALS];
};

/* Integrate an analytic flow pulse to derive Fourier coefficients at runtime.
 * No measured waveform/table is stored. Differentiating the finite harmonic
 * series models radiation/excitation while enforcing the anti-alias cutoff.
 * An open phase of 0.66 cycle and brisk closure supply a warm, slightly rough
 * pulse. Parameters are our own, rather than a fit to an issued recording. */
static void prepare_voice(struct VoiceSource *source)
{
    unsigned h, j;
    double sum = 0.0;
    memset(source, 0, sizeof(*source));
    for (j = 0; j < SOURCE_QUADRATURE; ++j) {
        double t = ((double)j + 0.5) / SOURCE_QUADRATURE, flow;
        double c = cos(2.0 * PI * t), s = sin(2.0 * PI * t);
        double hc = c, hs = s;
        if (t < 0.50)
            flow = 0.5 * (1.0 - cos(PI * t / 0.50));
        else if (t < 0.66)
            flow = cos(0.5 * PI * (t - 0.50) / 0.16);
        else
            flow = 0.0;
        for (h = 0; h < PARTIALS; ++h) {
            double next_c = hc * c - hs * s;
            /* Derivative: d(a*cos+b*sin)/dphase = h*(b*cos-a*sin). */
            source->cosine[h] += flow * hs * (double)(h + 1);
            source->sine[h] -= flow * hc * (double)(h + 1);
            hs = hs * c + hc * s;
            hc = next_c;
        }
    }
    for (h = 0; h < PARTIALS; ++h) {
        source->cosine[h] *= 2.0 / SOURCE_QUADRATURE;
        source->sine[h] *= 2.0 / SOURCE_QUADRATURE;
        sum += hypot(source->cosine[h], source->sine[h]);
    }
    for (h = 0; h < PARTIALS; ++h) {
        source->cosine[h] /= sum;
        source->sine[h] /= sum;
    }
}

static double noise(uint32_t *state)
{
    uint32_t x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return (double)x / 2147483648.0 - 1.0;
}

/* Event identity is independent of its array position. Filtering the score
 * into stems therefore preserves pick/breath/noise excitation exactly. */
static uint32_t note_seed(const struct Note *note)
{
    uint64_t bits = 0;
    uint32_t seed;
    memcpy(&bits, &note->start, sizeof(note->start));
    bits ^= (uint64_t)(unsigned)note->midi * UINT64_C(0x9e3779b97f4a7c15);
    bits ^= (uint64_t)(unsigned)note->instrument * UINT64_C(0xbf58476d1ce4e5b9);
    bits ^= bits >> 30;
    bits *= UINT64_C(0xbf58476d1ce4e5b9);
    bits ^= bits >> 27;
    seed = (uint32_t)(bits ^ (bits >> 32));
    return seed ? seed : UINT32_C(0x9e3779b9);
}

static double release_time(enum Instrument instrument)
{
    if (is_voice(instrument))
        return 0.16;
    switch (instrument) {
    case INST_BASS:   return 0.09;
    case INST_STEEL:  return 0.18;
    case INST_PIANO:  return 0.20;
    case INST_SHAKER: return 0.02;
    default:         return 0.14;
    }
}

/* Smooth attack and finite release are separate from each partial's natural
 * exponential decay. The gate starts releasing exactly at score note-off. */
static double gate(double t, double duration, double attack, double release)
{
    double env = smooth(t / attack);
    if (t > duration)
        env *= 1.0 - smooth((t - duration) / release);
    return env;
}

struct Resonator {
    double a, b, c, da, db, dc;
    double x1, x2, y1, y2;
    unsigned ramp_left;
};

static void resonator_coefficients(double frequency, double width, double rate,
                                  double *a, double *b, double *c)
{
    double radius = exp(-PI * width / rate);
    double angle = 2.0 * PI * clamp(frequency, 40.0, rate * 0.42) / rate;
    *a = 0.5 * (1.0 - radius * radius);
    *b = 2.0 * radius * cos(angle);
    *c = -radius * radius;
}

static void resonator_init(struct Resonator *r, double f, double width, double rate)
{
    memset(r, 0, sizeof(*r));
    resonator_coefficients(f, width, rate, &r->a, &r->b, &r->c);
}

static void resonator_target(struct Resonator *r, double f, double width, double rate)
{
    double a, b, c;
    resonator_coefficients(f, width, rate, &a, &b, &c);
    r->da = (a - r->a) / 32.0;
    r->db = (b - r->b) / 32.0;
    r->dc = (c - r->c) / 32.0;
    r->ramp_left = 32;
}

static double resonator_sample(struct Resonator *r, double x)
{
    double y;
    if (r->ramp_left) {
        r->a += r->da;
        r->b += r->db;
        r->c += r->dc;
        --r->ramp_left;
    }
    y = r->a * (x - r->x2) + r->b * r->y1 + r->c * r->y2;
    r->x2 = r->x1;
    r->x1 = x;
    r->y2 = r->y1;
    r->y1 = y;
    return y;
}

static char lower_consonant(char c)
{
    return c >= 'A' && c <= 'Z' ? (char)(c + ('a' - 'A')) : c;
}

/* Diphthong and voiced-consonant coarticulation. The lowered tract is a design
 * choice for a dark male voice; it is not measured from a particular singer. */
static void voice_formants(const struct Note *note, double t,
                           double centers[3], double widths[3], double *nasal)
{
    unsigned from = (unsigned)(note->instrument - INST_VOICE_A), to = from, band;
    double blend = 0.0, onset = 1.0 - smooth(t / fmax(0.005, fmin(0.09, note->duration * 0.30)));
    double onset_centers[3] = {0.0, 0.0, 0.0};
    char consonant = lower_consonant(note->consonant);
    if (is_voice(note->vowel_end)) {
        to = (unsigned)(note->vowel_end - INST_VOICE_A);
        blend = smooth((t - note->duration * 0.52) / fmax(0.025, note->duration * 0.34));
    }
    *nasal = consonant == 'm' || consonant == 'n' ? onset : 0.0;
    switch (consonant) {
    case 'm': case 'n':
        onset_centers[0] = 270.0; onset_centers[1] = 1100.0; onset_centers[2] = 2250.0;
        break;
    case 'l':
        onset_centers[0] = 300.0; onset_centers[1] = 1350.0; onset_centers[2] = 2600.0;
        break;
    case 'r':
        onset_centers[0] = 320.0; onset_centers[1] = 1100.0; onset_centers[2] = 1650.0;
        break;
    case 'w':
        onset_centers[0] = 320.0; onset_centers[1] = 650.0; onset_centers[2] = 2300.0;
        break;
    case 'y':
        onset_centers[0] = 240.0; onset_centers[1] = 1800.0; onset_centers[2] = 2550.0;
        break;
    default:
        onset = 0.0;
        break;
    }
    for (band = 0; band < 3; ++band) {
        double center = vowel_centers[from][band] * (1.0 - blend)
                      + vowel_centers[to][band] * blend;
        centers[band] = (center * (1.0 - onset) + onset_centers[band] * onset) * 0.94;
        /* A little extra damping rounds the very sharp table resonances. */
        widths[band] = 1.30 * (vowel_widths[from][band] * (1.0 - blend)
                            + vowel_widths[to][band] * blend);
    }
}

static double bend_phase(double f, double t, double bend, double tau)
{
    return 2.0 * PI * f * (exp2(bend / 12.0) - 1.0) * tau * (1.0 - exp(-t / tau));
}

/* Integral of a finite smoothstep glide in frequency. Its derivative begins
 * at the source pitch and reaches the score pitch exactly at `seconds`, with
 * zero slope at either endpoint. Integration preserves oscillator phase. */
static double slide_phase(double frequency, double t, double semitones, double seconds)
{
    double u = clamp(t / seconds, 0.0, 1.0);
    double integral = seconds * (u - u*u*u + 0.5*u*u*u*u);
    return 2.0 * PI * frequency * (exp2(semitones / 12.0) - 1.0) * integral;
}

static void mix_voice(struct Audio *audio, const struct Note *note,
                      const struct VoiceSource *source)
{
    struct Resonator formants[3], nasal_filter;
    struct Resonator friction_filter;
    struct PhoneSequence phones;
    struct PhoneState phone_state;
    int has_ipa = note->ipa && note->ipa[0];
    double centers[3], widths[3], nasal, source_c[PARTIALS], source_s[PARTIALS];
    double sr = audio->rate, f = synth_frequency(note->midi), step = 2.0 * PI * f / sr;
    double cs = cos(step), sn = sin(step), osc_c = 1.0, osc_s = 0.0;
    double pan = (clamp(note->pan, -1.0, 1.0) + 1.0) * PI * 0.25;
    double left = cos(pan), right = sin(pan), chest = 0.0, noise_lp = 0.0;
    double chest_coefficient = exp(-2.0 * PI * 180.0 / sr);
    double friction_level = 0.0, aspiration_level = 0.0;
    double air_coefficient = exp(-1.0 / (0.0015 * sr));
    double retro_lp1 = 0, retro_lp2 = 0, retro_held = 0, retro_clock = 0;
    double retro_coefficient = exp(-2.0 * PI * 3400.0 / sr);
    double release = release_time(note->instrument), velocity = clamp(note->velocity, 0.0, 1.0);
    double largest_f = f * fmax(1.0, exp2(note->bend_semitones / 12.0));
    uint32_t seed = note_seed(note);
    size_t first, length, j;
    unsigned count = 0, h, band;
    char consonant = lower_consonant(note->consonant);
    if (note->start >= (double)audio->frames / sr || velocity == 0.0)
        return;
    first = (size_t)llround(note->start * sr);
    if (first >= audio->frames)
        return;
    if (has_ipa && phoneme_plan(note->ipa, note->duration, &phones) != 0)
        return; /* Already validated before allocation by synth_render_raw. */
    length = (note->duration + release) * sr >= (double)(audio->frames - first)
           ? audio->frames - first : (size_t)ceil((note->duration + release) * sr);
    double harmonic_limit = note->voice_style == VOICE_RETRO ? fmin(sr*.42,4630.0) : sr*.42;
    for (h = 0; h < PARTIALS && (h + 1) * largest_f < harmonic_limit; ++h) {
        double tilt = exp(-((double)h + 1.0) * f / 6200.0);
        source_c[h] = source->cosine[h] * tilt;
        source_s[h] = source->sine[h] * tilt;
        ++count;
    }
    voice_formants(note, 0.0, centers, widths, &nasal);
    if (has_ipa) {
        phoneme_at(&phones, 0, &phone_state);
        for (band = 0; band < 3; ++band) {
            centers[band] = phone_state.formant[band] * .94;
            widths[band] = phone_state.bandwidth[band] * 1.20;
        }
        nasal = phone_state.nasal;
    }
    for (band = 0; band < 3; ++band)
        resonator_init(&formants[band], centers[band], widths[band], sr);
    resonator_init(&nasal_filter, 270.0, 110.0, sr);
    resonator_init(&friction_filter, 3500.0, 1800.0, sr);
    for (j = 0; j < length; ++j) {
        double t = (double)j / sr, voiced = 1.0, frication = 0.0, aspiration = 0.0;
        double elapsed = fmax(0.0, t - 0.24), expression = smooth(elapsed / 0.20);
        double pm = f * (exp2(4.5 / 1200.0) - 1.0) / 4.9
                  * sin(2.0 * PI * 4.9 * elapsed) * expression;
        double source_sample = 0.0, signal, excitation, n = noise(&seed), filtered[3];
        double hc, hs, fc, fs, vc, vs;
        /* Small bounded phase fluctuations add grain without integrating random
         * frequency errors. The pitch reference therefore never wanders. */
        pm += expression * (0.004 * sin(2.0 * PI * 11.3 * t)
                           + 0.0015 * sin(2.0 * PI * 31.7 * t));
        if (note->bend_semitones != 0.0)
            pm += bend_phase(f, t, note->bend_semitones, 0.042);
        vc = cos(pm); vs = sin(pm);
        fc = osc_c * vc - osc_s * vs;
        fs = osc_s * vc + osc_c * vs;
        hc = fc; hs = fs;
        for (h = 0; h < count; ++h) {
            double next_c = hc * fc - hs * fs;
            source_sample += source_c[h] * hc + source_s[h] * hs;
            hs = hs * fc + hc * fs;
            hc = next_c;
        }
        source_sample *= 0.975 + 0.025 * sin(2.0 * PI * 7.13 * t);
        noise_lp = 0.72 * noise_lp + 0.28 * n;
        if ((j & 31u) == 0) {
            if (has_ipa) {
                phoneme_at(&phones, fmin(t, note->duration-1e-9), &phone_state);
                if (t >= note->duration) {
                    phone_state.voiced = 0;
                    phone_state.noise = 0;
                }
                if (phone_state.formant[0] > 0)
                    for (band = 0; band < 3; ++band) {
                        centers[band] = phone_state.formant[band] * .94;
                        widths[band] = phone_state.bandwidth[band] * 1.20;
                    }
                nasal = phone_state.nasal;
                double fric_center = note->voice_style == VOICE_RETRO ?
                    fmin(phone_state.fric_freq, 4500.0) : phone_state.fric_freq;
                if (fric_center > 0 && phone_state.fric_width > 0)
                    resonator_target(&friction_filter, fric_center,
                        phone_state.fric_width, sr);
            } else {
                voice_formants(note, ((double)j + 32.0) / sr, centers, widths, &nasal);
            }
            for (band = 0; band < 3; ++band)
                resonator_target(&formants[band], centers[band], widths[band], sr);
        }
        if (has_ipa) {
            voiced = phone_state.voiced;
            if (phone_state.fric_freq > 0)
                frication = phone_state.noise;
            else
                aspiration += phone_state.noise;
        } else switch (consonant) {
        case 'h':
            voiced = smooth(t / 0.055);
            aspiration = 0.16 * (1.0 - smooth(t / 0.070));
            break;
        case 'p': case 't': case 'k': case 'b': case 'd': case 'g':
            voiced = smooth((t - 0.020) / 0.035);
            frication = 0.20 * exp(-pow((t - 0.021) / 0.006, 2.0));
            if (consonant == 'b' || consonant == 'd' || consonant == 'g') {
                voiced = 0.20 + 0.80 * voiced;
                frication *= 0.55;
            }
            break;
        case 's': case 'f': case 'v':
            voiced = smooth((t - 0.025) / 0.040);
            frication = (consonant == 's' ? 0.23 : 0.12)
                      * (1.0 - smooth(t / 0.070));
            if (consonant == 'v')
                voiced = 0.35 + 0.65 * voiced;
            break;
        case 'q':
            voiced = smooth((t - 0.016) / 0.016);
            break;
        default:
            break;
        }
        /* Air has its own short envelope: suppress control-step clicks and
         * let a release settle without changing the voiced musical pitch.
         * Aspiration is aperiodic; only the quiet vowel breath is modulated
         * by the glottal cycle. */
        friction_level = air_coefficient * friction_level +
            (1.0 - air_coefficient) * frication;
        aspiration_level = air_coefficient * aspiration_level +
            (1.0 - air_coefficient) * aspiration;
        double vowel_breath = 0.007 * voiced * (0.45 + 0.55 * fabs(fs));
        excitation = source_sample * voiced +
            (n - noise_lp) * (vowel_breath + aspiration_level);
        for (band = 0; band < 3; ++band)
            filtered[band] = resonator_sample(&formants[band], excitation);
        chest = chest_coefficient * chest + (1.0 - chest_coefficient) * source_sample;
        double f2_gain = note->voice_style == VOICE_RETRO ? 1.90 : 1.15;
        double f3_gain = note->voice_style == VOICE_RETRO ? 1.25 : 0.85;
        double chest_gain = note->voice_style == VOICE_RETRO ? 0.10 : 0.24;
        signal = (filtered[0] - f2_gain * filtered[1] + f3_gain * filtered[2]) * (1.0 - 0.65 * nasal);
        signal += chest_gain * chest * voiced + 1.10 * nasal * resonator_sample(&nasal_filter, excitation);
        if (has_ipa)
            signal += friction_level * resonator_sample(&friction_filter, n);
        else
            signal += 0.65 * friction_level * (n - noise_lp);
        /* Gentle source compression rounds harsh closure spikes and preserves
         * their harmonics. It creates no new pitch clock or unrelated notes. */
        signal = signal / (1.0 + 0.45 * fabs(signal));
        if (note->voice_style == VOICE_RETRO) {
            retro_lp1 = retro_coefficient * retro_lp1 + (1-retro_coefficient) * signal;
            retro_lp2 = retro_coefficient * retro_lp2 + (1-retro_coefficient) * retro_lp1;
            retro_clock += 11025.0 / sr;
            if (retro_clock >= 1.0) {
                retro_clock -= floor(retro_clock);
                retro_held = round(clamp(retro_lp2*3, -1, 1)*63)/63;
            }
            signal = retro_held / 3.0;
        }
        signal *= 3.0 * velocity * gate(t, note->duration, 0.008, release);
        audio->samples[2 * (first + j)] += (float)(signal * left);
        audio->samples[2 * (first + j) + 1] += (float)(signal * right);
        if ((j & 4095u) == 4095u) {
            double phase = step * (double)(j + 1);
            osc_c = cos(phase); osc_s = sin(phase);
        } else {
            double next_c = osc_c * cs - osc_s * sn;
            osc_s = osc_s * cs + osc_c * sn;
            osc_c = next_c;
        }
    }
}

static void mix_note(struct Audio *audio, const struct Note *note,
                     const struct VoiceSource *source)
{
    struct Resonator guitar_body[3];
    double amplitude[PARTIALS], decay[PARTIALS];
    double tail[PARTIALS], tail_decay[PARTIALS], tail_share = 0.0, tail_tau = 1.0;
    double f = synth_frequency(note->midi), sr = (double)audio->rate;
    double release = release_time(note->instrument), attack = 0.004;
    double step = 2.0 * PI * f / sr, cs = cos(step), sn = sin(step);
    double osc_c = 1.0, osc_s = 0.0, spectral_sum = 0.0;
    double pan = (clamp(note->pan, -1.0, 1.0) + 1.0) * PI * 0.25;
    double left = cos(pan), right = sin(pan);
    double velocity = clamp(note->velocity, 0.0, 1.0);
    double noise_lp = 0.0, gain = velocity;
    size_t first, length, j;
    unsigned harmonics = 0, h;
    /* Guitar strings keep audible partials to about 5 kHz even on low notes;
     * sixteen stopped the low E near 1.3 kHz and dulled the strums. */
    unsigned limit = note->instrument == INST_GUITAR ? PARTIALS : LEGACY_PARTIALS;
    uint32_t seed = note_seed(note);
    static const double steel_weights[LEGACY_PARTIALS] = {
        .80, .90, .32, .14, .09, .07, .052, .039,
        .029, .023, .018, .014, .011, .008, .006, .004
    };

    if (is_voice(note->instrument)) {
        mix_voice(audio, note, source);
        return;
    }
    if (note->instrument == INST_GUITAR) {
        resonator_init(&guitar_body[0], 98.0, 28.0, sr);
        resonator_init(&guitar_body[1], 195.0, 42.0, sr);
        resonator_init(&guitar_body[2], 365.0, 65.0, sr);
    }

    if (note->start >= (double)audio->frames / sr || velocity == 0.0)
        return;
    first = (size_t)llround(note->start * sr);
    if (first >= audio->frames)
        return;
    /* Compare in floating point before converting, even for huge durations. */
    if ((note->duration + release) * sr >= (double)(audio->frames - first))
        length = audio->frames - first;
    else
        length = (size_t)ceil((note->duration + release) * sr);

    for (h = 1; h <= limit; ++h) {
        double a, tau;
        /* Leave a margin for vibrato and avoid harmonics at/above Nyquist. */
        if ((double)h * f * fmax(1.0, exp2(note->bend_semitones / 12.0)) >= sr * 0.45)
            break;
        switch (note->instrument) {
        case INST_GUITAR:
            /* Pluck-position comb plus higher-mode loss: bright initial pick,
             * then the fundamentals outlive the upper string harmonics. */
            a = sin(PI * 0.22 * (double)h) / pow((double)h, 1.28);
            tau = (1.20 + 50.0 / f) / (1.0 + 0.21 * (double)(h - 1));
            break;
        case INST_BASS:
            a = (h == 1 ? 1.0 : 0.42 / pow((double)h, 1.5));
            tau = 1.25 / (1.0 + 0.35 * (double)(h - 1));
            attack = 0.006;
            gain = velocity * 0.88;
            break;
        case INST_STEEL:
            a = steel_weights[h - 1];
            /* Held notes on the recording fade by roughly 12 dB/s at first.
             * A quieter, slower tail keeps the steel sustaining under the
             * other parts, so the ensemble still blends between notes. */
            tau = 0.6 / (1.0 + 0.055 * (double)(h - 1));
            tail_share = 0.25;
            tail_tau = 3.0 / (1.0 + 0.055 * (double)(h - 1));
            attack = 0.020;
            gain = velocity * 1.43; /* Level matches the earlier, slower steel. */
            break;
        case INST_PIANO:
            a = 1.0 / pow((double)h, 1.50);
            if (h == 4 || h == 6)
                a *= 1.5;
            tau = 1.55 / (1.0 + 0.24 * (double)(h - 1));
            gain = velocity * 0.85;
            break;
        default:
            a = 0.0;
            tau = 1.0;
            break;
        }
        amplitude[h - 1] = a * (1.0 - tail_share);
        decay[h - 1] = exp(-1.0 / (tau * sr));
        tail[h - 1] = a * tail_share;
        tail_decay[h - 1] = exp(-1.0 / (tail_tau * sr));
        /* Normalize on the legacy partials, so the added upper partials
         * brighten a guitar without changing its level in the mix. */
        if (h <= LEGACY_PARTIALS)
            spectral_sum += fabs(a);
        ++harmonics;
    }
    if (note->attack_seconds > 0.0)
        attack = note->attack_seconds;
    if (spectral_sum > 0.0)
        gain /= spectral_sum;
    if (seed == 0)
        seed = 1;

    for (j = 0; j < length; ++j) {
        double t = (double)j / sr, signal = 0.0;
        if (note->instrument == INST_SHAKER) {
            double n = noise(&seed);
            noise_lp = 0.60 * noise_lp + 0.40 * n;
            signal = (n - noise_lp) * exp(-t / 0.038)
                   * gate(t, note->duration, 0.002, release) * velocity * 0.25;
        } else {
            double fundamental_c = osc_c, fundamental_s = osc_s;
            double harmonic_c = fundamental_c, harmonic_s = fundamental_s;
            if (note->instrument == INST_STEEL || note->bend_semitones != 0.0) {
                /* Centered phase modulation gives a +/- 23 cent, 5.6 Hz bar
                 * vibrato after the attack, as measured on the recording's held
                 * steel notes. Its onset grows over 0.16 seconds. Base tuning
                 * remains A4 = 440 Hz, without integer-period error. */
                double elapsed = fmax(0.0, t - 0.22);
                double beta = f * (exp2(23.0 / 1200.0) - 1.0) / 5.6;
                double pm = note->instrument == INST_STEEL
                          ? beta * sin(2.0 * PI * 5.6 * elapsed) * smooth(elapsed / 0.16) : 0.0;
                if (note->bend_semitones != 0.0) {
                    if (note->bend_seconds > 0.0)
                        pm += slide_phase(f, t, note->bend_semitones, note->bend_seconds);
                    else
                        pm += bend_phase(f, t, note->bend_semitones,
                                         note->instrument == INST_STEEL ? 0.045 : 0.025);
                }
                double vc = cos(pm), vs = sin(pm);
                fundamental_c = osc_c * vc - osc_s * vs;
                fundamental_s = osc_s * vc + osc_c * vs;
                harmonic_c = fundamental_c;
                harmonic_s = fundamental_s;
            }
            for (h = 0; h < harmonics; ++h) {
                double next_c;
                signal += (amplitude[h] + tail[h]) * harmonic_s;
                amplitude[h] *= decay[h];
                tail[h] *= tail_decay[h];
                /* sin((h+1) phase), cos((h+1) phase) by a second rotation. */
                next_c = harmonic_c * fundamental_c - harmonic_s * fundamental_s;
                harmonic_s = harmonic_s * fundamental_c + harmonic_c * fundamental_s;
                harmonic_c = next_c;
            }
            /* Upper partials decay fastest. Stop rotating them once they are
             * 80 dB below a unit partial; the full mix changes by less than
             * 16-bit resolution, and 64-partial guitars stay affordable. */
            while (harmonics > 0 && fabs(amplitude[harmonics - 1])
                   + fabs(tail[harmonics - 1]) < 1e-4)
                --harmonics;
            signal *= gain * gate(t, note->duration, attack, release);
            if (note->instrument == INST_GUITAR && t < 0.06) {
                double n = noise(&seed);
                noise_lp = 0.75 * noise_lp + 0.25 * n;
                signal += (n - noise_lp) * exp(-t / 0.012)
                        * gate(t, note->duration, attack, release) * velocity * 0.030;
            }
            if (note->instrument == INST_GUITAR) {
                double body = 0.14 * resonator_sample(&guitar_body[0], signal)
                            + 0.09 * resonator_sample(&guitar_body[1], signal)
                            + 0.045 * resonator_sample(&guitar_body[2], signal);
                signal += body * gate(t, note->duration, attack, release);
            } else if (note->instrument == INST_BASS) {
                signal = signal / (1.0 + 0.32 * fabs(signal));
            }
            /* Use double precision; re-anchor occasionally to remove the tiny
             * accumulated roundoff from millions of oscillator rotations. */
            if ((j & 4095u) == 4095u) {
                double phase = step * (double)(j + 1);
                osc_c = cos(phase);
                osc_s = sin(phase);
            } else {
                double next_c = osc_c * cs - osc_s * sn;
                osc_s = osc_s * cs + osc_c * sn;
                osc_c = next_c;
            }
        }
        audio->samples[2 * (first + j)] += (float)(signal * left);
        audio->samples[2 * (first + j) + 1] += (float)(signal * right);
    }
}

struct Comb {
    float *delay;
    size_t length, cursor;
    double filtered;
};

static int room(struct Audio *audio)
{
    static const double times[8] = {
        .0297, .0371, .0411, .0437, .0307, .0383, .0427, .0451
    };
    struct Comb combs[8];
    size_t frame;
    unsigned c;
    memset(combs, 0, sizeof(combs));
    for (c = 0; c < 8; ++c) {
        combs[c].length = (size_t)llround(times[c] * (double)audio->rate);
        combs[c].delay = calloc(combs[c].length, sizeof(float));
        if (!combs[c].delay) {
            unsigned k;
            for (k = 0; k < c; ++k)
                free(combs[k].delay);
            return -1;
        }
    }
    for (frame = 0; frame < audio->frames; ++frame) {
        double input = (audio->samples[2 * frame] + audio->samples[2 * frame + 1]) * 0.5;
        double left = 0.0, right = 0.0;
        for (c = 0; c < 8; ++c) {
            struct Comb *comb = &combs[c];
            double delayed = comb->delay[comb->cursor];
            comb->filtered = 0.58 * comb->filtered + 0.42 * delayed;
            comb->delay[comb->cursor] = (float)(input + 0.75 * comb->filtered);
            if (++comb->cursor == comb->length)
                comb->cursor = 0;
            if (c < 4)
                left += delayed;
            else
                right += delayed;
        }
        audio->samples[2 * frame] += (float)(0.050 * left);
        audio->samples[2 * frame + 1] += (float)(0.050 * right);
    }
    for (c = 0; c < 8; ++c)
        free(combs[c].delay);
    return 0;
}

int synth_render_raw(const struct Note *notes, size_t n, struct Audio *out,
                     double seconds, unsigned rate, int wet)
{
    struct Audio audio;
    struct VoiceSource voice;
    double count;
    size_t i, fade;
    if (!out || (n && !notes) || !isfinite(seconds) || seconds <= 0.0
        || rate < 8000 || rate > 192000) {
        errno = EINVAL;
        return -1;
    }
    memset(out, 0, sizeof(*out));
    count = ceil(seconds * (double)rate);
    if (!isfinite(count) || count >= (double)(SIZE_MAX / (2 * sizeof(float)))) {
        errno = EOVERFLOW;
        return -1;
    }
    for (i = 0; i < n; ++i) {
        if (!isfinite(notes[i].start) || notes[i].start < 0.0
            || !isfinite(notes[i].duration) || notes[i].duration <= 0.0
            || !isfinite(notes[i].velocity) || !isfinite(notes[i].pan)
            || !isfinite(notes[i].bend_semitones) || fabs(notes[i].bend_semitones) > 12.0
            || !isfinite(notes[i].bend_seconds) || notes[i].bend_seconds < 0.0
            || notes[i].bend_seconds > notes[i].duration
            || !isfinite(notes[i].attack_seconds) || notes[i].attack_seconds < 0.0
            || notes[i].attack_seconds > notes[i].duration
            || notes[i].voice_style < VOICE_BARITONE || notes[i].voice_style > VOICE_RETRO
            || notes[i].midi < 0 || notes[i].midi > 127
            || notes[i].instrument < INST_GUITAR || notes[i].instrument > INST_VOICE_U) {
            errno = EINVAL;
            return -1;
        }
        if (is_voice(notes[i].instrument) && notes[i].ipa && notes[i].ipa[0]) {
            struct PhoneSequence check;
            if (phoneme_plan(notes[i].ipa, notes[i].duration, &check) != 0)
                return -1;
        }
    }
    audio.frames = (size_t)count;
    audio.rate = rate;
    audio.samples = calloc(audio.frames * 2, sizeof(float));
    if (!audio.samples)
        return -1;
    /* This is inexpensive (one integration per render), and keeping it local
     * makes concurrent calls reentrant without a mutable global lookup table. */
    prepare_voice(&voice);
    for (i = 0; i < n; ++i)
        mix_note(&audio, &notes[i], &voice);
    if (wet && room(&audio) < 0) {
        free(audio.samples);
        return -1;
    }
    /* The score normally leaves time for releases; this short fade also makes
     * deliberately truncated diagnostic renders end without a hard click. */
    fade = (size_t)((double)rate * 0.035);
    if (fade > audio.frames)
        fade = audio.frames;
    for (i = 0; i < fade; ++i) {
        double env = smooth((double)i / (double)fade);
        size_t frame = audio.frames - 1 - i;
        audio.samples[2 * frame] *= (float)env;
        audio.samples[2 * frame + 1] *= (float)env;
    }
    for (i = 0; i < audio.frames * 2; ++i) {
        double sample = audio.samples[i];
        if (!isfinite(sample)) {
            free(audio.samples);
            errno = ERANGE;
            return -1;
        }
    }
    *out = audio;
    return 0;
}

int synth_render(const struct Note *notes, size_t n, struct Audio *out,
                 double seconds, unsigned rate, double volume, int wet)
{
    double peak = 0.0, gain;
    size_t i;
    if (!isfinite(volume)) {
        errno = EINVAL;
        return -1;
    }
    if (synth_render_raw(notes, n, out, seconds, rate, wet) < 0)
        return -1;
    for (i = 0; i < out->frames * 2; ++i)
        if (fabs(out->samples[i]) > peak)
            peak = fabs(out->samples[i]);
    gain = peak > 0.0 ? clamp(volume, 0.0, 0.9) / peak : 0.0;
    for (i = 0; i < out->frames * 2; ++i)
        out->samples[i] = (float)(out->samples[i] * gain);
    return 0;
}

int synth_note_render(const struct Note *note, struct Audio *out,
                      double seconds, unsigned rate, double volume)
{
    struct Note copy;
    if (!note) {
        errno = EINVAL;
        return -1;
    }
    copy = *note;
    copy.start = 0.0;
    return synth_render(&copy, 1, out, seconds, rate, volume, 0);
}

void synth_free(struct Audio *audio)
{
    if (audio) {
        free(audio->samples);
        memset(audio, 0, sizeof(*audio));
    }
}
