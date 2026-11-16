/* An explicit SeekL completes one sector period after its due cycle. At that
 * moment the reported position sits nine sectors behind the target (never
 * before the first addressable sector, -150), the mechanism's idle head
 * starts one sector after that and walks to the target, and the SEEK, READ
 * and PLAY status bits all drop. The lead is what a caller observes: GetlocL
 * straight after the second response reports the start of that window, not
 * the target itself. */
static void source_drive_complete_explicit_seek(void)
{
    if (!s_nymashock_drive || !pending.pending || pending.cmd != 0x16 ||
        psx_cycle_count < pending.due_cyc || source_drive_head_valid)
        return;
    int target = msf_to_lba(seek_min, seek_sec, seek_sect);
    source_drive_hold_logical = 0;
    source_drive_subq_lba = target - 9;
    if (source_drive_subq_lba < -150) source_drive_subq_lba = -150;
    int position = source_drive_subq_lba + 1;
    if (position >= target) {
        position -= 9;
        if (position < -150) position = -150;
    }
    source_drive_head_start(position, target,
        pending.due_cyc + 2 * source_drive_sector_period());
    stat_reg &= ~(CDSTAT_SEEK | CDSTAT_READ | CDSTAT_PLAY);
    source_drive_head_update();
}

static void source_disc_startup_complete(void)
{
    stat_reg |= CDSTAT_MOTOR;
    clear_sector_buffer();
    last_sector_lba = -1; /* Original HeaderBufValid is cleared on insertion. */
    source_drive_hold_logical = 0;
    source_drive_head_target = source_drive_head_lba;
    source_drive_head_due = UINT64_MAX;
}

/* Optional source-CD clock/device behavior retained from our accepted
 * ac56b624 authored black-box implementation. The behavior contract is the
 * private corpus CDROM-SOURCE-PROFILE-SPEC.md and CD-ENTROPY-CONTRACT.md.
 * Provenance remains in runtime/tests/cd*_provenance*.json and the complete
 * authored synthetic-disc transcripts. This is compatibility behavior with
 * explicit unobserved/unsupported limits, not a hardware timing guarantee.
 * Modern defaults, XA DSP, FIFO, geometry and resident paths stay in cdrom.c.
 */

static uint32_t source_clock_random(uint32_t maximum) {
    uint32_t value;
    if (!cd_tape_bounded(&s_source_clock_tape,maximum,&value)) {
        fprintf(stderr,"[CDROM] Source clock random tape exhausted at word %u\n",s_source_clock_tape.cursor);
        exit(2);
    }
    s_source_clock_calls++;
    return value;
}

static int source_clock_receive_ready(void) {
    if (irq_flag != 0) return 0;
    if (s_source_clock && psx_cycle_count < s_source_ready_due) return 0;
    return 1;
}

static int source_boot_model(const char *name) {
    const char *value=getenv(name);
    if(!value || !*value || strcmp(value,"default")==0) return 0;
    if(strcmp(value,"octoshock-2.2.2")==0) return 1;
    fprintf(stderr,"[CDROM] Unsupported %s=%s\n",name,value);
    exit(2);
}

static int source_seek_lower_bound(int origin, int target, int motor_on, int paused, uint8_t mode)
{
    return psx_cd_seek_delay(origin, target, motor_on, paused, mode, s_nymashock_drive);
}

static void source_drive_head_update(void)
{
    if (!s_nymashock_drive || !source_drive_head_valid) return;
    uint64_t period = (mode_reg & 0x80) ?
        CDROM_SINGLE_SPEED_SECTOR_CYCLES / 2 : CDROM_SINGLE_SPEED_SECTOR_CYCLES;
    int64_t limit = (int64_t)source_drive_head_target + (source_drive_hold_logical ? 1 : -1);
    while (source_drive_head_due <= psx_cycle_count) {
        source_drive_subq_lba = source_drive_head_lba;
        source_drive_head_lba += source_drive_head_lba < limit ? 1 : -8;
        if (source_drive_head_lba < -150) source_drive_head_lba = -150;
        source_drive_head_due += period;
    }
}

static uint64_t source_drive_sector_period(void)
{
    uint64_t single = CDROM_SINGLE_SPEED_SECTOR_CYCLES;
    return (mode_reg & 0x80) ? single / 2 : single;
}

static void source_drive_head_start(int position, int anchor, uint64_t first_advance)
{
    source_drive_head_valid = 1;
    source_drive_head_lba = position;
    source_drive_head_target = anchor;
    source_drive_head_due = first_advance;
}

static int implicit_read_seek_cycles(void)
{
    if (!s_source_clock) {
        if (!setloc_pending) return 0;
        /* The seek travels from where the drive is: the read cursor. A
         * completed SeekL/SeekP leaves it on the seek target without
         * delivering data, reads advance it, reset clears it (PSX-SPX Setloc,
         * SeekL, SeekP, ReadN/ReadS, GetlocP). The last delivered sector
         * would re-charge travel a seek already made. */
        int origin = msf_to_lba(read_min, read_sec, read_sect);
        if (origin < 0) origin = 0;
        return apply_speed(source_seek_lower_bound(origin, s_setloc_lba,
            (stat_reg & CDSTAT_MOTOR) != 0, s_source_seek_paused, mode_reg));
    }

    int origin = msf_to_lba(read_min, read_sec, read_sect);
    if (origin < 0) origin = 0;
    int target = setloc_pending ? s_setloc_lba : origin;
    if (target < 0) target = 0;
    /* A ReadN to another target while reading seeks from the drive's head,
     * two sectors past the next delivered one, as an explicit seek does
     * [ORACLE FIXTURE C4: C4-readn-while-reading-other-s1/s2, first INT1 of
     * the new stream]. */
    if (reading) origin += 2;
    source_drive_head_update();
    if (s_nymashock_drive && source_drive_head_valid) origin = source_drive_head_lba;
    int delay = source_seek_lower_bound(origin, target,
        (stat_reg & CDSTAT_MOTOR) != 0, s_source_seek_paused, mode_reg);
    source_drive_head_valid = 0;
    int64_t total = (int64_t)delay + source_clock_random(25000);
    return total > INT32_MAX ? INT32_MAX : (int)total;
}

static int source_toc_seek_cycles(void)
{
    int origin = last_sector_lba;
    if (s_source_clock && !reading && s_source_seek_paused)
        origin = msf_to_lba(read_min, read_sec, read_sect);
    if (origin < 0) origin = 0;
    if (s_nymashock_drive && source_drive_head_valid)
        origin = source_drive_head_lba;
    int motor = (stat_reg & CDSTAT_MOTOR) != 0;
    int paused = motor && !reading && !(stat_reg & (CDSTAT_READ | CDSTAT_SEEK | CDSTAT_PLAY));
    int delay = source_seek_lower_bound(origin, 0, motor, paused, mode_reg);
    if (!s_source_clock) return delay;
    int64_t total = (int64_t)delay + source_clock_random(25000);
    return total > INT32_MAX ? INT32_MAX : (int)total;
}

static int source_explicit_seek_cycles(uint8_t cmd)
{
    int origin = msf_to_lba(read_min, read_sec, read_sect);
    int target = msf_to_lba(seek_min, seek_sec, seek_sect);
    if (origin < 0) origin = 0;
    if (target < 0) target = 0;
    if (reading) origin += 2;
    else if (s_nymashock_drive && source_drive_head_valid)
        origin = source_drive_head_lba;
    source_drive_head_valid = 0;
    int64_t total = source_seek_lower_bound(origin, target,
        (stat_reg & CDSTAT_MOTOR) != 0, s_source_seek_paused, mode_reg);
    if (cmd == 0x15) {
        int period = (mode_reg & 0x80) ? CDROM_SINGLE_SPEED_SECTOR_CYCLES / 2 :
                                      CDROM_SINGLE_SPEED_SECTOR_CYCLES;
        total += period * (s_nymashock_drive ? 2 : 1);
    }
    if (s_source_clock) total += source_clock_random(25000);
    return total > INT32_MAX ? INT32_MAX : (int)total;
}

static int source_cdda_peek(int32_t lba)
{
    uint8_t subq[12];
    int valid = 0;
    if (!iso_read_cdda_subq(iso_handle, (uint32_t)lba, subq, 12, &valid) ||
        !valid || (subq[0] & 15) != 1)
        return 0;
    for (unsigned i = 0; i < 12; ++i)
        last_valid_subq[i] = subq[i];
    last_valid_subq_available = 1;
    source_cdda.position_valid = 1;
    return 1;
}

static void source_cdda_present(void)
{
    if (!source_cdda.enabled || !source_cdda.async_type) return;
    if (!source_clock_receive_ready()) return;
    response_clear();
    for (unsigned i = 0; i < source_cdda.async_count; ++i)
        response_push(source_cdda.async_data[i]);
    unsigned type = source_cdda.async_type;
    source_cdda.async_type = 0;
    source_cdda.async_count = 0;
    set_irq((int)type);
    fire_cdrom_irq();
}

