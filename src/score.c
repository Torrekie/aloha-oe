#include "score.h"
#include "melody.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Score time is an integer sixteenth-note grid, PPQ=4. Conversion uses the
 * absolute grid position, so rounding does not accumulate across notes. The
 * melody is transposed from the source Ab major to E major, down 4 semitones. */
enum { NOTE_CAPACITY = 8192, LYRIC_CAPACITY = 128, KEY_SHIFT = -4 };

/* Word boundaries refer to the same source note indices used by the IPA
 * syllables below. A word lists one '|'-separated syllable per note, matching
 * the sung IPA; an empty syllable holds the previous one across that note.
 * The valley note alone carries two syllables, as the voice splits it. The
 * final verse rest is deliberately absent from this table. */
struct LyricWord { size_t first, last; const char *text; int line_end; };
static const struct LyricWord verse_words[] = {
    {0,1,"Proud|ly",0}, {2,2,"swept",0}, {3,3,"the",0},
    {4,4,"rain",0}, {5,5,"cloud",0}, {6,6,"by",0},
    {7,7,"the",0}, {8,8,"cliffs,",1},
    {9,9,"As",0}, {10,10,"on",0}, {11,11,"it",0},
    {12,13,"gli|ded",0}, {14,14,"through",0}, {15,15,"the",0},
    {16,16,"trees,",1},
    {17,17,"Still",0}, {18,20,"fol|low|ing",0}, {21,21,"with",0},
    {22,22,"glee",0}, {23,23,"the",0}, {24,25,"li|ko,",1},
    {26,26,"The",0}, {27,29,"ʻā|hi|hi",0}, {30,32,"le|hu|a",0},
    {33,33,"of",0}, {34,34,"the",0}, {35,35,"val|ley.",1}
};
static const struct LyricWord chorus_words[] = {
    {0,2,"A|lo|ha",0}, {3,3,"ʻoe,",0},
    {4,6,"a|lo|ha",0}, {7,7,"ʻoe,",1},
    {8,8,"E",0}, {9,9,"ke",0}, {10,13,"o|na|o|na",0},
    {14,15,"no|ho",0}, {16,16,"i",0}, {17,17,"ka",0},
    {18,19,"li|po,",1},
    {20,20,"One",0}, {21,21,"fond",0}, {22,23,"em|brace,",1},
    {24,24,"A",0}, {25,26,"ho|ʻi",0}, {27,28,"a|ʻe",0}, {29,29,"au,",1},
    {30,31,"Un|til",0}, {32,32,"we",0}, {33,34,"meet|",0},
    {35,36,"a|gain.",1}
};

static int
is_valley(const MelodyNote *notes, size_t index)
{
    return notes == aloha_verse && index+2 == ALOHA_VERSE_NOTE_COUNT;
}

/* Count a word's syllables, and the printed (nonempty) ones among them. */
static size_t
word_syllables(const struct LyricWord *word, size_t *printed)
{
    size_t total = 1, shown = 0, length = 0;
    for (const char *c = word->text;; ++c) {
        if (*c == '|' || *c == '\0') {
            shown += length != 0;
            if (*c == '\0') break;
            ++total;
            length = 0;
        } else {
            ++length;
        }
    }
    if (printed) *printed = shown;
    return total;
}

