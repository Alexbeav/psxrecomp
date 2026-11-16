/* Included privately by debug_server.c after the ordinary route fields.
 * Test-supplied configuration only; no game addresses are compiled in. */
#include "input_update_clock.h"
#include "psx_sha256.h"
static InputUpdateClock s_update;
static uint32_t s_update_config[12];
static int s_update_enabled;
static int s_update_previous_accept;
static uint32_t s_refresh_guard[4]; /* pressed RAM/PC, raw-held RAM, raw-pressed RAM */
static int s_refresh_guard_enabled,s_refresh_wait_pressed;
static FILE *s_update_log;
static uint64_t s_update_last_accept;
static psx_sha256_ctx s_update_hash;
static uint32_t s_update_decision_frames[256],s_update_decision_open[256],s_update_decision_count;
static uint16_t update_peek16(uint32_t a) {
    return (uint16_t)(g_psx_ram[a] | ((uint16_t)g_psx_ram[a+1]<<8));
}
static void update_fail(const char *reason) {
    if (s_update_log) {
        fprintf(s_update_log,"{\"kind\":\"failure\",\"frame\":%llu,\"accepted\":%u,\"reason\":\"%s\"}\n",
                (unsigned long long)s_frame_count,s_update.accepted,reason);
        fflush(s_update_log);
        /* Preserve the existing bounded recorder on fail-stop as well as EOF.
         * This observes only test-configured fields and never resumes execution. */
        const uint32_t watched[] = {s_update_config[0],s_update_config[3],
                                   s_update_config[4],s_update_config[5]};
        FILE *access = input_route_observer_output("update-failure-access.jsonl");
        if (access) {
            debug_server_dump_watched_writes(access,watched,4);
            fclose(access);
        }
    }
    fprintf(stderr,"accepted-update replay failed: %s\n",reason); exit(4);
}
/* marker RAM, marker PC, marker RA, gate RAM(u32), packet RAM(u16),
 * header RAM(u16), expected header, total VBlank watchdog, stalled watchdog. */
