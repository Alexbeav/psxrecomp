/* Accepted first-party ac56b624 source dispatch and optional FIFO schedule. */
static int gpu_source_draw_triangle(const uint32_t *words,int second) {
    unsigned opcode=words[0]>>24,stride=source_gpu_polygon_stride(opcode);
    int x[3],y[3],extra_work=0;uint32_t colors[3];SourceGPUTexture texture={0};
    SourceGPUCommandProjection state;source_gpu_runtime_copy(0,&state);
    if((opcode&4) && !second) {
        uint32_t page=words[4+!!(opcode&0x10)]>>16;
        gr_source_texture_control(SOURCE_GPU_TEXTURE_POLYGON_PAGE,page);
        set_tpage_from_poly((uint16_t)page);
    }
    texture.page=current_texpage();texture.window=texture_window_value;
    texture.clut=words[2]>>16;texture.raw=!!(opcode&1);texture.load_clut=!second;
    for(unsigned i=0;i<3;i++) {
        unsigned v=i+(second?1:0);
        x[i]=source_gpu_command_coord(words[1+stride*v],0)+draw_offset_x;
        y[i]=source_gpu_command_coord(words[1+stride*v],16)+draw_offset_y;
        colors[i]=words[(opcode&0x10)?stride*v:0]&0xffffffu;
        if(opcode&4)texture.uv[i]=words[2+stride*v]&0xffff;
    }
    gr_set_semi_transparency(!!(opcode&2),(int)semi_transparency);
    if(!gr_draw_source_triangle(x,y,colors,!!(opcode&0x10),(int)dither_enabled,
        (state.display_mode&0x24)==0x24 && !(state.draw_mode&0x400),state.skip_field,
        (opcode&4)?&texture:0,&extra_work))
        psx_fatal_halt("Source GPU triangle requires native software rendering without geometry enhancement");
    return extra_work;
}

static int gpu_source_draw_block(const uint32_t *words) {
    unsigned opcode=words[0]>>24;int extra=0;
    SourceGPUCommandProjection state;source_gpu_runtime_copy(0,&state);
    SourceGPUBlock block={0};block.words=words;block.draw_mode=state.draw_mode;
    block.texture_window=state.texture_window;
    block.clip_left=state.clip_x0;block.clip_top=state.clip_y0;
    block.clip_right=state.clip_x1;block.clip_bottom=state.clip_y1;
    block.x=source_gpu_sprite_origin(words[1],0,state.offset_x);
    block.y=source_gpu_sprite_origin(words[1],16,state.offset_y);
    if(source_gpu_line_supported(opcode)) {
        block.x=source_gpu_command_coord(words[1],0)+state.offset_x;
        block.y=source_gpu_command_coord(words[1],16)+state.offset_y;
    }
    block.interlace=(state.display_mode&0x24)==0x24 && !(state.draw_mode&0x400);
    block.skip_field=state.skip_field;
    if(opcode==0x80)gr_source_texture_control(2,0);
    gr_set_semi_transparency(!!(opcode&2),(int)semi_transparency);
    if(!gr_draw_source_block(&block,&extra))psx_fatal_halt("Source GPU block requires native software rendering without enhancement");
    return extra;
}

static int gpu_source_dispatch(const SourceGPUCommandDispatch *event) {
    extern int g_exec_phase;
    int previous_phase=g_exec_phase,extra_work=0;
    g_exec_phase=4;
    memcpy(gp0_cmd_buf,event->words,event->count*sizeof(uint32_t));
    gp0_cmd_source_addr=gp0_next_source_addr;
    gp0_words_collected=gp0_words_needed=(int)event->count;
    if(event->kind==SOURCE_GPU_DISPATCH_UPLOAD_WORD) {
        gpu_write_gp0_body(event->words[0]);
    } else if(event->kind==SOURCE_GPU_DISPATCH_COMMAND) {
        unsigned opcode=event->words[0]>>24;
        if(source_gpu_block_supported(opcode) || source_gpu_line_supported(opcode)) {
            gp0_opcode_count[opcode]++;
            if(opcode==2)gp0_fill_count++;else if(opcode==0x80)gp0_copy_count++;else gp0_draw_count++;
            gp0_ring_record(gp0_cmd_buf,event->count);
            extra_work=gpu_source_draw_block(event->words);
        } else if(source_gpu_polygon_supported(opcode)) {
            gp0_opcode_count[opcode]++;gp0_draw_count++;
            gp0_ring_record(gp0_cmd_buf,event->count);
            extra_work=gpu_source_draw_triangle(event->words,0);
        } else {
            if(opcode==1)gr_source_texture_control(1,0);
            if(opcode==0xa0 || opcode==0x80)gr_source_texture_control(2,0);
            if(opcode==0xe1)gr_source_texture_control(3,event->words[0]);
            gp0_execute_command();
        }
    } else if(event->kind==SOURCE_GPU_DISPATCH_QUAD_FIRST) {
        /* Dispatch only the first three vertex groups. */
        unsigned opcode=event->words[0]>>24;
        gp0_opcode_count[opcode]++;gp0_draw_count++;
        extra_work=gpu_source_draw_triangle(event->words,0);
    } else if(event->kind==SOURCE_GPU_DISPATCH_QUAD_SECOND) {
        /* The completed packet supplies source vertex groups (1,2,3). */
        gp0_ring_record(gp0_cmd_buf,event->count);
        extra_work=gpu_source_draw_triangle(event->words,1);
    }
    g_exec_phase=previous_phase;
    return extra_work;
}