static int
add_lyrics(struct Score *score, const MelodyNote *notes, size_t count,
    int start_tick)
{
    const struct LyricWord *words = notes == aloha_verse ? verse_words : chorus_words;
    size_t word_count = notes == aloha_verse ?
        sizeof(verse_words)/sizeof(verse_words[0]) :
        sizeof(chorus_words)/sizeof(chorus_words[0]);
    for (size_t w = 0; w < word_count; ++w) {
        const struct LyricWord *word = &words[w];
        if (word->first > word->last || word->last >= count) {
            errno = EINVAL; return -1;
        }
        for (size_t n = word->first; n <= word->last; ++n)
            if (!notes[n].midi) { errno = EINVAL; return -1; }
        int valley = word->first == word->last && is_valley(notes, word->last);
        size_t printed, syllables = word_syllables(word, &printed);
        if (syllables != (valley ? 2 : word->last - word->first + 1) ||
            word->text[0] == '|') {
            errno = EINVAL; return -1;
        }
        if (score->lyric_count + printed > LYRIC_CAPACITY) {
            errno = EOVERFLOW; return -1;
        }
        int tick = start_tick;
        for (size_t n = 0; n < word->first; ++n)
            tick += notes[n].ticks;
        const char *text = word->text;
        for (size_t i = 0; i < syllables; ++i) {
            size_t length = strcspn(text, "|");
            /* Beats from the word's first note to this syllable and to the
             * end of its sung note, as the voice part schedules them. */
            double from, to;
            if (valley) {
                from = i ? 2.0 : 0.0;
                to = i ? 2.92 : 2.0;
            } else {
                int at = tick;
                for (size_t n = word->first; n < word->first + i; ++n)
                    at += notes[n].ticks;
                from = (double)(at - tick) / ALOHA_PPQ;
                to = from + (double)notes[word->first + i].ticks / ALOHA_PPQ * 0.97;
            }
            double start = ((double)tick / ALOHA_PPQ + from) * 60 / score->bpm;
            double end = ((double)tick / ALOHA_PPQ + to) * 60 / score->bpm;
            if (length == 0) {
                /* A held syllable extends the one before it. */
                score->lyrics[score->lyric_count-1].end = end;
            } else {
                struct LyricCue cue = {
                    .start = start, .end = end, .text = text, .length = length,
                    .word_start = i == 0,
                    .line_end = word->line_end && i + 1 == syllables
                };
                if (score->lyric_count &&
                    score->lyrics[score->lyric_count-1].end > cue.start) {
                    errno = EINVAL; return -1;
                }
                score->lyrics[score->lyric_count++] = cue;
            }
            text += length + (text[length] == '|');
        }
    }
    return 0;
}

static int
add_note(struct Score *score, double beat, double length, int midi,
    double velocity, double pan, enum Instrument instrument)
{
    if (score->count >= NOTE_CAPACITY || beat < 0 || length <= 0 || midi < 0 || midi > 127) {
        errno = EINVAL; return -1;
    }
    struct Note *note = &score->notes[score->count++];
    *note = (struct Note){ .start = beat * 60 / score->bpm,
        .duration = length * 60 / score->bpm, .midi = midi + score->transpose,
        .velocity = velocity, .pan = pan, .instrument = instrument };
    return 0;
}