static int update_configure(const InputRouteStep *steps,uint32_t count,uint32_t total) {
    const char *p=getenv("PSX_INPUT_UPDATE_CLOCK");
    if (!p) return 1;
    for (unsigned i=0;i<12;i++) {
        char *end; if (*p<'0'||*p>'9') return 0;
        unsigned long long v=strtoull(p,&end,0);
        if (v>0xffffffffull || end==p || (i==11 ? *end!=0 : *end!=',')) return 0;
        s_update_config[i]=(uint32_t)v; p=end+(i<11);
    }
    const unsigned ram_fields[]={0,3,4,5};
    for (unsigned i=0;i<4;i++) {
        uint32_t a=s_update_config[ram_fields[i]];
        if (a>0x1ffffcu || (a&1u)) return 0;
    }
    if ((s_update_config[1]&3)||(s_update_config[2]&3)||
        (s_update_config[3]&3)||s_update_config[6]>0xffffu||
        !s_update_config[7]||s_update_config[7]>60000||
        !s_update_config[8]||s_update_config[8]>s_update_config[7]||s_update_config[9]>31) return 0;
    uint16_t *words=malloc(total*sizeof(*words)); if (!words) return 0;
    uint32_t n=0;
    for (uint32_t i=0;i<count;i++) for(uint32_t j=0;j<steps[i].frames;j++) words[n++]=steps[i].buttons;
    if (!input_update_init(&s_update,words,total)) {free(words);return 0;}
    const char *contexts_path=getenv("PSX_INPUT_UPDATE_CONTEXTS");
    if (contexts_path) {
        FILE *f=fopen(contexts_path,"rb"); uint8_t header[12];
        if (!f) return 0;
        if (fread(header,1,12,f)!=12 || memcmp(header,"PSXCTX1\0",8)) {fclose(f);return 0;}
        uint32_t size=(uint32_t)header[8]|((uint32_t)header[9]<<8)|
                      ((uint32_t)header[10]<<16)|((uint32_t)header[11]<<24);
        uint8_t *contexts=malloc(total);
        if (!contexts || size!=total || fread(contexts,1,total,f)!=total ||
            fgetc(f)!=EOF || ferror(f) || !input_update_set_contexts(&s_update,contexts)) {
            free(contexts);fclose(f);return 0;
        }
        if (fclose(f)) return 0;
    }
    const char *protected_mask=getenv("PSX_INPUT_UPDATE_PROTECTED_RAW_MASK");
    if (protected_mask) {
        char *end; unsigned long v=strtoul(protected_mask,&end,0);
        if (!s_update.contexts || end==protected_mask || *end || v==0 || v>0xffff) return 0;
        s_update.compatible_contexts=1;s_update.protected_raw_mask=(uint16_t)v;
    }
    const char *predictor=getenv("PSX_INPUT_UPDATE_PREDICTOR");
    if (predictor) {
        if (strcmp(predictor,"previous-accept")) return 0;
        s_update_previous_accept=1;
    }
    const char *guard=getenv("PSX_INPUT_UPDATE_REFRESH_GUARD");
    if(guard) {
        if(!s_update.compatible_contexts)return 0;
        for(unsigned i=0;i<4;i++) {
            char *end;unsigned long v=strtoul(guard,&end,0);
            if(end==guard || (i==3?*end!=0:*end!=',') || v>0xfffffffful ||
               (i==1 ? (v&3u)!=0 : (v>0x1ffffeu || (v&1u))))return 0;
            s_refresh_guard[i]=(uint32_t)v;guard=end+(i<3);
        }
        s_refresh_guard_enabled=1;
    }
    const char *decisions=getenv("PSX_INPUT_UPDATE_DECISIONS");
    if (decisions) {
        FILE *f=fopen(decisions,"rb"); char line[128]; uint32_t previous=0;
        if (!f) return 0;
        if (!fgets(line,sizeof(line),f)) {fclose(f);return 0;}
        int version2=!strcmp(line,"PSX-UPDATE-DECISIONS2\n");
        if(!version2 && strcmp(line,"PSX-UPDATE-DECISIONS1\n")) {fclose(f);return 0;}
        while (fgets(line,sizeof(line),f)) {
            unsigned frame,open; char extra;
            if (sscanf(line,"%u %u %c",&frame,&open,&extra)!=2 || frame<=previous ||
                frame>s_update_config[7] || open>(version2?3u:1u) || s_update_decision_count==256) {fclose(f);return 0;}
            previous=frame;
            s_update_decision_frames[s_update_decision_count]=frame;
            s_update_decision_open[s_update_decision_count++]=open;
        }
        if(ferror(f)){fclose(f);return 0;} if(fclose(f))return 0;
    }
    for(uint32_t i=0;i<s_update_decision_count;i++) {
        uint32_t mode=s_update_decision_open[i];
        if(mode>=2 && !s_refresh_guard_enabled)return 0;
        if(mode==2 && (i+1==s_update_decision_count || s_update_decision_open[i+1]!=3 ||
           s_update_decision_frames[i+1]!=s_update_decision_frames[i]+1))return 0;
        if(mode==3 && (!i || s_update_decision_open[i-1]!=2 ||
           s_update_decision_frames[i]!=s_update_decision_frames[i-1]+1))return 0;
    }
    s_update_enabled=1; psx_sha256_init(&s_update_hash); return 1;
}
uint16_t debug_server_update_poll(int slot,uint16_t buttons,int analog) {
    if (!s_update_enabled || slot!=0) return buttons;
    if (analog) update_fail("non-digital P1 poll");
    uint32_t a=s_update_config[3];
    uint32_t gate=update_peek16(a)|((uint32_t)update_peek16(a+2)<<16);
    uint16_t word;
    int open=input_update_predict(&s_update,s_frame_count,gate,s_update_previous_accept),override=0;
    for(uint32_t i=0;i<s_update_decision_count;i++) if(s_update_decision_frames[i]==s_frame_count) {
        open=(int)s_update_decision_open[i];override=1;break;
    }
    if(s_refresh_wait_pressed)update_fail("neutral refresh missing pressed store");
    if (!input_update_poll_mode(&s_update,s_frame_count,open,&word)) {
        if (s_update.predicted) {
            fprintf(s_update_log,"{\"kind\":\"decision_failure\",\"decision_frame\":%llu,\"observed_open\":0,\"accepted\":%u}\n",
                    (unsigned long long)s_update.last_poll,s_update.accepted); fflush(s_update_log);
        }
        update_fail("poll/acceptance phase mismatch");
    }
    fprintf(s_update_log,"{\"kind\":\"poll\",\"frame\":%llu,\"accepted\":%u,\"gate\":%u,\"word\":%u}\n",
            (unsigned long long)s_frame_count,s_update.accepted,gate,word);
    if(override)fprintf(s_update_log,"{\"kind\":\"measured_decision\",\"frame\":%llu,\"open\":%d}\n",(unsigned long long)s_frame_count,open);
    if (fflush(s_update_log)) update_fail("log flush");
    return word;
}
static void update_accept_write(uint32_t phys,uint8_t width,uint32_t pc,uint32_t ra,uint32_t old_val,uint32_t new_val) {
    if(s_update_enabled && s_refresh_wait_pressed && phys==s_refresh_guard[0]) {
        if(width!=2 || pc!=s_refresh_guard[1] || ra!=s_update_config[2] || old_val || new_val ||
           update_peek16(s_update_config[0]) || update_peek16(s_refresh_guard[2]) || update_peek16(s_refresh_guard[3]))
            update_fail("neutral refresh changed pressed/raw state");
        s_refresh_wait_pressed=0;
        fprintf(s_update_log,"{\"kind\":\"neutral_refresh\",\"frame\":%llu,\"accepted\":%u,\"word\":65535,\"held_before\":0,\"held_after\":0,\"pressed_before\":0,\"pressed_after\":0,\"raw_held\":0,\"raw_pressed\":0}\n",
                (unsigned long long)s_frame_count,s_update.accepted);fflush(s_update_log);return;
    }
    if (!s_update_enabled || s_update.accepted==s_update.count || phys!=s_update_config[0] ||
        pc!=s_update_config[1] || ra!=s_update_config[2] || width!=2) return;
    uint16_t header=update_peek16(s_update_config[5]);
    if (header!=s_update_config[6]) {
        if (s_update.primed) update_fail("accepted packet header changed");
        return;
    }
    uint16_t word=update_peek16(s_update_config[4]);
    uint32_t selector=debug_cpu_ptr ? debug_cpu_ptr->gpr[s_update_config[9]] : UINT32_MAX;
    if (selector!=s_update_config[10]) {
        fprintf(s_update_log,"{\"kind\":\"auxiliary_store\",\"frame\":%llu,\"word\":%u,\"old_held\":%u,\"new_held\":%u,\"selector\":%u}\n",
                (unsigned long long)s_frame_count,word,old_val,new_val,selector);
        fflush(s_update_log);
        if (selector!=s_update_config[11]) update_fail("unexpected auxiliary context/input");
        if (!s_update.contexts) {
            if (!input_update_auxiliary(&s_update,word,new_val)) update_fail("unexpected auxiliary context/input");
            return;
        }
    }
    if(s_update.neutral_refresh_stage==3) {
        if(!s_refresh_guard_enabled || s_refresh_wait_pressed || selector!=s_update_config[11] ||
           update_peek16(s_refresh_guard[2]) || update_peek16(s_refresh_guard[3]) ||
           !input_update_neutral_refresh(&s_update,s_frame_count,word,old_val,new_val,update_peek16(s_refresh_guard[0])))
            update_fail("declared neutral refresh mismatch");
        s_refresh_wait_pressed=1;return;
    }
    if(s_refresh_wait_pressed)update_fail("unexpected acceptance before refresh pressed store");
    uint8_t context=selector==s_update_config[10] ? 0u : 1u;
    if (s_update.contexts && !s_update.compatible_contexts && s_update.contexts[s_update.accepted]!=context)
        update_fail("source event context mismatch");
    if (!(s_update.contexts ? input_update_context_accept(&s_update,s_frame_count,word,context) :
                              input_update_accept(&s_update,s_frame_count,word))) {
        if (s_update.primed && !s_update.context_failed && !s_update.predicted && s_update.last_poll==s_frame_count && word==s_update.words[s_update.accepted]) {
            fprintf(s_update_log,"{\"kind\":\"decision_failure\",\"decision_frame\":%llu,\"observed_open\":1,\"accepted\":%u}\n",
                    (unsigned long long)s_frame_count,s_update.accepted);fflush(s_update_log);
        }
        if(s_refresh_guard_enabled && s_update.context_failed && context==1 &&
           s_update.accepted && s_update.last_accept+1==s_frame_count &&
           s_update.words[s_update.accepted-1]==0xffff && !s_update.contexts[s_update.accepted] &&
           ((uint16_t)~word&s_update.protected_raw_mask) && !old_val &&
           !update_peek16(s_refresh_guard[0]) && !update_peek16(s_refresh_guard[2]) && !update_peek16(s_refresh_guard[3])) {
            fprintf(s_update_log,"{\"kind\":\"neutral_refresh_candidate\",\"hold_frame\":%llu,\"refresh_frame\":%llu,\"accepted\":%u}\n",
                (unsigned long long)s_update.last_accept,(unsigned long long)s_frame_count,s_update.accepted);fflush(s_update_log);
        }
        update_fail(s_update.context_failed ? "protected raw input effect mismatch" : "accepted packet/prediction mismatch");
    }
    uint8_t b[2]={(uint8_t)word,(uint8_t)(word>>8)};
    psx_sha256_update(&s_update_hash,b,2); s_update_last_accept=s_frame_count;
    fprintf(s_update_log,"{\"kind\":\"accept\",\"frame\":%llu,\"sequence\":%u,\"word\":%u}\n",
            (unsigned long long)s_frame_count,s_update.accepted,word);
    if (s_update.contexts)
        fprintf(s_update_log,"{\"kind\":\"accepted_context\",\"frame\":%llu,\"sequence\":%u,\"context\":%u}\n",
                (unsigned long long)s_frame_count,s_update.accepted,context);
    if (fflush(s_update_log)) update_fail("log flush");
    if (s_update.accepted==s_update.count) {
        uint8_t hash[32]; char hex[65]; psx_sha256_final(&s_update_hash,hash);
        for(unsigned i=0;i<32;i++) sprintf(hex+2*i,"%02x",hash[i]);
        FILE *f=input_route_observer_output("update-input-end.json");
        fprintf(f,"{\"clock\":\"accepted-update-experimental\",\"frame\":%u,\"accepted_samples\":%u,\"accepted_words_sha256\":\"%s\"}\n",
                s_input_route_consumed,s_update.accepted,hex);
        if(fclose(f)) update_fail("end receipt close");
        input_route_observer_set_end(s_input_route_consumed);
    }
}
static void update_boundary(void) {
    if (!s_update_enabled || s_update.accepted==s_update.count) return;
    if (s_frame_count>s_update_config[7] ||
        s_frame_count-s_update_last_accept>s_update_config[8]) update_fail("accepted-update watchdog");
}