static void source_cdda_queue(unsigned type, const uint8_t *data, unsigned count)
{
    if (count > sizeof(source_cdda.async_data)) abort();
    source_cdda.async_type = type;
    source_cdda.async_count = count;
    for (unsigned i = 0; i < count; ++i)
        source_cdda.async_data[i] = data[i];
    source_cdda_present();
}

static void start_source_cdda(int requested_track)
{
    if (reading || (mode_reg & 0x80)) exit(2);
    CDROMSourceCDDAPlayDebugState *debug = &s_source_cdda_play_debug;
    memset(debug, 0, sizeof(*debug));
    debug->valid = 1;
    debug->cycle = psx_cycle_count;
    debug->requested_track = (uint8_t)requested_track;
    debug->motor = (uint8_t)((stat_reg & 2) != 0);
    debug->paused = (uint8_t)(s_source_seek_paused != 0);
    debug->mode = mode_reg;
    debug->was_playing = (uint8_t)(cdda_playing != 0);
    debug->was_seeking = (uint8_t)(source_cdda.seeking != 0);
    debug->setloc_pending = (uint8_t)(setloc_pending != 0);
    debug->head_valid = (uint8_t)(source_drive_head_valid != 0);
    debug->head_lba = source_drive_head_lba;
    debug->head_target_lba = source_drive_head_target;
    debug->head_due_cycle = source_drive_head_due;
    debug->random_cursor_before = s_source_clock_tape.cursor;
    debug->random_calls_before = s_source_clock_calls;
    debug->random_cursor_after = s_source_clock_tape.cursor;
    debug->random_calls_after = s_source_clock_calls;
    source_cdda.async_type = 0;
    source_cdda.async_count = 0;
    cdrom_clear_pending_dataready();
    if (cdda_playing && !source_cdda.seeking && !requested_track && !setloc_pending) return;

    /* Any track count (PS1G-118: Tomb Raider has 57). From track 10 on, the
     * binary play_track_match differs from the BCD SubQ track, so an
     * autopause Play of such a track ends at its first sector. */
    int tracks = iso_cdda_track_count(iso_handle);
    if (tracks < 1) exit(2);
    if (requested_track > tracks) requested_track = tracks;
    int origin = cdda_playing ? (cdda_lba > 2147483647U ? 0 : (int)cdda_lba) : msf_to_lba(read_min, read_sec, read_sect);
    if (origin < 0) origin = 0;
    /* A stopped stream's delivery cursor is not the physical standby head. */
    if (s_nymashock_drive && source_drive_head_valid)
        origin = source_drive_head_lba;
    int target = origin;
    if (requested_track) {
        uint32_t position = iso_cdda_track_start_lba(iso_handle, requested_track);
        target = position > 2147483647U ? 0 : (int)position;
    }
    else if (setloc_pending)
        target = s_setloc_lba < 0 ? 0 : s_setloc_lba;
    /* Play starts on a data track too. The drive then plays without sound: it
     * walks, reports and pauses as on an audio track (PS1G-116, Spyro: Year of
     * the Dragon's disc check; cdrom_source_cdda_data_track_fixtures.json). */
    int track = requested_track ? requested_track : cdda_track_for_lba((uint32_t)target);
    if (!track) exit(2);

    int delay = source_seek_lower_bound(origin, target, (stat_reg & 2) != 0,
                                        s_source_seek_paused, mode_reg);
    uint32_t jitter = source_clock_random(25000);
    if ((int64_t)delay + jitter > 2147483647LL) abort();
    cdda_delay = delay + (int)jitter;
    source_drive_head_valid = 0;
    debug->new_seek = 1;
    debug->origin_lba = origin;
    debug->target_lba = target;
    debug->lower_bound_cycles = delay;
    debug->jitter_cycles = jitter;
    debug->delay_cycles = (uint32_t)cdda_delay;
    debug->computed_due_cycle = psx_cycle_count + (uint32_t)cdda_delay;
    debug->random_cursor_after = s_source_clock_tape.cursor;
    debug->random_calls_after = s_source_clock_calls;
    stop_read_stream();
    spu_cd_audio_reset();
    cdda_playing = 1;
    cdda_track = track;
    cdda_lba = (uint32_t)target;
    cdda_data_end_pending = 0;
    source_cdda.seeking = 1;
    source_cdda.sectors_read = 0;
    source_cdda.pipe_count = 0;
    source_cdda.pipe_at = 0;
    source_cdda.report_last_tens = 255;
    source_cdda.play_track_match = requested_track ? requested_track : -1;
    for (int i = 0; i < 32; ++i)
        if (source_cdda_peek(target + i)) break;
    lba_to_msf(target, 150, &read_min, &read_sec, &read_sect);
    setloc_pending = 0;
    s_source_seek_paused = 0;
    stat_reg = (stat_reg & 0x1f) | 0x42;
}

static void process_source_cdda(uint32_t cycles)
{
    source_cdda_present();
    if (!cdda_playing) return;
    cdda_delay -= (int)cycles;
    unsigned serviced = 0;
    while (cdda_delay <= 0) {
        if (++serviced > 64) exit(2);
        if (source_cdda.seeking) {
            source_cdda.seeking = 0;
            for (int back = 1; back <= 16; ++back)
                if (source_cdda_peek((int32_t)cdda_lba - back)) break;
            stat_reg = (stat_reg & ~CDSTAT_SEEK) | CDSTAT_PLAY;
            cdda_delay += CDROM_SINGLE_SPEED_SECTOR_CYCLES;
            continue;
        }

        uint8_t raw[2352];
        if (!iso_read_cdda_sector(iso_handle, cdda_lba, raw, sizeof(raw))) exit(2);
        int fresh = source_cdda_peek((int32_t)cdda_lba);
        if (!last_valid_subq_available) exit(2);
        if (fresh && source_cdda.play_track_match < 0)
            source_cdda.play_track_match = last_valid_subq[1];
        int leadout = last_valid_subq[1] == 0xaa;
        int track_end = (mode_reg & 2) && source_cdda.play_track_match >= 0 &&
                        source_cdda.play_track_match != last_valid_subq[1];
        if (leadout || track_end) {
            uint8_t status = stat_reg;
            cdda_playing = 0;
            cdda_delay = 0;
            s_source_seek_paused = 1;
            source_cdda.pipe_at = 0;
            source_cdda.pipe_count = 0;
            source_cdda.sectors_read = 0;
            stat_reg &= ~CDSTAT_PLAY;
            if (leadout) status = stat_reg;
            source_cdda_queue(CDIRQ_DATA_END, &status, 1);
            return;
        }

        unsigned tens = last_valid_subq[9] >> 4;
        if (fresh && (mode_reg & 4) && source_cdda.report_last_tens != tens) {
            source_cdda.report_last_tens = tens;
            unsigned channel = last_valid_subq[8] & 1;
            unsigned peak = 0;
            for (unsigned i = channel * 2; i < sizeof(raw); i += 4) {
                int sample = raw[i] | ((unsigned)raw[i + 1] << 8);
                if (sample >= 32768) sample -= 65536;
                unsigned magnitude = (unsigned)(sample < 0 ? -sample : sample);
                if (magnitude > peak) peak = magnitude;
            }
            if (peak > 32767) peak = 32767;
            peak |= channel << 15;
            unsigned time = (last_valid_subq[9] & 0x10) ? 3 : 7;
            uint8_t report[8] = {stat_reg, last_valid_subq[1], last_valid_subq[2],
                last_valid_subq[time], last_valid_subq[time + 1], last_valid_subq[time + 2],
                (uint8_t)peak, (uint8_t)(peak >> 8)};
            if (time == 3) report[4] |= 0x80;
            source_cdda_queue(CDIRQ_DATA_READY, report, sizeof(report));
        }

        unsigned slot = source_cdda.pipe_at;
        /* The oldest buffered sector reaches the SPU only while the sub-Q of
         * the sector under the head names audio (control bit 0x40 clear). */
        if (source_cdda.pipe_count == 2) {
            if (!(last_valid_subq[0] & 0x40)) {
                int16_t stereo[1176];
                for (unsigned i = 0; i < 1176; ++i) {
                    int sample = source_cdda.pipe[slot][i * 2] |
                                 ((unsigned)source_cdda.pipe[slot][i * 2 + 1] << 8);
                    if (sample >= 32768) sample -= 65536;
                    stereo[i] = cd_muted ? 0 : (int16_t)sample;
                }
                cd_apply_decode_volume(stereo, 588);
                spu_cd_audio_push(stereo, 588);
                ++cdda_sectors_played;
            }
        } else {
            ++source_cdda.pipe_count;
        }
        memcpy(source_cdda.pipe[slot], raw, sizeof(raw));
        source_cdda.pipe_at = slot ^ 1;
        cdda_track = bcd_to_bin(last_valid_subq[1]);
        ++cdda_lba;
        ++source_cdda.sectors_read;
        lba_to_msf(cdda_lba, 150, &read_min, &read_sec, &read_sect);
        cdda_delay += CDROM_SINGLE_SPEED_SECTOR_CYCLES;
    }
}