static int
melody(struct Score *score, const MelodyNote *notes, size_t count,
    int start_tick, double velocity)
{
    int tick = start_tick;
    /* Approximate English verse / Hawaiian-English refrain syllables. This
     * phonetic score contains symbolic vowels/consonants, never recorded voice.
     * The melody data stays exactly as independently checked in melody.h. */
    struct Phone { enum Instrument vowel, end; char consonant; };
#define A INST_VOICE_A
#define E INST_VOICE_E
#define I INST_VOICE_I
#define O INST_VOICE_O
#define U INST_VOICE_U
    static const struct Phone verse_phones[] = {
        {A,U,'p'}, {I,0,'l'}, {E,0,'s'}, {E,0,'d'}, {E,I,'r'},
        {A,U,'k'}, {A,I,'b'}, {E,0,'d'}, {I,0,'k'}, {A,0,0},
        {O,0,0}, {I,0,0}, {A,I,'g'}, {E,0,'d'}, {U,0,'t'},
        {E,0,'d'}, {I,0,'t'}, {I,0,'s'}, {O,0,'f'}, {O,U,'l'},
        {I,0,0}, {I,0,'w'}, {I,0,'g'}, {E,0,'d'}, {I,0,'l'},
        {O,U,'k'}, {E,0,'d'}, {A,0,0}, {I,0,'h'}, {I,0,'h'},
        {E,0,'l'}, {U,0,'h'}, {A,0,0}, {O,0,0}, {E,0,'d'},
        {A,I,'v'}, {I,0,0}
    };
    static const struct Phone chorus_phones[] = {
        {A,0,'q'},
        {O,0,'l'}, {A,0,'h'}, {O,E,0}, {A,0,'q'},
        {O,0,'l'}, {A,0,'h'}, {O,E,0}, {E,0,0}, {E,0,'k'},
        {O,0,'q'}, {A,0,'n'}, {O,0,'q'}, {A,0,'n'},
        {O,0,'n'}, {O,0,'h'}, {I,0,'q'}, {A,0,'k'},
        {I,0,'l'}, {O,0,'p'}, {O,U,'w'},
        {O,0,'f'}, {E,0,'q'}, {E,I,'b'}, {A,0,'q'},
        {O,I,'h'}, {I,0,'q'}, {A,0,'q'}, {E,0,'q'},
        {A,U,'q'}, {U,0,0},
        {I,0,'t'}, {I,0,'w'}, {I,0,'m'}, {I,0,0}, {E,0,0},
        {E,I,'g'}
    };
    static const char *const verse_ipa[] = {
        "prɑʊd", "li", "swɛpt", "ðə", "reɪn", "klaʊd", "baɪ", "ðə",
        "klɪfs", "æz", "ɑn", "ɪt", "glaɪ", "dɪd", "θru", "ðə", "triz",
        "stɪl", "fɑ", "loʊ", "ɪŋ", "wɪð", "gli", "ðə", "li", "koʊ",
        "ðə", "ɑ", "hi", "hi", "lɛ", "hu", "ɑ", "ʌv", "ðə", "væ", "li"
    };
    static const char *const chorus_ipa[] = {
        "ɑ", "lo", "hɑ", "ʔoɛ", "ɑ", "lo", "hɑ", "ʔoɛ", "ɛ", "kɛ",
        "o", "nɑ", "o", "nɑ", "no", "ho", "i", "kɑ", "li", "po",
        "wʌn", "fɑnd", "ɛm", "breɪs", "ɑ", "ho", "ʔi", "ɑ", "ʔɛ",
        "ɑʊ", "ʌn", "tɪl", "wi", "mi", "it", "ə", "geɪn"
    };
#undef A
#undef E
#undef I
#undef O
#undef U
    for (size_t i = 0; i < count; ++i) {
        double beat = (double)tick / ALOHA_PPQ;
        double length = (double)notes[i].ticks / ALOHA_PPQ;
        int pitch = notes[i].midi + KEY_SHIFT;
        if (notes[i].midi && score->voice != MODE_VOCAL) {
            double gain = score->voice == MODE_ENSEMBLE ? 0.24 : 0.80;
            if (add_note(score, beat, length, pitch, velocity*gain,
                0.52, INST_STEEL)) return -1;
            /* Small steel pick/slide gestures settle onto the written pitch. */
            score->notes[score->count-1].bend_semitones = length >= 1.0 ? -0.22 : 0;
        }
        if (notes[i].midi && score->voice != MODE_INSTRUMENTAL) {
            const struct Phone *phone = notes == aloha_chorus ? &chorus_phones[i] : &verse_phones[i];
            double vocal_length = length * 0.97;
            /* The final English word has two syllables. Quincke separately
             * articulates the same half/quarter pair that Lampe ties together. */
            int split = is_valley(notes, i);
            if (split) vocal_length = 2.0;
            if (add_note(score, beat, vocal_length, pitch-12,
                velocity*0.75, 0.0, phone->vowel)) return -1;
            struct Note *voice = &score->notes[score->count-1];
            voice->vowel_end = split ? 0 : phone->end;
            voice->consonant = phone->consonant;
            voice->ipa = notes == aloha_chorus ? chorus_ipa[i] : verse_ipa[i];
            voice->bend_semitones = length >= 1.0 ? -0.12 : 0;
            if (split) {
                if (add_note(score, beat+2, 0.92, pitch-12,
                    velocity*.64, 0.0, INST_VOICE_I)) return -1;
                score->notes[score->count-1].consonant = 'l';
                score->notes[score->count-1].ipa = "li";
            }
        }
        tick += notes[i].ticks;
    }
    return 0;
}

enum Chord { CHORD_I, CHORD_IV, CHORD_V7 };
struct Harmony { int root, fifth; int guitar[6]; int keys[4]; };
static const struct Harmony harmony[] = {
    {40, 47, {40,47,52,56,59,64}, {52,56,59,64}},
    {45, 40, {45,52,57,61,64,69}, {57,61,64,69}},
    {47, 42, {47,54,57,63,66,71}, {54,57,63,66}}
};

