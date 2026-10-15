#ifndef ALOHA_SCORE_H
#define ALOHA_SCORE_H
#include "synth.h"

enum Section { SECTION_FULL, SECTION_VERSE, SECTION_CHORUS };
enum PlayMode { MODE_INSTRUMENTAL, MODE_VOCAL, MODE_ENSEMBLE };
/* One sung syllable: text[0..length) is printed when it starts. A syllable
 * that begins a word follows a space unless it also begins the line. */
struct LyricCue {
    double start;
    double end;
    const char *text;
    size_t length;
    int word_start;
    int line_end;
};
struct Score {
    struct Note *notes;
    size_t count;
    double seconds;
    double bpm;
    int transpose;
    int voice;
    struct LyricCue *lyrics;
    size_t lyric_count;
};

int score_build(struct Score *, enum Section, double bpm, int transpose, int lead_only, int voice);
int score_check(void);
int score_csv(const struct Score *, const char *path);
void score_free(struct Score *);
#endif