static uint64_t source_drive_init_travel(int origin, int motor_on, int paused)
{
    uint32_t floor_cycles, jitter;
    if (s_cd_reset_seek_draw_first) {
        jitter = source_clock_random(25000);
        floor_cycles = source_clock_random(3250000);
    } else {
        floor_cycles = source_clock_random(3250000);
        jitter = source_clock_random(25000);
    }
    uint64_t seek = (uint64_t)source_seek_lower_bound(origin, 0, motor_on, paused,
                                                      mode_reg) + jitter;
    return seek > floor_cycles ? seek : floor_cycles;
}

static int source_drive_last_decoded(void)
{
    source_drive_head_update();
    return source_drive_subq_lba;
}

static void source_drive_init_command(void)
{
    /* Spec section 4, S6: the drive profile is not qualified here. */
    if (cdda_playing || (reading && (stat_reg & CDSTAT_SEEK))) {
        fprintf(stderr, "[CDROM] Init during CD-DA or an unfinished read seek is outside the qualified drive profile\n");
        exit(2);
    }

    /* I1: the first response carries the status from before the command
     * (PSX-SPX "Init"). */
    response_push(stat_reg);
    set_irq(CDIRQ_ACK);

    /* I6 times the travel with the motor bit and the paused state from
     * before the command, and with the new mode. */
    int motor_was_on = (stat_reg & CDSTAT_MOTOR) != 0;
    int was_paused = s_source_seek_paused;

    /* I2: demute, mode 20h (PSX-SPX "Init"), and a waiting Setloc is
     * cancelled: its flag is cleared and its stored sector becomes 0. All of
     * it at the command, not at the second response. */
    cd_muted = 0;
    mode_reg = 0x20;
    setloc_pending = 0;
    s_setloc_lba = 0;

    /* I9: the head is already travelling. The command is answered, the
     * second response is asked for again shortly, and the travel goes on with
     * no new draw. PSX-SPX "Init" says such a command is dropped; the oracle
     * answers it (cdrom_source_explicit_timing_test; fixture N2 is owed).
     * This test comes before the disc test. */
    if (source_reset_phase == 1) {
        pending_arm(0x0A, 256, 1);
        return;
    }

    /* I3: without a disc only the second response follows. 70,000 cycles has
     * no oracle observation yet (fixture N1, no-disc row). */
    if (!has_disc()) {
        pending_arm(0x0A, 70000, 1);
        return;
    }

    /* I5: an established read keeps the head two sectors ahead of the next
     * delivery. Init travels from there; otherwise from the idle head, or
     * from the delivery cursor when no head model runs. */
    int origin = msf_to_lba(read_min, read_sec, read_sect);
    if (reading) origin += 2;
    else if (source_drive_head_valid) origin = source_drive_head_lba;

    /* I4: abort everything (PSX-SPX "Init"): streams, buffers, the pended
     * data-ready notice, queued CD audio and the XA decoder. The drive seeks
     * with the motor on and is no longer in the paused timing state. */
    stop_read_stream();
    stop_cdda_playback();
    clear_sector_buffer();
    cdrom_clear_pending_dataready();
    spu_cd_audio_reset();
    xa_reset_decode();
    stat_reg = (uint8_t)((stat_reg & ~(CDSTAT_READ | CDSTAT_PLAY)) |
                         CDSTAT_MOTOR | CDSTAT_SEEK);
    s_source_seek_paused = 0;

    /* I6, I7: stage 1. The head counts as being at sector 0 with its window
     * there, and stands still until the reset ends. The hold flag and the
     * last decoded position are kept. */
    s_source_reset_due = psx_cycle_count +
        source_drive_init_travel(origin, motor_was_on, was_paused);
    source_reset_phase = 1;
    source_drive_head_start(0, 0, UINT64_MAX);

    /* I8: the second response has its own deadline, fitted to oracle
     * observation (Crash Bandicoot 7798S; fixture N1 is owed). */
    pending_arm(0x0A, 4100000, 1);
}

static void process_source_reset(void)
{
    if (!s_source_clock || !s_source_reset_due || psx_cycle_count < s_source_reset_due)
        return;

    if (!s_nymashock_drive) {
        s_source_reset_due = 0;
        if (source_clock_receive_ready()) {
            response_clear();
            response_push(stat_reg);
            set_irq(2);
            fire_cdrom_irq();
        }
        cd_muted = 0;
        spu_cd_audio_reset();
        xa_reset_decode();
        mode_reg = 0x20;
        s_source_seek_paused = 1;
        read_min = seek_min = 0;
        read_sec = seek_sec = 2;
        read_sect = seek_sect = 0;
        s_setloc_lba = 0;
        setloc_seek_far = 0;
        return;
    }

    while (s_source_reset_due && s_source_reset_due <= psx_cycle_count) {
        if (source_drive_hold_logical && source_reset_phase < 3) {
            source_drive_head_lba = source_reset_phase;
            ++source_reset_phase;
            s_source_reset_due += 451584;
            continue;
        }
        read_min = seek_min = 0;
        read_sec = seek_sec = 2;
        read_sect = seek_sect = 0;
        stat_reg &= (uint8_t)~64u;
        s_source_seek_paused = 1;
        source_reset_phase = 0;
        source_drive_head_lba = source_drive_hold_logical ? -6 : -8;
        source_drive_head_due = s_source_reset_due +
            (source_drive_hold_logical ? 451584u : 903168u);
        s_source_reset_due = 0;
        source_drive_head_update();
        return;
    }
}

static int getlocl_no_header(void) {
    return last_sector_have_raw==GETLOCL_NO_HEADER || last_sector_have_raw==GETLOCL_SEEK_DONE;
}
static void getlocp_seek_done_clear(void) {
    if(last_sector_have_raw==GETLOCL_SEEK_DONE) last_sector_have_raw=GETLOCL_NO_HEADER;
}
static int source_read_model(void) {
    return s_source_clock || s_source_read_pipeline || s_source_explicit_seek_model || s_source_toc_seek_model;
}
static void source_start_read_stream(uint8_t cmd) {
    last_sector_have_raw = GETLOCL_NO_HEADER;   /* until the first data sector */
    int source_target = setloc_pending ? s_setloc_lba :
        msf_to_lba(read_min, read_sec, read_sect);
    int seek_cycles = implicit_read_seek_cycles();
    source_cdda.position_valid=0;source_cdda.async_type=source_cdda.async_count=0;source_cdda.seeking=0;
    cdda_playing = 0;
    cdda_track = 0;
    cdda_delay = 0;
    cdda_data_end_pending = 0;
    stat_reg &= (uint8_t)~CDSTAT_PLAY;
    clear_sector_buffer();
    /* Drive-state change cancels any pended notification. [NOT OBSERVED:
     * pending the C5c replay (PS1B-212).] */
    cdrom_clear_pending_dataready();
    if (mode_reg & 0x40u) {
        xa_reset_decode();
        spu_cd_audio_reset();
    }
    if (setloc_pending) {
        read_min = seek_min;
        read_sec = seek_sec;
        read_sect = seek_sect;
    }
    setloc_pending = 0;
    if (s_source_clock) {
        lba_to_msf(source_target, 150, &read_min, &read_sec, &read_sect);
        s_source_read_start_lba = source_target;
    }
    /* Spec 6.2 R1: a read start leaves the drive holding a data position. A
     * read that only continues the stream never gets here. */
    if (s_nymashock_drive) source_drive_hold_logical = 1;
    read_cmd = cmd;
    read_delay = seek_cycles + initial_read_delay_cycles();
    s_cd_probe_read_start_count++;
    s_cd_probe_read_start_cycles += (uint64_t)read_delay;
    s_cd_timing_next_due = psx_cycle_count + (uint64_t)read_delay;
    s_cd_timing_stream_starts++;
    cd_stream_account_end();               /* a read started over an open one */
    s_cd_stream_open_seq = s_cd_timing_total;
    s_cd_stream_starts++;
    read_has_sector = 0;
    reading = 1;
    s_source_seek_paused = 0;
    stat_reg &= (uint8_t)~(CDSTAT_SEEK | CDSTAT_READ | CDSTAT_PLAY);
    stat_reg |= seek_cycles ? CDSTAT_SEEK : CDSTAT_READ;
    /* Spec 6.11: a read start spins the motor up (PSX-SPX: Read starts the
     * motor by itself). The seek above was charged with the motor state the
     * command found, so the first read after a Stop pays the spin-up once.
     * A Pause leaves the motor on, so a read re-issued before the first
     * sector does not pay it again. Without this a game library that
     * restarts a read after 60 silent vblanks (one second) never gets a
     * sector after a Stop: every attempt is charged a new full second
     * (PS1B-317, Time Crisis). A source profile follows the same rule: its
     * Pause leaves the motor on too, and without the bit the retry of
     * Street Fighter EX Plus Alpha at LBA 8693 was charged 45.6M cycles,
     * longer than the library's 35.66M, so no sector ever arrived
     * (PS1G-120). */
    stat_reg |= CDSTAT_MOTOR;
    /* ENQUEUE: sector-read stream scheduled (due in read_delay cycles). A
     * content load that happens in OFF but not ON shows up as a missing
     * SRC_CD_READ enqueue here. */
    event_ring_record_aux(EV_ENQ, (uint8_t)SRC_CD_READ, (uint32_t)read_delay);
    {
        static int s_bisect = -1;
        if (s_bisect < 0) {
            const char *e = getenv("PSX_RB_CD_BISECT");
            s_bisect = (e && e[0] && e[0] != '0') ? 1 : 0;
        }
        if (s_bisect) {
            fprintf(stderr,
                    "psxrecomp: rb cd-bisect ReadN-arm cyc=%llu csv=%u "
                    "rd0=%d mode=%02x div=%d cmd=%02x lba=%d "
                    "seek=%02d:%02d:%02d irq_f=%02x present_d=%d\n",
                    (unsigned long long)psx_cycle_count,
                    (unsigned)interrupts_get_cycles_since_vblank(),
                    read_delay, (unsigned)mode_reg, g_disc_speed_divisor,
                    (unsigned)cmd, msf_to_lba(read_min, read_sec, read_sect),
                    read_min, read_sec, read_sect,
                    (unsigned)irq_flag, irq_present_rem_cycles());
            fflush(stderr);
        }
    }
}