static int
accompany(struct Score *score, double first_beat, const enum Chord *chords,
    size_t bars, double intensity)
{
    for (size_t bar = 0; bar < bars; ++bar) {
        const struct Harmony *chord = &harmony[chords[bar]];
        double base = first_beat + (double)bar * 4;
        for (int beat = 0; beat < 4; beat += 2) {
            int bass = beat ? chord->fifth : chord->root;
            if (add_note(score, base+beat, 1.42, bass,
                (beat ? .083 : .096) * intensity, 0.0, INST_BASS)) return -1;
        }
        /* One soft rhythm guitar, plus a separately voiced fingerpicked
         * acoustic on the right. No drum/percussion part is enabled by default.
         * Small deterministic rake/velocity variations keep the accompaniment
         * from sounding like identical repeated full-keyboard chords. */
        static const double strokes[] = {0, 1.5, 2, 3.5};
        for (int stroke = 0; stroke < 4; ++stroke) {
            double human = 1.0 + .05*sin((first_beat/4+(double)bar)*1.7+stroke*.9);
            double velocity = ((stroke & 1) ? 0.031 : 0.045) * intensity * human;
            int up = stroke & 1;
            for (int string = 0; string < 6; ++string) {
                int index = up ? 5-string : string;
                double offset = string * (up ? .0032 : .0052) * score->bpm / 60;
                if (add_note(score, base+strokes[stroke]+offset, 0.65,
                    chord->guitar[index], velocity, -0.70, INST_GUITAR)) return -1;
            }
        }
        static const int fingers[] = {0,3,2,4,1,3,2,5};
        for (int pick = 0; pick < 8; ++pick) {
            double strength = (pick%4 == 0 ? .082 : .057) * intensity;
            if (add_note(score, base+pick*.5, .72,
                chord->guitar[fingers[pick]], strength, .68, INST_GUITAR)) return -1;
            /* Optional decorations are present in the score but CLI gains
             * start at zero for these two parts (Cash's album has no drums). */
            if (add_note(score, base+pick*.5, .05, 60,
                .018*intensity, .35, INST_SHAKER)) return -1;
        }
        for (int beat = 0; beat < 4; ++beat) {
            if (add_note(score, base+beat+0.5, 0.90, chord->keys[beat],
                0.045*intensity, 0.45, INST_PIANO)) return -1;
        }
    }
    return 0;
}

/* These fills and the accompaniment are this toy's arrangement. They are
 * not represented as a note-for-note transcription of the Cash recording. */
static int
fill(struct Score *score, double beat, const int *pitches, size_t count,
    double step, double velocity)
{
    for (size_t i = 0; i < count; ++i) {
        if (add_note(score, beat+i*step, step, pitches[i],
            velocity, 0.55, INST_STEEL)) return -1;
        struct Note *note = &score->notes[score->count-1];
        /* Connect from the preceding pitch, including downward slides,
         * instead of giving every destination the same upward scoop. */
        note->bend_semitones = i ? pitches[i-1] - pitches[i] : -1.0;
        note->bend_seconds = step * 60 / score->bpm * 0.55;
        note->attack_seconds = fmin(0.055, note->duration * 0.18);
    }
    return 0;
}

/* Times are seconds at the default 49 BPM, scaled to the requested tempo. */
static int
recording_slide(struct Score *score, double start, double duration, int from,
    int to, double glide, double velocity, double attack)
{
    if (add_note(score, start * 49 / 60, duration * 49 / 60,
        to, velocity, .55, INST_STEEL)) return -1;
    struct Note *note = &score->notes[score->count-1];
    note->bend_semitones = from - to;
    note->bend_seconds = glide * 49 / score->bpm;
    note->attack_seconds = attack * 49 / score->bpm;
    return 0;
}

static int
intro_slides(struct Score *score)
{
    /* Alternating slides and holds, with smoothly joined pitch endpoints. */
    return recording_slide(score, .25, .80, 66, 68, .35, .145, .060) ||
        recording_slide(score, 1.05, 2.45, 68, 66, .85, .140, .045) ||
        recording_slide(score, 3.60, .53, 68, 68, 0, .140, .030) ||
        recording_slide(score, 4.13, 1.23, 68, 69, .20, .135, .040) ||
        recording_slide(score, 5.36, .14, 69, 68, .10, .120, .025) ||
        recording_slide(score, 5.50, 1.0, 68, 66, .55, .135, .045) ||
        recording_slide(score, 6.50, 2.30, 66, 68, .38, .115, .055);
}