/* Reuse the owned cost helpers as a compatibility schedule. These costs are
 * not a claim of physical GPU timing or texture-cache work on every backend. */
static int gpu_queue_command_cost(void) {
    SourceGPUCommandProjection state = {0};
    state.clip_x0=draw_area_left; state.clip_y0=draw_area_top;
    state.clip_x1=draw_area_right; state.clip_y1=draw_area_bottom;
    state.offset_x=draw_offset_x; state.offset_y=draw_offset_y;
    state.mask_bits=(check_mask_bit<<1)|set_mask_bit;
    /* The ordinary renderer owns interlace and enhancement policies. Charge
     * only the rows it draws: 480i with drawing to the displayed field
     * prohibited skips that field's rows. */
    int skipped=vertical_interlace && vres && !draw_to_display?(int)interlace_field:-1;
    if (skipped >= 0) { state.display_mode=0x24u; state.skip_field=(unsigned)skipped; }
    unsigned op=gp0_cmd_buf[0]>>24;
    gpu_queue_quad_second=0;
    if (source_gpu_polygon_supported(op)) {
        int cost=source_gpu_command_polygon_cost(&state,gp0_cmd_buf,0);
        if(op&8u) {
            int second=source_gpu_command_polygon_cost(&state,gp0_cmd_buf,1);
            cost+=second;
            gpu_queue_quad_second=second > 0 ? second : 0;
        }
        return cost > 0 ? cost : 0;
    }
    if ((op & 0xe0u)==0x80u) return 2+source_gpu_command_copy_cost(&state,gp0_cmd_buf);
    if (source_gpu_block_supported(op)) return 2+source_gpu_command_block_cost(&state,gp0_cmd_buf);
    if (source_gpu_line_supported(op)) return 2+source_gpu_command_line_cost(&state,gp0_cmd_buf);
    return op==0 || (op>=0xe3 && op<=0xe5) ? 0 : 2;
}

static void gpu_queue_consume(void) {
    while (gpu_queue.count && gpu_queue.credit >= 0 && !vram_read_active) {
        uint32_t value=gpu_queue.words[0];
        uint32_t saved_source=gp0_next_source_addr;
        uint16_t saved_rank=gp0_ot_rank;
        gp0_next_source_addr=gpu_queue.sources[0];
        gp0_ot_rank=(uint16_t)gpu_queue.ranks[0];
        --gpu_queue.count;
        memmove(gpu_queue.words,gpu_queue.words+1,gpu_queue.count*4u);
        memmove(gpu_queue.sources,gpu_queue.sources+1,gpu_queue.count*4u);
        memmove(gpu_queue.ranks,gpu_queue.ranks+1,gpu_queue.count*4u);
        gpu_queue.words[gpu_queue.count]=gpu_queue.sources[gpu_queue.count]=gpu_queue.ranks[gpu_queue.count]=0;
        Gp0State before=gp0_state;
        int previous_vertices=polyline_has_prev;
        int x=polyline_prev_x,y=polyline_prev_y;
        extern int g_exec_phase;
        int old_phase=g_exec_phase; g_exec_phase=4;
        gpu_write_gp0_body(value);
        g_exec_phase=old_phase;
        if ((before==GP0_IDLE || before==GP0_COLLECTING) &&
            (gp0_state==GP0_IDLE || gp0_state==GP0_VRAM_WRITE)) {
            int cost=gpu_queue_command_cost();
            gpu_queue.credit-=cost;
            gpu_walk_quad_pending=gpu_queue_quad_second > 0 && gpu_queue_quad_second < cost;
            if(gpu_walk_quad_pending)
                gpu_walk_quad_floor=gpu_queue.credit+(cost-gpu_queue_quad_second);
        } else if ((before==GP0_POLYLINE_MONO && previous_vertices && gp0_state==before) ||
                 (before==GP0_POLYLINE_SHADED && previous_vertices==2)) {
            int dx=abs(polyline_prev_x-x),dy=abs(polyline_prev_y-y);
            gpu_queue.credit-=source_gpu_t_line(0,0,dx,dy,0);
        }
        gp0_next_source_addr=saved_source; gp0_ot_rank=saved_rank;
    }
}