static int source_pause_complete_delay_cycles(void) {
    int source_audio = source_cdda.enabled && cdda_playing;
    /* A completed source seek is STANDBY, not PAUSED, despite identical
     * public status bits. Vagrant6315 original RAM keeps this Pause pending
     * at3016..3018; the fast path incorrectly advanced to Read in3016. */
    int source_standby = !reading && !source_audio && s_nymashock_drive && (stat_reg & CDSTAT_MOTOR) &&
                         !s_source_seek_paused;

    /* Spec 6.3 P1: nothing is reading or playing, so there is nothing to
     * settle. PSX-SPX "Response Timings" gives 1D25h..1F22h for "Pause (when
     * paused)"; 5,000 is the C2-pause-from-paused fixture. No drive value
     * changes and no draw is taken. */
    if (!reading && !source_audio && !source_standby &&
        !(stat_reg & (CDSTAT_READ | CDSTAT_PLAY)))
        return 5000;

    /* Spec 6.3 P3, step (a): where the drive stops. An active read stops at
     * the next sector it would deliver, anything else at the last sector it
     * did deliver (-1 when there is none). */
    int paused_lba = (reading || source_standby) ? msf_to_lba(read_min, read_sec, read_sect)
                             : last_sector_lba;
    if (source_standby && source_drive_head_valid) {
        /* In standby the mechanism keeps traversing its existing idle
         * window. Pause settles where that head is now, not at the old
         * logical SeekL target. Active reads retain their delivery cursor. */
        source_drive_head_update();
        paused_lba = source_drive_head_lba;
        /* The profile keeps the two logical header slots that SeekL defined,
         * so Pause backs them out of the reported position even when no data
         * read is active (observed at return 3018, counter 318958). */
        if (source_drive_hold_logical) paused_lba -= 2;
        lba_to_msf(paused_lba, 150, &read_min, &read_sec, &read_sect);
    }

    /* Step (b): the source CD-DA model gives back the sectors it read ahead
     * in this play, four at most, and the delivery cursor follows. Observed
     * behaviour: cdrom_source_cdda_fixtures.json (Octoshock 2.3 oracle). */
    if (source_audio) {
        uint32_t read_ahead = source_cdda.sectors_read;
        if (read_ahead > 4) read_ahead = 4;
        cdda_lba -= read_ahead;
        paused_lba = (int)cdda_lba;
        lba_to_msf(paused_lba, 150, &read_min, &read_sec, &read_sect);
    }

    /* Step (c), source profile: Pause moves the drive back to two sectors
     * before the next delivery, but not before the start of the stream. A
     * later seek or resumed read starts from there (PSX-SPX "Setloc, Read,
     * Pause"; Mega Man X5 6377M, return 7306). */
    if (s_source_clock && reading) {
        paused_lba -= 2;
        if (paused_lba < s_source_read_start_lba)
            paused_lba = s_source_read_start_lba;
        lba_to_msf(paused_lba, 150, &read_min, &read_sec, &read_sect);
    }

    /* Spec 6.3 P4, drive profile: the idle head starts at the paused position
     * and decodes its first sector one sector period after the command. The
     * hold flag and the last decoded position stay as they are. */
    if (s_nymashock_drive)
        source_drive_head_start(paused_lba, paused_lba,
                                psx_cycle_count + source_drive_sector_period());

    /* Spec 6.3 P5: a base plus a per-sector slope at double speed, the whole
     * value doubled at single speed. PSX-SPX "Response Timings" gives
     * 10477Ah..11B302h (2x) and 20EAEFh..216E3Ch (1x), "about 5 sectors"; base
     * and slope are fitted to oracle observation (held by
     * cdrom_source_explicit_timing_test: 1,124,621 at sector 4, 2x; fixture
     * N4 is owed). The physical-drive profile keeps signed lead-in positions:
     * Chrono4538 Pause at sector -6 completes 56 cycles before sector zero.
     * Keep signed division so the slope truncates toward zero. */
    int timing_lba = s_nymashock_drive ? paused_lba : (paused_lba > 0 ? paused_lba : 0);
    int64_t settle = 1124584 + (int64_t)timing_lba * 42596 / 4500;
    if (!(mode_reg & 0x80)) settle *= 2;

    /* Spec 6.3 P6, drive profile (which always has the source clock): one
     * draw on top, after the doubling. The range 0..100,000 is fitted to
     * oracle observation (Bio Hazard route; fixture N4 is owed). */
    if (s_nymashock_drive && s_source_clock) settle += source_clock_random(100000);

    return settle > INT32_MAX ? INT32_MAX : (int)settle;
}