int
score_build(struct Score *score, enum Section section, double bpm,
    int transpose, int lead_only, int voice)
{
    if (!score || section < SECTION_FULL || section > SECTION_CHORUS ||
        !isfinite(bpm) || bpm < 25 || bpm > 150 || transpose < -12 || transpose > 12 ||
        voice < MODE_INSTRUMENTAL || voice > MODE_ENSEMBLE) {
        errno = EINVAL; return -1;
    }
    memset(score, 0, sizeof(*score));
    score->notes = calloc(NOTE_CAPACITY, sizeof(*score->notes));
    if (!score->notes) return -1;
    score->lyrics = calloc(LYRIC_CAPACITY, sizeof(*score->lyrics));
    if (!score->lyrics) goto failed;
    score->bpm = bpm;
    score->transpose = transpose;
    score->voice = voice;
    static const enum Chord verse_chords[8] = {
        CHORD_I, CHORD_V7, CHORD_V7, CHORD_I,
        CHORD_I, CHORD_V7, CHORD_V7, CHORD_I
    };
    static const enum Chord chorus_chords[8] = {
        CHORD_IV, CHORD_I, CHORD_V7, CHORD_I,
        CHORD_IV, CHORD_I, CHORD_V7, CHORD_I
    };
    static const enum Chord intro_chords[2] = {CHORD_I, CHORD_V7};
    static const enum Chord outro_chords[2] = {CHORD_V7, CHORD_I};
    static const int turnaround[] = {68,66,64,59};
    static const int coda[] = {69,68,66,64,59,64};
    size_t nv = sizeof(aloha_verse)/sizeof(aloha_verse[0]);
    size_t nc = sizeof(aloha_chorus)/sizeof(aloha_chorus[0]);
    if (section == SECTION_FULL) {
        /* One-beat lead-in, two-bar intro and coda, and four eight-bar
         * sections: 145 beats, followed by a short decay. */
        score->seconds = 145 * 60 / bpm + 2.2489795918367347;
        if (melody(score, aloha_verse, nv, 32, 0.43) ||
            melody(score, aloha_chorus, nc, 162, 0.47) ||
            melody(score, aloha_chorus, nc, 418, 0.50)) goto failed;
        /* These three vocal sections own the lyric timeline in every mode.
         * The instrumental refrain below intentionally has no lyric cues. */
        if (add_lyrics(score, aloha_verse, nv, 32) ||
            add_lyrics(score, aloha_chorus, nc, 162) ||
            add_lyrics(score, aloha_chorus, nc, 418)) goto failed;
        if (!lead_only) {
            int mode = score->voice;
            /* Instrumental solo remains present in vocal/ensemble modes. */
            score->voice = MODE_INSTRUMENTAL;
            if (melody(score, aloha_chorus, nc, 290, 0.30)) goto failed;
            score->voice = mode;
            if (accompany(score, 1, intro_chords, 2, 0.72) ||
                accompany(score, 9, verse_chords, 8, 0.82) ||
                accompany(score, 41, chorus_chords, 8, 0.95) ||
                accompany(score, 73, chorus_chords, 8, 0.76) ||
                accompany(score, 105, chorus_chords, 8, 1.0) ||
                accompany(score, 137, outro_chords, 2, 0.65) ||
                intro_slides(score) ||
                /* A quiet descending slide answers the instrumental melody. */
                recording_slide(score, 91.50, 1.50, 66, 61, .50, .055, .060) ||
                fill(score, 72, turnaround, 4, .25, .10) ||
                fill(score, 136, turnaround, 4, .25, .10) ||
                fill(score, 137, coda, 6, 1.0, .17) ||
                /* The final steel note sets the song's peak. Its velocity
                 * keeps that peak where it was before the steel gain rose. */
                add_note(score, 141, 4, 64, .167, .55, INST_STEEL)) goto failed;
            for (int i = 0; i < 6; ++i)
                if (add_note(score, i*.003, .95, harmony[CHORD_I].guitar[i],
                    .050, -.70, INST_GUITAR)) goto failed;
            /* Slide answers under held phrase endings rather than a constant
             * second loud lead. These are arrangement gestures, not claimed
             * as a full transcription of Jack Clement's recorded playing. */
            static const int answer[] = {71,68,66};
            if (fill(score, 15.5, answer, 3, .35, .060) ||
                fill(score, 23.5, answer, 3, .35, .055) ||
                fill(score, 47.5, answer, 3, .35, .055) ||
                fill(score, 55.5, answer, 3, .35, .055) ||
                fill(score, 111.5, answer, 3, .35, .055) ||
                fill(score, 119.5, answer, 3, .35, .055)) goto failed;
            /* A final E chord decays into the three-second room tail. */
            for (int i = 0; i < 6; ++i)
                if (add_note(score, 141+i*.003, 4, harmony[CHORD_I].guitar[i],
                    .06, -.30, INST_GUITAR)) goto failed;
        }
    } else {
        const MelodyNote *phrase = section == SECTION_VERSE ? aloha_verse : aloha_chorus;
        size_t count = section == SECTION_VERSE ? nv : nc;
        int pickup = section == SECTION_VERSE ? 4 : 2;
        const enum Chord *chords = section == SECTION_VERSE ? verse_chords : chorus_chords;
        /* Shift a standalone phrase forward by its pickup so none is lost. */
        score->seconds = (32 + (double)pickup / ALOHA_PPQ) * 60 / bpm + 3;
        if (melody(score, phrase, count, 0, 0.48) ||
            add_lyrics(score, phrase, count, 0)) goto failed;
        if (!lead_only) {
            struct Score part = *score;
            if (accompany(&part, 0, chords, 8, 0.92)) goto failed;
            double shift = (double)pickup / ALOHA_PPQ * 60 / bpm;
            for (size_t i = score->count; i < part.count; ++i)
                score->notes[i].start += shift;
            score->count = part.count;
        }
    }
    return 0;
failed:
    score_free(score); return -1;
}