void gpu_queue_service(void) {
    if (!source_gpu_queue_enabled || source_gpu_runtime_active()) return;
    uint64_t now=psx_get_cycle_count();
    if(now < gpu_queue.cycle) psx_fatal_halt("GPU queue clock moved backwards");
    gpu_queue.credit=source_gpu_t_credit(gpu_queue.credit,now-gpu_queue.cycle);
    gpu_queue.cycle=now;
    gpu_queue_consume();
}

uint32_t gpu_queue_cycles_to_event(void) {
    if(!source_gpu_queue_enabled && !source_gpu_runtime_active()) {
        uint64_t now=psx_get_cycle_count();
        if(now >= gpu_dma_resume_at) return UINT32_MAX;
        uint64_t d=gpu_dma_resume_at-now;
        return d > 0xFFFFFFF0u ? 0xFFFFFFF0u : (uint32_t)d;
    }
    if(source_gpu_runtime_active() || (!gpu_queue.count && gpu_queue.credit>=0) || vram_read_active)
        return UINT32_MAX;
    return gpu_queue.credit<0 ? (uint32_t)(-gpu_queue.credit+1)/2u : 1u;
}

/* Ordinary renderer: run a GP0 word now and, when it completes a command,
 * charge that command's drawing time. The command starts once the credit is
 * back at zero (SOURCE_GPU_T_ADMIT_AT); until then it occupies the FIFO. */
static void gpu_draw_timed_gp0(uint32_t val) {
    Gp0State before = gp0_state;
    gpu_write_gp0_body(val);
    if (!((before == GP0_IDLE || before == GP0_COLLECTING) &&
          (gp0_state == GP0_IDLE || gp0_state == GP0_VRAM_WRITE)))
        return;
    int cost = gpu_queue_command_cost();
    gpu_queue_quad_second = 0;
    uint64_t now = psx_get_cycle_count();
    uint64_t start = gpu_draw_cycle > now ? gpu_draw_cycle : now;
    int32_t credit = source_gpu_t_credit(gpu_draw_credit, start - gpu_draw_cycle);
    if (credit < 0) {
        uint64_t wait = ((uint64_t)(-(int64_t)credit) + 1u) / 2u;
        credit = source_gpu_t_credit(credit, wait);
        start += wait;
    }
    gpu_dma_resume_at = start;
    gpu_draw_credit = credit - cost;
    gpu_draw_cycle = start;
}

/* GPU DMA request (GPUSTAT.25 for CPU-to-GP0). Off with the FIFO full. In the
 * ordinary renderer, off while the GPU is still drawing and the FIFO already
 * holds the whole next command: a list transfer then waits on drawing time
 * instead of always finishing at once. Team Buddies (PS1G-160) reads CHCR2
 * right after its progress-bar list and relies on the transfer still running. */
int gpu_queue_has_space(void) {
    if (source_gpu_queue_enabled || source_gpu_runtime_active())
        return gpu_queue.count < 16u;
    return psx_get_cycle_count() >= gpu_dma_resume_at;
}

/* The list walk's readiness before a header: GPUSTAT bit 28 (no queued word,
 * no command partly received), and not inside the first triangle of a quad.
 * A VRAM read still owed leaves bit 28 set (probe case 4a). */
int gpu_dma_walk_ready(void) {
    if (source_gpu_runtime_active()) return 1;
    gpu_queue_service();
    if (gpu_queue.count) return 0;
    if (gp0_state != GP0_IDLE && gp0_state != GP0_VRAM_WRITE) return 0;
    if (gpu_walk_quad_pending) {
        if (gpu_queue.credit < gpu_walk_quad_floor) return 0;
        gpu_walk_quad_pending = 0;
    }
    return 1;
}