static void source_profile_init(void) {
    memset(&s_source_cdda_play_debug, 0, sizeof(s_source_cdda_play_debug));
    s_source_firmware_model=source_boot_model("PSX_CD_FIRMWARE_MODEL");
    s_source_xa_eof_model=source_boot_model("PSX_CD_XA_EOF_MODEL");
    s_source_cold_status_model=source_boot_model("PSX_CD_COLD_STATUS_MODEL");
    s_source_toc_seek_model=source_boot_model("PSX_CD_TOC_SEEK_MODEL");
    s_source_explicit_seek_model=source_boot_model("PSX_CD_EXPLICIT_SEEK_MODEL");
    {
        const char *pipeline=getenv("PSX_CD_READ_START_MODEL");
        s_source_read_pipeline=pipeline && strcmp(pipeline,"octoshock-2.2.2-pipeline")==0;
    }
    cd_tape_free(&s_source_clock_tape);
    s_source_clock=0;s_source_command_due=0;s_source_ready_due=0;s_source_clock_calls=0;s_source_reset_due=0;
    s_source_command_phase=-1;s_source_args_remaining=0;
    const char *clock_tape=getenv("PSX_CD_SOURCE_CLOCK_TAPE");
    if(clock_tape && *clock_tape) {
        const char *pipeline=getenv("PSX_CD_READ_START_MODEL");
        if(!s_source_explicit_seek_model || !s_source_toc_seek_model ||
           !pipeline || strcmp(pipeline,"octoshock-2.2.2-pipeline") ||
           !cd_tape_load(&s_source_clock_tape,clock_tape)) {
            fprintf(stderr,"[CDROM] Source clock needs a valid tape, source seek models and source pipeline\n");exit(2);
        }
        s_source_clock=1;
    }
    const char *clock_extension=getenv("PSX_CD_SOURCE_CLOCK_TAPE_EXTENSION");
    if(clock_extension && *clock_extension &&
       (!s_source_clock || !cd_tape_extend(&s_source_clock_tape,clock_extension))) {
        fprintf(stderr,"[CDROM] Source clock extension must preserve the entire original tape prefix\n");exit(2);
    }
    /* Spec section 4, S1-S4. Unset or empty selects no drive profile.
     * octoshock-2.7 selects the same drive profile as nymashock-1.29.0 with
     * the other Init draw order. Anything else stops, and so does a drive
     * profile without the source clock tape, which supplies its draws. */
    s_nymashock_drive = 0;
    s_cd_262_lid_status = 0;
    s_cd_reset_seek_draw_first = 0;
    {
        const char *drive_model = getenv("PSX_CD_DRIVE_MODEL");
        if (drive_model && *drive_model) {
            /* Exact 2.4.2 and 2.6.2 original-movie state captures show the
             * staged two-header SeekL/physical-head path too. Their named
             * binding is distinct from borrowing a different host version.
             * This enables existing independently authored mechanics; the
             * immutable source clock tape and all refusal guards remain. */
            if (strcmp(drive_model, "octoshock-2.7") == 0 ||
                strcmp(drive_model, "octoshock-2.4.2") == 0 ||
                strcmp(drive_model, "octoshock-2.6.2") == 0) {
                s_cd_reset_seek_draw_first = 1;
            } else if (strcmp(drive_model, "nymashock-1.29.0") != 0 &&
                       strcmp(drive_model, "nymashock-1.32.1") != 0) {
                fprintf(stderr, "[CDROM] Unsupported PSX_CD_DRIVE_MODEL=%s\n", drive_model);
                exit(2);
            }
            if (!s_source_clock) {
                fprintf(stderr, "[CDROM] PSX_CD_DRIVE_MODEL needs the source clock tape\n");
                exit(2);
            }
            s_nymashock_drive = 1;
            s_cd_262_lid_status = strcmp(drive_model, "octoshock-2.6.2") == 0;
        }
    }
    /* Outside the qualified profile block above, so its registered lines stay
     * contiguous: the drive models that take the Octoshock route tray path. */
    s_cd_242_lid_model = 0;
    s_cd_27_lid_model = 0;
    if (s_nymashock_drive) {
        const char *drive_model = getenv("PSX_CD_DRIVE_MODEL");
        s_cd_27_lid_model = strcmp(drive_model, "octoshock-2.7") == 0;
        s_cd_242_lid_model = strcmp(drive_model, "octoshock-2.4.2") == 0;
    }
    {
        /* PSX_CD_INIT_DRAW_ORDER=jitter-first or floor-first sets the order of
         * Init's two draws for a route that has observed it (MediEvil 4875M on
         * nymashock-1.29.0 takes the jitter draw first, PS1G-126). Unset or empty
         * keeps the drive model's own order, so no other route changes. */
        const char *draw_order = getenv("PSX_CD_INIT_DRAW_ORDER");
        if (draw_order && *draw_order) {
            if (!s_nymashock_drive) {
                fprintf(stderr, "[CDROM] PSX_CD_INIT_DRAW_ORDER needs PSX_CD_DRIVE_MODEL\n");
                exit(2);
            }
            if (strcmp(draw_order, "jitter-first") == 0) {
                s_cd_reset_seek_draw_first = 1;
            } else if (strcmp(draw_order, "floor-first") == 0) {
                s_cd_reset_seek_draw_first = 0;
            } else {
                fprintf(stderr, "[CDROM] Unsupported PSX_CD_INIT_DRAW_ORDER=%s\n", draw_order);
                exit(2);
            }
        }
    }
    /* Spec section 4, S5: the drive starts with no idle head, no reset in
     * progress and nothing decoded. */
    source_drive_head_valid = 0;
    source_drive_head_lba = 0;
    source_drive_head_target = 0;
    source_drive_head_due = 0;
    source_drive_hold_logical = 0;
    source_reset_phase = 0;
    source_drive_subq_lba = -1;
    memset(&source_cdda,0,sizeof(source_cdda));
    const char *cdda_model=getenv("PSX_CD_CDDA_MODEL");
    if(cdda_model && *cdda_model) {
        if(strcmp(cdda_model,"octoshock-2.3") || !s_source_clock){fprintf(stderr,"[CDROM] Invalid source CDDA model or missing clock\n");exit(2);}
        source_cdda.enabled=1;
    }
}

static int reject_audio_disc_data_read(void) {
    int tracks = iso_handle ? iso_cdda_track_count(iso_handle) : 0;
    if (tracks <= 0) return 0;
    for (int track = 1; track <= tracks; ++track)
        if (!iso_cdda_track_is_audio(iso_handle, track)) return 0;
    response_push(stat_reg | CDSTAT_ERROR);
    response_push(0x40);
    set_irq(CDIRQ_ERROR);
    return 1;
}