static int
check_lyrics(const struct Score *score, enum Section section)
{
    size_t verse_count = 0, chorus_count = 0, printed;
    for (size_t w = 0; w < sizeof(verse_words)/sizeof(verse_words[0]); ++w) {
        word_syllables(&verse_words[w], &printed);
        verse_count += printed;
    }
    for (size_t w = 0; w < sizeof(chorus_words)/sizeof(chorus_words[0]); ++w) {
        word_syllables(&chorus_words[w], &printed);
        chorus_count += printed;
    }
    size_t expected = section == SECTION_FULL ? verse_count+2*chorus_count :
        section == SECTION_VERSE ? verse_count : chorus_count;
    unsigned line_ends = 0;
    if (score->lyric_count != expected) return -1;
    for (size_t i = 0; i < score->lyric_count; ++i) {
        const struct LyricCue *cue = &score->lyrics[i];
        if (!cue->text || !cue->length || !isfinite(cue->start) ||
            !isfinite(cue->end) || cue->start < 0 || cue->end <= cue->start ||
            cue->end > score->seconds || (i && cue->start < score->lyrics[i-1].end) ||
            (cue->line_end != 0 && cue->line_end != 1)) return -1;
        line_ends += cue->line_end;
        /* No lyric intersects the interlude, including its half-beat pickup. */
        if (section == SECTION_FULL && cue->start < 104.5*60/score->bpm &&
            cue->end > 72*60/score->bpm) return -1;
        if (score->voice != MODE_INSTRUMENTAL) {
            int found_start = 0, found_end = 0;
            for (size_t n = 0; n < score->count; ++n) {
                const struct Note *note = &score->notes[n];
                if (!note->ipa) continue;
                if (fabs(note->start-cue->start) < 1e-9) found_start = 1;
                if (fabs(note->start+note->duration-cue->end) < 1e-9) found_end = 1;
            }
            if (!found_start || !found_end) return -1;
        }
    }
    if (line_ends != (section == SECTION_FULL ? 14u :
        section == SECTION_VERSE ? 4u : 5u)) return -1;
    for (size_t n = 0; n < score->count; ++n) {
        const struct Note *note = &score->notes[n];
        if (!note->ipa) continue;
        unsigned owners = 0;
        for (size_t i = 0; i < score->lyric_count; ++i)
            if (note->start+1e-9 >= score->lyrics[i].start &&
                note->start+note->duration <= score->lyrics[i].end+1e-9) ++owners;
        if (owners != 1) return -1;
    }
    return 0;
}

