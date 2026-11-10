#ifndef PSX_INPUT_UPDATE_CLOCK_H
#define PSX_INPUT_UPDATE_CLOCK_H
#include <stdint.h>
/* Experimental two-stage controller clock. No guest state and no title data.
 * The caller must verify the packet at its configured acceptance marker.
 * A poll prepares one packet ahead; the later marker commits the older one. */
typedef struct {
    const uint16_t *words;
    uint32_t count, accepted;
    int primed, predicted, failed;
    uint64_t last_poll, last_accept;
    uint16_t queued_word;
    const uint8_t *contexts; /* Optional source event types: 0 normal, 1 auxiliary. */
    int compatible_contexts, context_failed;
    int neutral_refresh_stage; /* 1 hold poll, 2 held acceptance, 3 refresh poll. */
    uint64_t neutral_hold_frame;
    uint32_t neutral_refreshes;
    uint16_t protected_raw_mask;
    uint16_t source_raw_hold, source_raw_press, native_raw_hold, native_raw_press;
    uint8_t last_source_context, last_native_context;
} InputUpdateClock;
/* Optional prediction only. The later packet/context checks remain authoritative.
 * A gate of one may be reset by a continuing main loop after the early poll. */
static inline int input_update_predict(const InputUpdateClock *s, uint64_t frame,
                                       uint32_t gate, int previous_accept) {
    return gate==0 || (previous_accept && s->primed && gate==1 &&
                       frame>s->last_accept && frame-s->last_accept==1);
}
static inline int input_update_init(InputUpdateClock *s, const uint16_t *w, uint32_t n) {
    *s = (InputUpdateClock){0};
    if (!w || n < 2 || w[0] != 0xffff || w[1] != 0xffff) return 0;
    s->words=w; s->count=n;
    s->source_raw_hold=s->native_raw_hold=0xffff; return 1;
}
static inline int input_update_set_contexts(InputUpdateClock *s, const uint8_t *contexts) {
    if (!contexts || s->primed || s->failed || contexts[0] || contexts[1]) return 0;
    for (uint32_t i=0;i<s->count;i++) if (contexts[i]>1) return 0;
    s->contexts=contexts; return 1;
}
static inline int input_update_poll(InputUpdateClock *s, uint64_t frame, int open, uint16_t *word) {
    *word=0xffff;
    if (s->failed) return 0;
    if (!s->primed || s->accepted == s->count) return 1;
    /* A prediction may be wrong without changing the packet: equal adjacent
     * held/neutral samples require no timing decision. Fail only when the
     * packet already in flight would differ from the required sample. */
    if ((s->predicted && s->queued_word != s->words[s->accepted]) ||
        frame <= s->last_poll) { s->failed=1; return 0; }
    s->last_poll=frame; s->predicted=!!open;
    uint32_t next=s->accepted + (open ? 1u : 0u);
    if (next < s->count) *word=s->words[next];
    s->queued_word=*word;
    return 1;
}
/* Modes 2/3 are a declared pair, never a prediction heuristic. Only a
 * neutral normal sample before a protected normal press can arm the hold. */