static int source_exec_command(uint8_t cmd) {
    switch(cmd) {
    case 0x01: /* GetStat observes a stopped source drive. */
        if (!s_source_cold_status_model && !s_cd_262_lid_status) return 0;
        if (s_cd_262_lid_status) {
            if (s_lid.physical_open) stat_reg|=CDSTAT_SHELL;
        } else if (has_disc()) {
            if (!s_nymashock_drive) stat_reg|=CDSTAT_MOTOR;
        } else stat_reg|=CDSTAT_SHELL;
        response_push(stat_reg);
        if (s_source_cold_status_model && has_disc()) stat_reg&=(uint8_t)~CDSTAT_SHELL;
        else if (cdrom_lid_acknowledge_closed_shell(&s_lid)) stat_reg&=(uint8_t)~CDSTAT_SHELL;
        set_irq(CDIRQ_ACK);
        break;

    case 0x03: /* Source Play owns its physical two-sector pipeline. */
        if(!source_cdda.enabled)return 0;
        if(!has_disc()) {
            response_push(stat_reg|CDSTAT_ERROR);set_irq(CDIRQ_ERROR);break;
        }
        response_push(stat_reg);set_irq(CDIRQ_ACK);
        start_source_cdda(param_count?bcd_to_bin(param_fifo[0]):0);
        break;

    case 0x06: /* ReadN */
        if(!source_read_model()) return 0;
        if (!has_disc()) {
            response_push(stat_reg | CDSTAT_ERROR);
            set_irq(CDIRQ_ERROR);
            break;
        }
        if (reject_audio_disc_data_read()) break;
        if (read_continues_current_stream()) break;   /* ACKed inside */
        response_push(stat_reg);
        source_start_read_stream(cmd);
        set_irq(CDIRQ_ACK);
        break;

    case 0x07: /* MotorOn — spin the drive motor up. Two-phase like Stop:
                * INT3 (ACK) now with the current status, then a pending INT2
                * (COMPLETE) after spin-up reporting status with the motor bit
                * SET (psx-spx "07h MotorOn"). If the motor is ALREADY spinning
                * the hardware rejects the command with INT5 (ERROR) and error
                * code 0x20 ("wrong condition") — replicate that.
                *
                * Previously 0x07 had no case and fell through to default ->
                * CDIRQ_ERROR (INT5) with no error code, so a game that spins the
                * motor up (e.g. after a Stop) and waits for the MotorOn
                * completion IRQ would never see it. */
        if(!source_cdda.enabled) return 0;
        if (stat_reg & CDSTAT_MOTOR) {
            response_push(stat_reg | CDSTAT_ERROR);
            response_push(0x20); /* error code: motor already on */
            set_irq(CDIRQ_ERROR);
            break;
        }
        response_push(stat_reg);
        set_irq(CDIRQ_ACK);
        {
            /* [NOT OBSERVED: release policy] apply_speed(30000) on the default
             * path; the oracle's MotorOn INT2 from stopped is 3,386,864 cycles
             * after INT3 (C1-C9 row 8). */
            int lat = source_cdda.enabled?3386880:apply_speed(30000); /* motor spin-up */
            if(source_cdda.enabled){stat_reg|=CDSTAT_MOTOR;s_source_seek_paused=0;spu_cd_audio_reset();source_cdda.async_type=source_cdda.async_count=0;}
            pending_arm(0x07, lat, 1);
            s_cd_probe_motor_count++;
            s_cd_probe_motor_cycles += (uint64_t)lat;
        }
        break;

    case 0x08: { /* Stop — stop the motor. Two-phase like Pause: INT3 (ACK) now
                * with the pre-stop status (motor still spinning), then a pending
                * INT2 (COMPLETE) after the motor spins down, reporting the new
                * status with the motor bit cleared (psx-spx "08h Stop").
                *
                * Previously 0x08 had no case and fell through to default ->
                * CDIRQ_ERROR (INT5). A game that stops the drive on a scene
                * change then waits for the Stop completion IRQ never sees it and
                * hangs: Tsumu Light's CD library retries Stop forever (~90-frame
                * timeout) and never advances past its first content load. */
        if (source_disc_startup_active()) {
            response_push(stat_reg | CDSTAT_ERROR); response_push(0x80);
            set_irq(CDIRQ_ERROR); break;
        }
        s_source_seek_paused=0;
        if (s_cd_27_lid_model && s_nymashock_drive && source_drive_head_valid) {
            source_drive_head_update(); source_drive_head_due=UINT64_MAX;
        }
        if(!source_cdda.enabled) return 0;
        uint8_t old_status=stat_reg;
        stop_read_stream();
        stop_cdda_playback();
        s_source_seek_paused = 0;
        xa_reset_decode();
        spu_cd_audio_reset();
        stat_reg &= ~(CDSTAT_READ | CDSTAT_PLAY | CDSTAT_SEEK);
        response_push(source_cdda.enabled?old_status:stat_reg);
        set_irq(CDIRQ_ACK);
        {
            /* [NOT OBSERVED: release policy] apply_speed(30000) on the default
             * path; the oracle's Stop INT2 is 33,864 cycles after INT3 from
             * reading and 5,008 from stopped (C1-C9 rows 6-7). */
            int lat = source_cdda.enabled?((old_status&CDSTAT_MOTOR)?33868:5000):apply_speed(30000); /* motor spin-down */
            if(source_cdda.enabled)stat_reg&=~CDSTAT_MOTOR;
            pending_arm(0x08, lat, 1);
            s_cd_probe_stop_count++;
            s_cd_probe_stop_cycles += (uint64_t)lat;
        }
        break;
    }

    case 0x09: { /* Pause */
        if(!s_source_clock) {s_source_seek_paused=1;return 0;}
        /* The first response reports the status from before the Pause (read
         * bit still set); the second reports the paused drive (PSX-SPX
         * "Pause"). The default path keeps its own response. */
        uint8_t source_ack_status = stat_reg;
        int preserve_source_audio=source_cdda.enabled && cdda_playing;
        int complete_delay = source_pause_complete_delay_cycles();
        stop_read_stream();
        stop_cdda_playback();
        if(!preserve_source_audio){xa_reset_decode();spu_cd_audio_reset();}
        stat_reg &= ~(CDSTAT_READ | CDSTAT_PLAY | CDSTAT_SEEK);
        response_push(s_source_clock ? source_ack_status : stat_reg);
        set_irq(CDIRQ_ACK);
        pending_arm(0x09, complete_delay, 1);
        s_cd_probe_pause_count++;
        s_cd_probe_pause_cycles += (uint64_t)complete_delay;
        break;
    }

    case 0x0A: /* Source Init: spec I1-I11 / J2-J6. */
        if(!s_source_clock){s_source_seek_paused=has_disc()?1:0;return 0;}
        if(s_nymashock_drive){source_drive_init_command();break;}
        response_push(stat_reg);set_irq(CDIRQ_ACK);
        /* An existing reset keeps its deadline. A first Init stops active
         * producers at the command; mode/audio/position settle at completion. */
        if(!s_source_reset_due) {
            if(reading || pending_dataready)stop_read_stream();
            if(cdda_playing)stop_cdda_playback();
            s_source_reset_due=psx_cycle_count+1136000u;
            stat_reg=has_disc()?CDSTAT_MOTOR:CDSTAT_SHELL;
        }
        break;

    case 0x0E: /* Preserve queued source CDDA while changing mode. */
        if(!source_cdda.enabled || !cdda_playing)return 0;
        if(param_count>=1) {
            if(param_fifo[0]&0x80u) {
                fprintf(stderr,"[CDROM] Source CDDA double speed unqualified\n");exit(2);
            }
            mode_reg=param_fifo[0];
        }
        response_push(stat_reg);set_irq(CDIRQ_ACK);break;

    case 0x10: /* Spec C6: source header eligibility; shared header reply. */
        if(!source_read_model() && !source_cdda.enabled)return 0;
        if((stat_reg&CDSTAT_SEEK) || cdda_playing || getlocl_no_header()) {
            response_push(stat_reg|CDSTAT_ERROR);response_push(0x80);
            set_irq(CDIRQ_ERROR);break;
        }
        return 0;

    case 0x11: { /* GetlocP */
        /* The independently selected drive profile also owns idle SubQ. */
        if(!source_read_model() && !source_cdda.enabled && !s_nymashock_drive) return 0;
        if(source_cdda.enabled && source_cdda.position_valid) {
            for(unsigned i=1;i<=5;i++)response_push(last_valid_subq[i]);
            for(unsigned i=7;i<=9;i++)response_push(last_valid_subq[i]);
            set_irq(CDIRQ_ACK);break;
        }
        int lba;
        int track = 1;
        int track_lba = 0;
        if (cdda_playing) {
            lba = (int)cdda_lba;
            track = cdda_track;
            track_lba = (int)iso_cdda_track_start_lba(iso_handle, track);
        } else if (reading) {
            /* GetlocP reports the drive/sub-Q position. During a read the
             * sector stream has already advanced past the data-ready sector. */
            lba = msf_to_lba(read_min, read_sec, read_sect);
            /* Spec 6.7 G2b: on the source read pipeline, and so in the drive
             * profile, an established read names the sector after the next
             * delivery (Bio Hazard route; Spyro 4083M return 4423; oracle
             * fixture C6-getlocp-reading; cdrom_source_explicit_timing_test).
             * G2c: while the read is still seeking, GetlocP names the seek
             * target, which is the delivery cursor (PSX-SPX "GetlocP": it
             * works during a seek). */
            if ((s_nymashock_drive || s_source_read_pipeline) && !(stat_reg & CDSTAT_SEEK)) lba += 1;
            /* PS1B-109: on the source clock, G2c holds only until one sector
             * period before the first delivery. From then on the still
             * seeking read names the target + 1; the status stays 42h, motor
             * and seek (PSX-SPX "Status code (stat)"). read_delay is the time
             * left to that delivery; the source clock runs commands after the
             * read timer has advanced to the same cycle. Spyro the Dragon
             * 5523M, return 1661: GetlocP 132,712 cycles before the first
             * delivery of sector 113448 at double speed (period 225,792,
             * 150 sectors per second, PSX-SPX "CDROM Incoming Data / Buffer
             * Overrun Timings") names 113449; at 1,264,488 cycles before it
             * names 113448. Boundary: a GetlocP exactly one period before the
             * delivery names the target + 1. The reply then steps up by one at
             * each period boundary, as it does at each later delivery under
             * G2b, where the step is seen on the cycle of the delivery itself
             * (cdrom_getlocp_first_delivery_test). */
            if (s_source_clock && (stat_reg & CDSTAT_SEEK) && read_delay > 0 &&
                read_delay <= sector_delay_cycles()) lba += 1;
        } else if (s_nymashock_drive && source_drive_last_decoded() >= 0) {
            /* Spec 6.7 G3: idle drive, drive profile. Without a decoded
             * position the two branches below answer (G4). */
            lba = source_drive_subq_lba;
        } else if (last_sector_have_raw == GETLOCL_SEEK_DONE) {
            /* [NOT OBSERVED: release policy] A completed SeekL/SeekP is the
             * newest position: the drive stands at the seek target and no
             * sector has been delivered since, so the last delivered sector
             * is stale. Setloc alone only changes seek_* and does not get
             * here. Mega Man Legends 2 repeats GetlocP after SeekL until the
             * reply names the target (PS1G-95;
             * cdrom_getlocp_after_seek_test). */
            lba = msf_to_lba(read_min, read_sec, read_sect);
        } else if (last_sector_lba >= 0) {
            /* Stopped after delivered sectors, which includes a Pause: the
             * last delivered sector (cdrom_paused_subq_cycle_test). */
            lba = last_sector_lba;
        } else {
            /* Nothing delivered yet: the drive cursor. */
            lba = msf_to_lba(read_min, read_sec, read_sect);
        }
        if (subq_replacements_active) update_last_valid_subq((uint32_t)lba);
        if (subq_replacements_active && last_valid_subq_available) {
            for (int i = 1; i <= 5; ++i) response_push(last_valid_subq[i]);
            for (int i = 7; i <= 9; ++i) response_push(last_valid_subq[i]);
        } else {
            int rm, rs, rf;
            int am, as, af;
            lba_to_msf(lba - track_lba, 0, &rm, &rs, &rf);
            lba_to_msf(lba, 150, &am, &as, &af);
            response_push(bin_to_bcd(track));
            response_push(0x01);
            response_push(bin_to_bcd(rm));
            response_push(bin_to_bcd(rs));
            response_push(bin_to_bcd(rf));
            response_push(bin_to_bcd(am));
            response_push(bin_to_bcd(as));
            response_push(bin_to_bcd(af));
        }
        set_irq(CDIRQ_ACK);
        break;
    }

    case 0x15: /* SeekL (data-mode seek, uses sector headers) */
    case 0x16: /* SeekP (audio-mode seek, uses subchannel Q) — this model seeks
                * to the SetLoc position the same way for both commands. */
        if(!s_source_explicit_seek_model) return 0;
        if (!has_disc()) {
            response_push(stat_reg | CDSTAT_ERROR | CDSTAT_SEEKERR);
            set_irq(CDIRQ_ERROR);
            break;
        }
        {
        ++s_stream_generation;
        int lat = source_explicit_seek_cycles(cmd);
        /* Plain seeks ACK the old state, cancel the stream, then own SEEK.
         * PSX-SPX SeekL/P; upstream cd55e8a stops/retargets the read producer.
         * The already admitted guest data FIFO is independent and survives. */
        response_push(stat_reg);
        set_irq(CDIRQ_ACK);
        stop_read_stream();
        xa_reset_decode();
        spu_cd_audio_reset();
        stop_cdda_playback();
        getlocp_seek_done_clear();  /* set again when this seek completes */
        s_source_seek_paused = 0; /* source STANDBY after plain seek */
        read_min = seek_min;
        read_sec = seek_sec;
        read_sect = seek_sect;
        stat_reg = (stat_reg & ~(CDSTAT_READ | CDSTAT_PLAY)) |
                   CDSTAT_MOTOR | CDSTAT_SEEK;
            pending_arm(cmd, lat, 1); /* 0x15/0x16 — completed in process_pending */
            s_cd_probe_seek_count++;
            s_cd_probe_seek_cycles += (uint64_t)lat;
        }
        break;

    case 0x19: /* Test */
        if(!s_source_firmware_model && !(s_source_clock && param_count && param_fifo[0]==5)) return 0;
        if (param_count >= 1 && param_fifo[0] == 0x20) {
            /* Two documented HC05 identities: PU-7 default, PU-18 source.
             * The old default also suppresses BIOS ReadTOC; retain it without
             * claiming that firmware selection proves main-BIOS compatibility. */
            static const uint8_t version[2][4] = {
                {0x94,0x09,0x19,0xC0}, {0x97,0x01,0x10,0xC2}
            };
            for(int i=0;i<4;i++) response_push(version[s_source_firmware_model][i]);
            set_irq(CDIRQ_ACK);
        } else if (param_count >= 1 && param_fifo[0] == 0x05) {
            /* Get SCEx counters: INT3(total, success), no status byte (PSX-SPX).
             * The counters are not modelled; the drive answers as one that
             * received no SCEx string, as Octoshock and Mednafen do. Anti-modchip
             * checks read them (PS1G-116, Spyro: Year of the Dragon). */
            response_push(0x00);
            response_push(0x00);
            set_irq(CDIRQ_ACK);
        } else {
            response_push(stat_reg);
            set_irq(CDIRQ_ACK);
        }
        break;

    case 0x1B: /* ReadS */
        if(!source_read_model()) return 0;
        if (!has_disc()) {
            response_push(stat_reg | CDSTAT_ERROR);
            set_irq(CDIRQ_ERROR);
            break;
        }
        if (reject_audio_disc_data_read()) break;
        if (read_continues_current_stream()) break;   /* ACKed inside */
        response_push(stat_reg);
        source_start_read_stream(cmd);
        set_irq(CDIRQ_ACK);
        break;

    case 0x1E: /* ReadTOC */
        if(!s_source_toc_seek_model) return 0;
        response_push(stat_reg);
        set_irq(CDIRQ_ACK);
        getlocp_seek_done_clear();
        /* [NOT OBSERVED: release policy] ReadTOC INT2 30,000,000 cycles after
         * the ACK. The oracle's is 30,138,120 from standby and 30,138,872 from
         * reading (C2b row 10). Unscaled. */
        {
            int seek=s_source_toc_seek_model?source_toc_seek_cycles():0;
            int delay=seek>INT32_MAX-30000000?INT32_MAX:30000000+seek;
            pending_arm(0x1E, delay, 1);
            /* Spec 6.6 T2, drive profile: the idle window is anchored to
             * sector 0, the drive no longer holds a data position, and a
             * pended data-ready notice is dropped. The head keeps its
             * position and its valid flag, and walks down from there (rule
             * H3; Bio Hazard route; no isolating observation yet, fixture
             * N6). */
            if (s_nymashock_drive) {
                source_drive_hold_logical = 0;
                source_drive_head_target = 0;
                cdrom_clear_pending_dataready();
            }
            s_source_seek_paused = 1;
        }
        break;

    default:return 0;
    }
    return 1;
}