int
score_check(void)
{
    unsigned verse = 0, chorus = 0;
    for (size_t i = 0; i < sizeof(aloha_verse)/sizeof(aloha_verse[0]); ++i) {
        if (!aloha_verse[i].ticks) return -1;
        verse += aloha_verse[i].ticks;
    }
    for (size_t i = 0; i < sizeof(aloha_chorus)/sizeof(aloha_chorus[0]); ++i) {
        if (!aloha_chorus[i].ticks) return -1;
        chorus += aloha_chorus[i].ticks;
    }
    if (verse != 130 || chorus != 126) {
        fprintf(stderr, "Score lengths failed: verse %u, chorus %u\n", verse, chorus);
        return -1;
    }
    struct Score score = {0};
    if (score_build(&score, SECTION_FULL, 49, 0, 1, 0)) return -1;
    double worst = 0;
    for (size_t i = 0; i < score.count; ++i) {
        const struct Note *note = &score.notes[i];
        double grid = note->start * 49 / 60 * ALOHA_PPQ;
        if (fabs(grid-round(grid)) > 1e-9 || note->midi < 0 || note->midi > 127 ||
            note->start+note->duration > score.seconds-3) {
            score_free(&score); return -1;
        }
        double sample = note->start*48000;
        double rounding = fabs(sample-round(sample));
        if (rounding > worst) worst = rounding;
    }
    score_free(&score);
    static const double tempos[] = {25, 49, 150};
    for (size_t t = 0; t < sizeof(tempos)/sizeof(tempos[0]); ++t)
        for (int section = SECTION_FULL; section <= SECTION_CHORUS; ++section)
            for (int mode = MODE_INSTRUMENTAL; mode <= MODE_ENSEMBLE; ++mode) {
                if (score_build(&score, (enum Section)section, tempos[t], 0, 1, mode))
                    return -1;
                int failed = check_lyrics(&score, (enum Section)section);
                score_free(&score);
                if (failed) {
                    fprintf(stderr, "Lyric cue validation failed at tempo %.0f, section %d, mode %d\n",
                        tempos[t], section, mode);
                    return -1;
                }
            }
    printf("Score: source verse 130 ticks + chorus 126 ticks = 16 common-time bars;\n"
        "all melody onsets on sixteenth grid, maximum 48kHz rounding %.3f sample\n"
        "Lyrics: 71 full-song words, vocal onsets/ends and interlude silence checked\n", worst);
    return 0;
}

int
score_csv(const struct Score *score, const char *path)
{
    FILE *file = fopen(path, "w");
    if (!file) { perror(path); return -1; }
    int failed = fprintf(file, "instrument,midi,hz,start_seconds,duration_seconds,velocity,pan,bend_semitones,vowel_end,consonant,ipa,voice_style,bend_seconds,attack_seconds\n") < 0;
    for (size_t i = 0; !failed && i < score->count; ++i) {
        const struct Note *note = &score->notes[i];
        failed = fprintf(file, "%s,%d,%.9f,%.9f,%.9f,%.6f,%.6f,%.6f,%s,%c,%s,%s,%.6f,%.6f\n",
            synth_instrument_name(note->instrument), note->midi,
            synth_frequency(note->midi), note->start, note->duration,
            note->velocity, note->pan, note->bend_semitones,
            note->vowel_end >= INST_VOICE_A && note->vowel_end <= INST_VOICE_U ?
            synth_instrument_name(note->vowel_end) : "hold",
            note->consonant ? note->consonant : '-', note->ipa ? note->ipa : "",
            note->voice_style == VOICE_RETRO ? "retro" : "baritone",
            note->bend_seconds, note->attack_seconds) < 0;
    }
    if (fclose(file)) failed = 1;
    if (failed) fprintf(stderr, "Cannot finish score CSV: %s\n", path);
    return failed ? -1 : 0;
}

void
score_free(struct Score *score)
{
    if (score) {
        free(score->notes);
        free(score->lyrics);
        memset(score, 0, sizeof(*score));
    }
}