static inline int input_update_poll_mode(InputUpdateClock *s,uint64_t frame,int mode,uint16_t *word) {
    if(mode==2) {
        uint16_t raw_difference=(uint16_t)((s->source_raw_hold^s->native_raw_hold)|
                                           (s->source_raw_press^s->native_raw_press));
        if(s->neutral_refresh_stage || !s->primed || !s->compatible_contexts ||
           s->accepted+1>=s->count || s->words[s->accepted]!=0xffff ||
           s->contexts[s->accepted] || s->contexts[s->accepted+1] ||
           !((uint16_t)~s->words[s->accepted+1]&s->protected_raw_mask) ||
           (raw_difference & s->protected_raw_mask) ||
           (raw_difference && (s->last_source_context!=0 || s->last_native_context!=1))) {s->failed=1;return 0;}
        /* The pending ordinary neutral can release a preceding held sample
         * later in this frame. A just-accepted compatible auxiliary may leave
         * unprotected raw fields different; the mandatory ordinary release
         * reconciles those fields before the all-zero refresh checks. Other
         * unequal predecessors and any protected difference still reject. */
        if(!input_update_poll(s,frame,0,word))return 0;
        s->neutral_refresh_stage=1;s->neutral_hold_frame=frame;return 1;
    }
    if(mode==3) {
        if(s->neutral_refresh_stage!=2 || frame!=s->neutral_hold_frame+1) {s->failed=1;return 0;}
        if(!input_update_poll(s,frame,0,word))return 0;
        s->neutral_refresh_stage=3;return 1;
    }
    if(s->neutral_refresh_stage || mode<0 || mode>1) {s->failed=1;return 0;}
    return input_update_poll(s,frame,mode,word);
}
static inline int input_update_neutral_refresh(InputUpdateClock *s,uint64_t frame,uint16_t word,
                                               uint32_t old_held,uint32_t new_held,uint32_t pressed) {
    if(s->neutral_refresh_stage!=3 || frame!=s->last_poll || word!=0xffff ||
       old_held || new_held || pressed || s->source_raw_hold!=0xffff ||
       s->native_raw_hold!=0xffff || s->source_raw_press || s->native_raw_press) {
        s->failed=1;return 0;
    }
    s->neutral_refresh_stage=0;++s->neutral_refreshes;return 1;
}
static inline int input_update_accept(InputUpdateClock *s, uint64_t frame, uint16_t word) {
    if (s->failed || s->accepted == s->count || s->neutral_refresh_stage>=2) return 0;
    if (!s->primed) {
        if (word != 0xffff) { s->failed=1; return 0; }
        s->primed=1; s->last_poll=frame;
    } else if (frame != s->last_poll || frame == s->last_accept ||
               s->queued_word != (s->neutral_refresh_stage==1 ? 0xffff :
                                  s->accepted + 1 < s->count ? s->words[s->accepted + 1] : 0xffff)) {
        s->failed=1; return 0;
    }
    if (word != s->words[s->accepted]) { s->failed=1; return 0; }
    if(s->neutral_refresh_stage==1) {
        if(word!=0xffff || frame!=s->neutral_hold_frame) {s->failed=1;return 0;}
        s->neutral_refresh_stage=2;
    }
    ++s->accepted; s->predicted=0; s->last_accept=frame; return 1;
}
static inline int input_update_auxiliary(InputUpdateClock *s, uint16_t word, uint32_t held) {
    if (s->failed || word != 0xffff || held != 0) { s->failed=1; return 0; }
    return 1; /* Declared neutral refresh; never advances the normal-update clock. */
}
static inline int input_update_context_accept(InputUpdateClock *s, uint64_t frame,
                                              uint16_t word, uint8_t context) {
    if (!s->contexts || s->accepted==s->count || context>1 ||
        (!s->compatible_contexts && context!=s->contexts[s->accepted])) {
        s->context_failed=s->failed=1; return 0;
    }
    if(s->neutral_refresh_stage==1 && context) {s->failed=1;return 0;}
    uint8_t source=s->contexts[s->accepted];
    uint16_t sh=source ? s->source_raw_hold : word;
    uint16_t sp=source ? s->source_raw_press : (uint16_t)(s->source_raw_hold & ~word);
    uint16_t nh=context ? s->native_raw_hold : word;
    uint16_t np=context ? s->native_raw_press : (uint16_t)(s->native_raw_hold & ~word);
    if (s->compatible_contexts && (((sh^nh)|(sp^np)) & s->protected_raw_mask)) {
        s->context_failed=s->failed=1; return 0;
    }
    if (!input_update_accept(s,frame,word)) return 0;
    s->source_raw_hold=sh;s->source_raw_press=sp;
    s->native_raw_hold=nh;s->native_raw_press=np;
    s->last_source_context=source;s->last_native_context=context;
    return 1;
}
#endif