/* Modern CDF4 remains the complete default prefix. SCD2 adds only optional
 * source context; the modern prefix already owns the FIFO and decoder ring.
 * Match configuration and tape identity before restoring any guest state. */
#include "pst_wire.h"
static uint32_t source_snapshot_flags(void) {
    return (uint32_t)s_source_firmware_model | ((uint32_t)s_source_cold_status_model<<1) |
        ((uint32_t)s_source_toc_seek_model<<2) | ((uint32_t)s_source_explicit_seek_model<<3) |
        ((uint32_t)s_source_read_pipeline<<4) | ((uint32_t)s_source_clock<<5) |
        ((uint32_t)s_nymashock_drive<<6) | ((uint32_t)source_cdda.enabled<<7) |
        ((uint32_t)s_cd_reset_seek_draw_first<<8) |
        ((uint32_t)s_source_xa_eof_model<<9) | ((uint32_t)s_source_fifo_model<<10) |
        ((uint32_t)s_cd_262_lid_status<<11) | ((uint32_t)s_cd_242_lid_model<<12) |
        ((uint32_t)s_cd_27_lid_model<<13) | ((uint32_t)s_cd_210_lid_model<<14);
}
static int source_snapshot_emit(PstW *w) {
    const uint32_t flags=source_snapshot_flags();
    /* A plain explicit-seek selector needs only CDF6's common timing byte
     * and identity. It owns no optional clock, audio or physical-head state. */
    if(!(flags & ~8u))return 1;
    if(!pst_w_u32(w,0x32444353u) || !pst_w_u32(w,flags))return 0;
    if(!pst_w_u8(w,s_source_seek_paused))return 0;
    if(!pst_w_i32(w,s_source_read_start_lba))return 0;
    if(!pst_w_u64(w,pending_present_due))return 0;
    if(!pst_w_u64(w,pending.due_cyc))return 0;
    if(!pst_w_u64(w,cdrom_irq_present_due))return 0;
    if(!pst_w_u64(w,s_cd_timing_next_due))return 0;
    if(!pst_w_u64(w,s_lid.close_due))return 0;
    if(!pst_w_u8(w,s_lid.physical_open))return 0;
    if(!pst_w_u8(w,s_lid.shell_open_latched))return 0;
    if(!pst_w_i32(w,s_lid_irq_pending))return 0;
    if(flags & 32u) {
    if(!pst_w_u64(w,s_source_command_due))return 0;
    if(!pst_w_u64(w,s_source_ready_due))return 0;
    if(!pst_w_u64(w,s_source_reset_due))return 0;
    if(!pst_w_bytes(w,s_source_clock_tape.sha256,32u))return 0;
    if(!pst_w_u32(w,s_source_clock_tape.count))return 0;
    if(!pst_w_u32(w,s_source_clock_tape.cursor))return 0;
    if(!pst_w_u32(w,s_source_clock_calls))return 0;
    if(!pst_w_i32(w,s_source_command_phase))return 0;
    if(!pst_w_i32(w,s_source_args_remaining))return 0;
    }
    if(flags & 128u) {
    if(!pst_w_i32(w,source_cdda.seeking))return 0;
    if(!pst_w_i32(w,source_cdda.position_valid))return 0;
    if(!pst_w_i32(w,source_cdda.play_track_match))return 0;
    if(!pst_w_u32(w,source_cdda.sectors_read))return 0;
    if(!pst_w_u32(w,source_cdda.pipe_count))return 0;
    if(!pst_w_u32(w,source_cdda.pipe_at))return 0;
    if(!pst_w_u32(w,source_cdda.report_last_tens))return 0;
    if(!pst_w_bytes(w,source_cdda.pipe,4704u))return 0;
    if(!pst_w_bytes(w,source_cdda.async_data,8u))return 0;
    if(!pst_w_u32(w,source_cdda.async_type))return 0;
    if(!pst_w_u32(w,source_cdda.async_count))return 0;
    }
    if(flags & 64u) {
    if(!pst_w_i32(w,source_drive_head_valid))return 0;
    if(!pst_w_i32(w,source_drive_head_lba))return 0;
    if(!pst_w_i32(w,source_drive_head_target))return 0;
    if(!pst_w_u64(w,source_drive_head_due))return 0;
    if(!pst_w_i32(w,source_drive_hold_logical))return 0;
    if(!pst_w_i32(w,source_reset_phase))return 0;
    if(!pst_w_i32(w,source_drive_subq_lba))return 0;
    }
    if (!pst_w_u8(w,s_source_lid_notice_pending) || !pst_w_u8(w,s_source_lid_response)) return 0;
    if (flags & (1u<<10)) {
        if (!pst_w_bytes(w,s_source_fifo,sizeof s_source_fifo) ||
            !pst_w_i32(w,s_source_fifo_rd) || !pst_w_i32(w,s_source_fifo_end) ||
            !pst_w_bytes(w,s_source_sector,sizeof s_source_sector) ||
            !pst_w_i32(w,s_source_sector_unloaded)) return 0;
    }
    return 1;
}
static uint32_t source_snapshot_bytes(void) {
    PstW w;pst_w_init(&w,NULL,0);source_snapshot_emit(&w);return (uint32_t)w.written;
}
typedef struct SourceCdSnapshot {
    uint8_t paused;
    int32_t start;
    uint64_t pending_present;
    uint64_t pending_due;
    uint64_t irq_due;
    uint64_t next_due;
    uint64_t lid_due;
    uint8_t lid_open;
    uint8_t lid_shell;
    int32_t lid_irq;
    uint64_t command_due;
    uint64_t ready_due;
    uint64_t reset_due;
    uint8_t identity[32];
    uint32_t tape_count;
    uint32_t tape_cursor;
    uint32_t clock_calls;
    int32_t phase;
    int32_t args;
    int32_t seeking;
    int32_t position_valid;
    int32_t track_match;
    uint32_t sectors;
    uint32_t pipe_count;
    uint32_t pipe_at;
    uint32_t report_tens;
    uint8_t pipe[4704];
    uint8_t async_data[8];
    uint32_t async_type;
    uint32_t async_count;
    int32_t head_valid;
    int32_t head_lba;
    int32_t head_target;
    uint64_t head_due;
    int32_t hold;
    int32_t reset_phase;
    int32_t subq;
    uint8_t lid_notice, lid_response;
    uint8_t fifo[SOURCE_FIFO_BYTES], sector[WHOLE_SECTOR_SIZE];
    int32_t fifo_rd, fifo_end, sector_unloaded;
} SourceCdSnapshot;
static int source_snapshot_parse(const uint8_t *p,uint32_t len,int apply) {
    PstR r; uint32_t magic,flags;
    SourceCdSnapshot image={0};
    pst_r_init(&r,p,len);
    if(!pst_r_u32(&r,&magic) || magic!=0x32444353u ||
       !pst_r_u32(&r,&flags) || flags!=source_snapshot_flags())return 0;
    if(!pst_r_u8(&r,&image.paused) || !(image.paused<=1))return 0;
    if(!pst_r_i32(&r,&image.start) || !(image.start>=-150 && image.start<=450000))return 0;
    if(!pst_r_u64(&r,&image.pending_present) || !(1))return 0;
    if(!pst_r_u64(&r,&image.pending_due) || !(1))return 0;
    if(!pst_r_u64(&r,&image.irq_due) || !(1))return 0;
    if(!pst_r_u64(&r,&image.next_due) || !(1))return 0;
    if(!pst_r_u64(&r,&image.lid_due) || !(1))return 0;
    if(!pst_r_u8(&r,&image.lid_open) || !(image.lid_open<=1))return 0;
    if(!pst_r_u8(&r,&image.lid_shell) || !(image.lid_shell<=1))return 0;
    if(!pst_r_i32(&r,&image.lid_irq) || !(image.lid_irq>=0 && image.lid_irq<=1))return 0;
    if(flags & 32u) {
    if(!pst_r_u64(&r,&image.command_due) || !(1))return 0;
    if(!pst_r_u64(&r,&image.ready_due) || !(1))return 0;
    if(!pst_r_u64(&r,&image.reset_due) || !(1))return 0;
    if(!pst_r_bytes(&r,image.identity,32u))return 0;
    if(!pst_r_u32(&r,&image.tape_count) || !(image.tape_count<=s_source_clock_tape.count))return 0;
    if(!pst_r_u32(&r,&image.tape_cursor) || !(image.tape_cursor<=image.tape_count))return 0;
    if(!pst_r_u32(&r,&image.clock_calls) || !(1))return 0;
    if(!pst_r_i32(&r,&image.phase) || !(image.phase>=-1 && image.phase<=1))return 0;
    if(!pst_r_i32(&r,&image.args) || !(image.args>=0 && image.args<=PARAM_FIFO_SIZE))return 0;
    }
    if(flags & 128u) {
    if(!pst_r_i32(&r,&image.seeking) || !(image.seeking>=0 && image.seeking<=1))return 0;
    if(!pst_r_i32(&r,&image.position_valid) || !(image.position_valid>=0 && image.position_valid<=1))return 0;
    if(!pst_r_i32(&r,&image.track_match) || !(image.track_match>=-1 && image.track_match<=255))return 0;
    if(!pst_r_u32(&r,&image.sectors) || !(1))return 0;
    if(!pst_r_u32(&r,&image.pipe_count) || !(image.pipe_count<=2))return 0;
    if(!pst_r_u32(&r,&image.pipe_at) || !(image.pipe_at<=1))return 0;
    if(!pst_r_u32(&r,&image.report_tens) || !(image.report_tens<=15 || image.report_tens==255))return 0;
    if(!pst_r_bytes(&r,image.pipe,4704u))return 0;
    if(!pst_r_bytes(&r,image.async_data,8u))return 0;
    if(!pst_r_u32(&r,&image.async_type) || !(image.async_type<=5))return 0;
    if(!pst_r_u32(&r,&image.async_count) || !(image.async_count<=8))return 0;
    }
    if(flags & 64u) {
    if(!pst_r_i32(&r,&image.head_valid) || !(image.head_valid>=0 && image.head_valid<=1))return 0;
    if(!pst_r_i32(&r,&image.head_lba) || !(image.head_lba>=-150 && image.head_lba<=450000))return 0;
    if(!pst_r_i32(&r,&image.head_target) || !(image.head_target>=0 && image.head_target<=450000))return 0;
    if(!pst_r_u64(&r,&image.head_due) || !(1))return 0;
    if(!pst_r_i32(&r,&image.hold) || !(image.hold>=0 && image.hold<=1))return 0;
    if(!pst_r_i32(&r,&image.reset_phase) || !(image.reset_phase>=0 && image.reset_phase<=3))return 0;
    if(!pst_r_i32(&r,&image.subq) || !(image.subq>=-150 && image.subq<=450000))return 0;
    }
    if (!pst_r_u8(&r,&image.lid_notice) || image.lid_notice>1 ||
        !pst_r_u8(&r,&image.lid_response)) return 0;
    if (!(flags & (1u<<14)) && (image.lid_notice || image.lid_response)) return 0;
    if (flags & (1u<<10)) {
        if (!pst_r_bytes(&r,image.fifo,sizeof image.fifo) ||
            !pst_r_i32(&r,&image.fifo_rd) || !pst_r_i32(&r,&image.fifo_end) ||
            !pst_r_bytes(&r,image.sector,sizeof image.sector) ||
            !pst_r_i32(&r,&image.sector_unloaded)) return 0;
        if (image.fifo_rd<0 || image.fifo_end<image.fifo_rd || image.fifo_end>SOURCE_FIFO_BYTES ||
            image.sector_unloaded<0 || image.sector_unloaded>WHOLE_SECTOR_SIZE) return 0;
    }
    if(r.p!=r.end)return 0;
    if((flags & 32u) && (!cd_tape_identity_matches(&s_source_clock_tape,image.identity,
                         image.tape_count,image.tape_cursor) || image.clock_calls>image.tape_cursor))return 0;
    if((flags & 64u) && image.head_valid && !image.head_due)return 0;
    if(!apply)return 1;
    s_source_seek_paused=image.paused;
    s_source_read_start_lba=image.start;
    pending_present_due=image.pending_present;
    pending.due_cyc=image.pending_due;
    cdrom_irq_present_due=image.irq_due;
    s_cd_timing_next_due=image.next_due;
    s_lid.close_due=image.lid_due;
    s_lid.physical_open=image.lid_open;
    s_lid.shell_open_latched=image.lid_shell;
    s_lid_irq_pending=image.lid_irq;
    s_source_lid_notice_pending=image.lid_notice;
    s_source_lid_response=image.lid_response;
    if (flags & (1u<<10)) {
        memcpy(s_source_fifo,image.fifo,sizeof s_source_fifo);
        memcpy(s_source_sector,image.sector,sizeof s_source_sector);
        s_source_fifo_rd=image.fifo_rd; s_source_fifo_end=image.fifo_end;
        s_source_sector_unloaded=image.sector_unloaded;
    }
    if(flags & 32u) {
    s_source_command_due=image.command_due;
    s_source_ready_due=image.ready_due;
    s_source_reset_due=image.reset_due;
    /* Keep the loaded full/extended tape identity; restore only its cursor. */
    s_source_clock_tape.cursor=image.tape_cursor;
    s_source_clock_calls=image.clock_calls;
    s_source_command_phase=image.phase;
    s_source_args_remaining=image.args;
    }
    if(flags & 128u) {
    source_cdda.seeking=image.seeking;
    source_cdda.position_valid=image.position_valid;
    source_cdda.play_track_match=image.track_match;
    source_cdda.sectors_read=image.sectors;
    source_cdda.pipe_count=image.pipe_count;
    source_cdda.pipe_at=image.pipe_at;
    source_cdda.report_last_tens=image.report_tens;
    memcpy(source_cdda.pipe,image.pipe,4704u);
    memcpy(source_cdda.async_data,image.async_data,8u);
    source_cdda.async_type=image.async_type;
    source_cdda.async_count=image.async_count;
    }
    if(flags & 64u) {
    source_drive_head_valid=image.head_valid;
    source_drive_head_lba=image.head_lba;
    source_drive_head_target=image.head_target;
    source_drive_head_due=image.head_due;
    source_drive_hold_logical=image.hold;
    source_reset_phase=image.reset_phase;
    source_drive_subq_lba=image.subq;
    }
    return 1;
}
