#include "input_update_clock.h"
#include <assert.h>
#include <stdio.h>
int main(void) {
    const uint16_t w[]={0xffff,0xffff,0xbfdf,0xbfdf,0xffff,0xfff7};
    InputUpdateClock s; uint16_t v;
    assert(input_update_init(&s,w,6));
    assert(input_update_predict(&s,1,0,0));
    assert(!input_update_predict(&s,1,1,1)); /* No invented initial acceptance. */
    assert(input_update_poll(&s,1,1,&v)&&v==0xffff); /* unprimed */
    assert(input_update_accept(&s,2,0xffff));
    assert(!input_update_predict(&s,3,1,0)); /* Default remains gate-only. */
    assert(input_update_predict(&s,3,1,1));
    assert(!input_update_predict(&s,2,1,1)); /* Same-frame calls are not continuation. */
    assert(!input_update_predict(&s,4,1,1)); /* Stalled frame does not extend prediction. */
    assert(!input_update_predict(&s,3,2,1)); /* Closed multi-frame gate. */
    assert(input_update_auxiliary(&s,0xffff,0)&&s.accepted==1);
    assert(input_update_poll(&s,3,1,&v)&&v==0xbfdf);
    assert(input_update_auxiliary(&s,0xffff,0)&&s.accepted==1&&s.predicted);
    assert(input_update_accept(&s,3,0xffff)); /* consecutive neutral */
    assert(input_update_auxiliary(&s,0xffff,0)&&s.accepted==2&&!s.predicted);
    assert(input_update_poll(&s,4,0,&v)&&v==0xbfdf);
    assert(input_update_poll(&s,5,0,&v)&&v==0xbfdf); /* closed gate holds */
    assert(input_update_poll(&s,6,1,&v)&&v==0xbfdf);
    assert(input_update_accept(&s,6,0xbfdf));
    assert(input_update_poll(&s,7,1,&v)&&v==0xffff);
    assert(input_update_accept(&s,7,0xbfdf)); /* held duration preserved */
    assert(input_update_poll(&s,8,1,&v)&&v==0xfff7);
    assert(input_update_accept(&s,8,0xffff));
    assert(input_update_poll(&s,9,1,&v)&&v==0xffff);
    assert(input_update_accept(&s,9,0xfff7)&&s.accepted==6); /* drain */
    assert(input_update_poll(&s,10,1,&v)&&v==0xffff);
    assert(input_update_init(&s,w,6)); assert(input_update_accept(&s,1,0xffff));
    assert(input_update_poll(&s,2,input_update_predict(&s,2,1,1),&v));
    assert(!input_update_poll(&s,3,input_update_predict(&s,3,1,1),&v)); /* Wrong prediction still fails. */
    assert(input_update_init(&s,w,6)); assert(input_update_accept(&s,1,0xffff));
    assert(input_update_poll(&s,2,0,&v)); assert(!input_update_accept(&s,2,0xffff)); /* unexpected acceptance */
    assert(input_update_init(&s,w,6)); assert(input_update_accept(&s,1,0xffff));
    assert(input_update_poll(&s,2,1,&v)); assert(!input_update_accept(&s,2,0xfff7)); /* wrong pipeline packet */
    assert(input_update_init(&s,w,6)); assert(input_update_accept(&s,1,0xffff));
    assert(input_update_poll(&s,2,0,&v)); assert(!input_update_poll(&s,2,0,&v)); /* duplicate poll */
    assert(!input_update_init(&s,w+1,5));
    assert(input_update_init(&s,w,6)); assert(!input_update_auxiliary(&s,0xfff7,0));
    assert(input_update_init(&s,w,6)); assert(!input_update_auxiliary(&s,0xffff,1));
    /* Prediction differences are harmless only while the physical packets
     * remain equal. Count actual acceptances, never predicted ones. */
    const uint16_t repeated[]={0xffff,0xffff,0xffff,0xbfdf,0xbfdf,0xbfdf,0xffff,0xffff};
    assert(input_update_init(&s,repeated,8)); assert(input_update_accept(&s,1,0xffff));
    assert(input_update_poll(&s,2,0,&v)&&v==0xffff);
    assert(input_update_accept(&s,2,0xffff)&&s.accepted==2); /* late opening */
    assert(!input_update_accept(&s,2,0xffff)); /* no duplicate commit */
    assert(input_update_init(&s,repeated,8)); assert(input_update_accept(&s,1,0xffff));
    assert(input_update_poll(&s,2,1,&v)&&v==0xffff);
    assert(input_update_poll(&s,3,1,&v)&&v==0xffff&&s.accepted==1); /* no acceptance */
    assert(input_update_accept(&s,3,0xffff));
    assert(input_update_poll(&s,4,1,&v)&&v==0xbfdf);
    assert(input_update_accept(&s,4,0xffff));
    assert(input_update_poll(&s,5,0,&v)&&v==0xbfdf);
    assert(input_update_accept(&s,5,0xbfdf)); /* held late opening */
    assert(input_update_poll(&s,6,1,&v)&&v==0xbfdf);
    assert(input_update_poll(&s,7,0,&v)&&v==0xbfdf&&s.accepted==4);
    assert(input_update_accept(&s,7,0xbfdf));
    assert(input_update_poll(&s,8,0,&v)&&v==0xbfdf);
    assert(!input_update_accept(&s,8,0xbfdf)); /* release must not be delayed */
    /* Source-context policy consumes the actual auxiliary refresh once. The
     * next ordinary press must not be delayed by a second neutral update. */
    const uint16_t cw[]={0xffff,0xffff,0xffdf,0xffff,0xffdf,0x5fdf,0xffff,0xffff};
    const uint8_t cx[]={0,0,0,1,0,0,0,0};
    assert(input_update_init(&s,cw,8)); assert(input_update_set_contexts(&s,cx));
    assert(input_update_context_accept(&s,1,cw[0],0));
    for (unsigned i=1;i<8;i++) {
        assert(input_update_poll(&s,i+1,1,&v));
        assert(v==(i+1<8 ? cw[i+1] : 0xffff));
        assert(input_update_context_accept(&s,i+1,cw[i],cx[i]));
    }
    assert(s.accepted==8);
    assert(input_update_init(&s,cw,8)); assert(input_update_set_contexts(&s,cx));
    assert(!input_update_context_accept(&s,1,cw[0],1)); /* extra auxiliary */
    assert(input_update_init(&s,cw,8)); assert(input_update_set_contexts(&s,cx));
    assert(input_update_context_accept(&s,1,cw[0],0));
    assert(input_update_poll(&s,2,1,&v)); assert(input_update_context_accept(&s,2,cw[1],0));
    assert(input_update_poll(&s,3,1,&v)); assert(input_update_context_accept(&s,3,cw[2],0));
    assert(input_update_poll(&s,4,1,&v));
    assert(!input_update_context_accept(&s,4,cw[3],0)); /* ordinary cannot substitute */
    assert(input_update_init(&s,cw,8));
    const uint8_t bad[]={0,0,0,2,0,0,0,0};
    assert(!input_update_set_contexts(&s,bad));
    const uint8_t alternate[]={0,0,0,1,0,1,0,0};
    assert(input_update_init(&s,cw,8)); assert(input_update_set_contexts(&s,cx));
    s.compatible_contexts=1;s.protected_raw_mask=8;
    assert(input_update_context_accept(&s,1,cw[0],0));
    for (unsigned i=1;i<8;i++) {
        assert(input_update_poll(&s,i+1,1,&v));
        assert(input_update_context_accept(&s,i+1,cw[i],alternate[i]));
    }
    assert(s.accepted==8); /* Attack through auxiliary, protected raw bit unchanged. */
    const uint16_t start_words[]={0xffff,0xffff,0xfff7,0xffff};
    const uint8_t normal[]={0,0,0,0};
    assert(input_update_init(&s,start_words,4)); assert(input_update_set_contexts(&s,normal));
    s.compatible_contexts=1;s.protected_raw_mask=8;
    assert(input_update_context_accept(&s,1,start_words[0],0));
    assert(input_update_poll(&s,2,1,&v));assert(input_update_context_accept(&s,2,start_words[1],0));
    assert(input_update_poll(&s,3,1,&v));
    assert(!input_update_context_accept(&s,3,start_words[2],1));
    assert(s.context_failed && s.accepted==2); /* A lost Start press cannot be accepted. */
    assert(input_update_init(&s,start_words,4)); assert(input_update_set_contexts(&s,normal));
    s.compatible_contexts=1;s.protected_raw_mask=8;
    assert(input_update_context_accept(&s,1,start_words[0],0));
    assert(input_update_poll(&s,2,1,&v));assert(input_update_context_accept(&s,2,start_words[1],0));
    assert(input_update_poll(&s,3,1,&v));assert(input_update_context_accept(&s,3,start_words[2],0));
    assert(input_update_poll(&s,4,1,&v));
    assert(!input_update_context_accept(&s,4,start_words[3],1)); /* Lost release/retained edge. */
    for(int fault=0;fault<10;fault++) {
        assert(input_update_init(&s,start_words,4));assert(input_update_set_contexts(&s,normal));
        s.compatible_contexts=1;s.protected_raw_mask=8;
        assert(input_update_context_accept(&s,1,0xffff,0));
        assert(input_update_poll_mode(&s,2,2,&v)&&v==0xffff);
        if(fault==1) {assert(!input_update_context_accept(&s,2,0xffff,1));continue;}
        assert(input_update_context_accept(&s,2,0xffff,0)&&s.accepted==2);
        if(fault==2) {assert(!input_update_poll_mode(&s,3,1,&v));continue;}
        assert(input_update_poll_mode(&s,3,3,&v)&&v==0xfff7);
        if(fault==3) {assert(!input_update_context_accept(&s,3,0xffff,0));continue;}
        if(fault==4) {assert(!input_update_poll_mode(&s,4,1,&v));continue;}
        if(fault==5) {assert(!input_update_neutral_refresh(&s,3,0xfff7,0,0,0));continue;}
        if(fault==6) {assert(!input_update_neutral_refresh(&s,3,0xffff,1,0,0));continue;}
        if(fault==7) {assert(!input_update_neutral_refresh(&s,3,0xffff,0,0,1));continue;}
        if(fault==8) {assert(!input_update_neutral_refresh(&s,3,0xffff,0,1,0));continue;}
        if(fault==9) {s.native_raw_press=8;assert(!input_update_neutral_refresh(&s,3,0xffff,0,0,0));continue;}
        assert(input_update_neutral_refresh(&s,3,0xffff,0,0,0)&&s.accepted==2&&s.neutral_refreshes==1);
        assert(input_update_poll_mode(&s,4,1,&v)&&v==0xffff);
        assert(input_update_context_accept(&s,4,0xfff7,0));
        assert(input_update_poll_mode(&s,5,1,&v)&&v==0xffff);
        assert(input_update_context_accept(&s,5,0xffff,0)&&s.accepted==4);
        assert(!input_update_neutral_refresh(&s,5,0xffff,0,0,0)); /* extra refresh */
    }
    /* An ordinary neutral release is accepted after the hold poll. Its
     * predecessor can still be held at that poll; no extra source event
     * may be consumed by the following separately checked refresh. */
    for (unsigned previous=0;previous<2;previous++) {
        const uint16_t release_words[]={0xffff,0xffff,previous ? 0xfff7 : 0x5fdf,0xffff,0xfff7,0xffff};
        const uint8_t release_contexts[]={0,0,0,0,0,0};
        assert(input_update_init(&s,release_words,6));
        assert(input_update_set_contexts(&s,release_contexts));
        s.compatible_contexts=1;s.protected_raw_mask=8;
        assert(input_update_context_accept(&s,1,release_words[0],0));
        assert(input_update_poll_mode(&s,2,1,&v));
        assert(input_update_context_accept(&s,2,release_words[1],0));
        assert(input_update_poll_mode(&s,3,1,&v)&&v==0xffff);
        assert(input_update_context_accept(&s,3,release_words[2],0));
        assert(s.accepted==3 && s.source_raw_hold==release_words[2] && s.native_raw_hold==release_words[2]);
        InputUpdateClock bad=s;
        bad.native_raw_hold^=1;
        assert(!input_update_poll_mode(&bad,4,2,&v) && bad.accepted==3);
        bad=s;bad.native_raw_press^=1;
        assert(!input_update_poll_mode(&bad,4,2,&v) && bad.accepted==3);
        assert(input_update_poll_mode(&s,4,2,&v)&&v==0xffff);
        assert(input_update_context_accept(&s,4,0xffff,0)&&s.accepted==4);
        assert(s.source_raw_hold==0xffff && s.native_raw_hold==0xffff && !s.source_raw_press && !s.native_raw_press);
        assert(input_update_poll_mode(&s,5,3,&v)&&v==0xfff7);
        InputUpdateClock extra=s;
        assert(!input_update_context_accept(&extra,5,0xffff,0)); /* duplicate source acceptance */
        extra=s;assert(!input_update_neutral_refresh(&extra,5,release_words[2],0,0,0));
        assert(input_update_neutral_refresh(&s,5,0xffff,0,0,0));
        assert(s.accepted==4 && s.neutral_refreshes==1);
        assert(input_update_poll_mode(&s,6,1,&v)&&v==0xffff);
        assert(input_update_context_accept(&s,6,0xfff7,0));
        assert(s.source_raw_press==8 && s.native_raw_press==8); /* one fresh Start */
        assert(input_update_poll_mode(&s,7,1,&v)&&v==0xffff);
        assert(input_update_context_accept(&s,7,0xffff,0));
        assert(s.accepted==6 && !s.source_raw_press && !s.native_raw_press);
        assert(!input_update_context_accept(&s,8,0xffff,0));
    }
    /* A source-normal attack can be accepted through the declared compatible
     * auxiliary context. The following normal release reconciles its raw
     * fields before a separately verified zero-effect refresh. */
    const uint16_t aux_release[]={0xffff,0xffff,0xffdf,0xffff,0xffdf,0x5fdf,0xffff,0xfff7,0xffff};
    const uint8_t aux_source[]={0,0,0,0,0,0,0,0,0};
    const uint8_t aux_native[]={0,0,0,1,0,1,0,0,0};
    assert(input_update_init(&s,aux_release,9));assert(input_update_set_contexts(&s,aux_source));
    s.compatible_contexts=1;s.protected_raw_mask=8;
    assert(input_update_context_accept(&s,1,aux_release[0],0));
    for(unsigned i=1;i<6;i++) {
        assert(input_update_poll_mode(&s,i+1,1,&v));
        assert(input_update_context_accept(&s,i+1,aux_release[i],aux_native[i]));
    }
    assert(s.accepted==6 && s.source_raw_hold==0x5fdf && s.source_raw_press==0xa000);
    assert(s.native_raw_hold==0xffdf && s.native_raw_press==0);
    InputUpdateClock protected_bad=s;protected_bad.native_raw_hold^=8;
    assert(!input_update_poll_mode(&protected_bad,7,2,&v));
    protected_bad=s;protected_bad.native_raw_press^=8;
    assert(!input_update_poll_mode(&protected_bad,7,2,&v));
    assert(input_update_poll_mode(&s,7,2,&v)&&v==0xffff);
    InputUpdateClock wrong=s;
    assert(!input_update_context_accept(&wrong,7,0xffff,1)); /* release cannot be auxiliary */
    wrong=s;assert(!input_update_context_accept(&wrong,7,0xffdf,0)); /* changed release */
    assert(input_update_context_accept(&s,7,0xffff,0)&&s.accepted==7);
    assert(s.source_raw_hold==0xffff && s.native_raw_hold==0xffff && !s.source_raw_press && !s.native_raw_press);
    assert(input_update_poll_mode(&s,8,3,&v)&&v==0xfff7);
    assert(input_update_neutral_refresh(&s,8,0xffff,0,0,0)&&s.accepted==7&&s.neutral_refreshes==1);
    assert(input_update_poll_mode(&s,9,1,&v)&&v==0xffff);
    assert(input_update_context_accept(&s,9,0xfff7,0)&&s.source_raw_press==8&&s.native_raw_press==8);
    assert(input_update_poll_mode(&s,10,1,&v)&&v==0xffff);
    assert(input_update_context_accept(&s,10,0xffff,0)&&s.accepted==9);
    puts("input update clock: priming, neutrals, held duration, stalls, reopening, draining and fail-stops PASS");
}
