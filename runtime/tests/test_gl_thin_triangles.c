/* Original source-owned one-pixel line coverage regression.
 * Reuse our source-owned fixture's linkage/context helpers; no retail data.
 * This tests native-footprint replication for authored one-pixel strips.
 * It does not qualify a title or define general enhanced triangle coverage. */
#define main retained_readback_fixture_main
#include "test_gl_readback_region.c"
#undef main


static void full_pixels(const char *label,int x,int y,int w,int h,int scale,uint16_t color) {
 unsigned char pixels[16*4*4*4]; /* <=16 native pixels, scale<=4, RGBA8 */
 int rw=w*scale,rh=h*scale,wrong=0,wrong_old_red=0;
 flush_flat_batch();flush_tex_batch();flush_cpu_upload();
 p_glBindFramebuffer(PSXGL_READ_FRAMEBUFFER,s_hr_fbo);
 glPixelStorei(GL_PACK_ALIGNMENT,1);glPixelStorei(PSXGL_PACK_ROW_LENGTH,0);
 glReadPixels(x*scale,y*scale,rw,rh,GL_RGBA,GL_UNSIGNED_BYTE,pixels);
 p_glBindFramebuffer(PSXGL_READ_FRAMEBUFFER,0);
 for(int j=0;j<rh;j++)for(int i=0;i<rw;i++){
  unsigned char *p=pixels+4*(j*rw+i);
  uint16_t expected=color?color:oracle[(y+j/scale)*1024+x+i/scale];
  uint16_t actual=(p[0]>>3)|((p[1]>>3)<<5)|((p[2]>>3)<<10);
  if(actual!=(expected&0x7fff)){wrong++;wrong_old_red+=actual==0x001f;}
 }
  printf("case=%s scale=%d wrong_hr_pixels=%d wrong_old_red_pixels=%d wrong_other_pixels=%d pixels=%d\n",label,scale,wrong,wrong_old_red,wrong-wrong_old_red,rw*rh);
 check(wrong==0,label);check(glGetError()==GL_NO_ERROR,"GL error");
}

/* Old/new comparison retains the actual HR image for excluded geometry. */
static void fingerprint(const char *label,int x,int y,int scale) {
 unsigned char pixels[16*4*4*4];uint64_t hash=UINT64_C(14695981039346656037);
 flush_flat_batch();flush_tex_batch();flush_cpu_upload();
 p_glBindFramebuffer(PSXGL_READ_FRAMEBUFFER,s_hr_fbo);
 glReadPixels(x*scale,y*scale,scale,16*scale,GL_RGBA,GL_UNSIGNED_BYTE,pixels);
 p_glBindFramebuffer(PSXGL_READ_FRAMEBUFFER,0);
 for(int i=0;i<16*scale*scale*4;i++){hash^=pixels[i];hash*=UINT64_C(1099511628211);}
 printf("control=%s scale=%d rgba_fnv64=%016llx\n",label,scale,(unsigned long long)hash);
 check(glGetError()==GL_NO_ERROR,"excluded geometry GL error");
}

static void native_pixels(const char *label,int x,int y,int w,int h) {
 uint16_t result[16];int wrong=0;
 check(gl_renderer_fbo_peek(x,y,w,h,result),"native sample read");
 for(int j=0;j<h;j++)for(int i=0;i<w;i++)
  wrong+=(result[j*w+i]&0x7fff)!=(oracle[(y+j)*1024+x+i]&0x7fff);
 check(wrong==0,label);
}

int main(int argc,char **argv){
 int scale=argc>1?atoi(argv[1]):1;
 if(scale!=1&&scale!=2&&scale!=4)return 2;
 for(int i=0;i<1024*512;i++)image[i]=oracle[i]=0x03e0;
 image[256*1024]=oracle[256*1024]=0xfc00;
 for(int j=0;j<16;j++)image[(288+j)*1024]=oracle[(288+j)*1024]=(uint16_t)(0x8000|((j+1)<<10));
 sw_renderer_init(oracle);sw_renderer_set_scale(1);sw_set_draw_area(0,0,1023,511);
 sw_set_mask_bits(0,0);sw_set_semi_transparency(0,0);sw_set_color_modulation(128,128,128,1);
 sw_draw_flat_rect(16,16,1,16,0x001f);sw_draw_flat_rect(16,64,16,1,0x001f);
 sw_draw_textured_triangle(16,16,0,0,17,16,0,0,16,32,0,0,0,0,0x110);
 sw_draw_textured_triangle(16,64,0,0,32,64,0,0,16,65,0,0,0,0,0x110);
 sw_copy_rect(16,16,80,16,1,16);sw_copy_rect(16,64,80,64,16,1);
 sw_draw_flat_rect(120,16,1,16,0x001f);
 sw_draw_shaded_textured_triangle(120,16,0,0,0x808080,121,16,0,0,0x808080,120,32,0,0,0x808080,0,0,0x110,0);
 sw_draw_flat_rect(160,16,1,16,0x001f);sw_set_semi_transparency(1,0);
 sw_draw_textured_triangle(160,16,0,0,161,16,0,0,160,32,0,0,0,0,0x110);
 sw_set_semi_transparency(0,0);sw_draw_flat_rect(200,16,1,16,0x001f);
 sw_set_semi_transparency(1,0);sw_draw_textured_rect(200,16,1,16,0,0,0,0,0x110);
 sw_set_semi_transparency(0,0);sw_draw_flat_rect(280,16,1,16,0x001f);
 sw_set_semi_transparency(1,0);
 sw_draw_textured_triangle(280,16,0,0,281,16,0,0,280,32,0,0,0,0,0x110);
 sw_draw_textured_triangle(280,32,0,0,281,16,0,0,281,32,0,0,0,0,0x110);
 sw_set_semi_transparency(0,0);
 sw_draw_flat_rect(440,16,1,16,0x001f);
 sw_draw_textured_triangle(440,16,0,32,441,16,0,32,440,32,0,48,0,0,0x110);
 sw_draw_flat_rect(480,64,16,1,0x001f);
 sw_draw_textured_triangle(480,64,0,32,496,64,0,48,480,65,0,32,0,0,0x110);
 if(SDL_Init(SDL_INIT_VIDEO)!=0)return 2;
 SDL_Window *win=SDL_CreateWindow("Thin primitive diagnostic",0,0,128,128,SDL_WINDOW_OPENGL|SDL_WINDOW_HIDDEN);
 if(!win){SDL_Quit();return 2;}
 glb_init(image);glb_set_scale(scale);gl_renderer_set_swap_interval(0);
 if(!gl_renderer_init_context(win)){SDL_DestroyWindow(win);SDL_Quit();return 2;}
 printf("vendor=%s renderer=%s version=%s scale=%d\n",glGetString(GL_VENDOR),glGetString(GL_RENDERER),glGetString(GL_VERSION),scale);
 glb_set_draw_area(0,0,1023,511);glb_set_mask_bits(0,0);glb_set_semi_transparency(0,0);glb_set_color_modulation(128,128,128,1);
 glb_draw_flat_rect(16,16,1,16,0x001f);glb_draw_flat_rect(16,64,16,1,0x001f);
 full_pixels("old column sentinel",16,16,1,16,scale,0x001f);
 full_pixels("old row sentinel",16,64,16,1,scale,0x001f);
 glb_draw_textured_triangle(16,16,0,0,17,16,0,0,16,32,0,0,0,0,0x110);
 glb_draw_textured_triangle(16,64,0,0,32,64,0,0,16,65,0,0,0,0,0x110);
 full_pixels("column before copy",16,16,1,16,scale,0);
 full_pixels("row before copy",16,64,16,1,scale,0);
 glb_copy_rect(16,16,80,16,1,16);glb_copy_rect(16,64,80,64,16,1);
 full_pixels("column after copy",80,16,1,16,scale,0);
 full_pixels("row after copy",80,64,16,1,scale,0);
 glb_draw_flat_rect(120,16,1,16,0x001f);
 glb_draw_shaded_textured_triangle(120,16,0,0,0x808080,121,16,0,0,0x808080,120,32,0,0,0x808080,0,0,0x110,0);
 full_pixels("shaded column",120,16,1,16,scale,0);
 glb_draw_flat_rect(160,16,1,16,0x001f);glb_set_semi_transparency(1,0);
 glb_draw_textured_triangle(160,16,0,0,161,16,0,0,160,32,0,0,0,0,0x110);
 full_pixels("semi column drawn once",160,16,1,16,scale,0);
 glb_set_semi_transparency(0,0);glb_draw_flat_rect(200,16,1,16,0x001f);
 glb_set_semi_transparency(1,0);glb_draw_textured_rect(200,16,1,16,0,0,0,0,0x110);
 full_pixels("textured rectangle drawn once",200,16,1,16,scale,0);
 glb_set_semi_transparency(0,0);glb_set_mask_bits(1,0);
 glb_draw_flat_rect(240,16,1,16,0x001f);glb_set_mask_bits(0,1);
 glb_draw_textured_triangle(240,16,0,0,241,16,0,0,240,32,0,0,0,0,0x110);
 full_pixels("mask check keeps old column",240,16,1,16,scale,0x001f);
 glb_set_mask_bits(0,0);glb_draw_flat_rect(280,16,1,16,0x001f);
 glb_set_semi_transparency(1,0);test_polygon_quad=1;
 glb_draw_textured_triangle(280,16,0,0,281,16,0,0,280,32,0,0,0,0,0x110);
 glb_draw_textured_triangle(280,32,0,0,281,16,0,0,281,32,0,0,0,0,0x110);
 test_polygon_quad=0;
 full_pixels("paired quad triangles drawn once",280,16,1,16,scale,0);
 glb_set_semi_transparency(0,0);
 full_pixels("untouched persistent background",900,400,2,2,scale,0x03e0);
 glb_draw_flat_rect(320,16,1,16,0x001f);
 glb_draw_textured_triangle(320,16,0,0,321,16,4,0,320,32,0,0,0,0,0x110);
 fingerprint("cross-axis UV triangle",320,16,scale);
 glb_draw_flat_rect(360,16,1,16,0x001f);
 glb_set_precise_triangle(1,360*65536,16*65536,361*65536,16*65536,360*65536,32*65536);
 glb_draw_textured_triangle(360,16,0,0,361,16,0,0,360,32,0,0,0,0,0x110);
 fingerprint("precision triangle",360,16,scale);
 glb_draw_flat_rect(400,16,1,16,0x001f);
 glb_set_perspective_triangle(1,1.0f,2.0f,1.0f);
 glb_draw_textured_triangle(400,16,0,0,401,16,0,0,400,32,0,0,0,0,0x110);
 fingerprint("perspective triangle",400,16,scale);
 glb_draw_flat_rect(440,16,1,16,0x001f);
 glb_draw_textured_triangle(440,16,0,32,441,16,0,32,440,32,0,48,0,0,0x110);
 native_pixels("column varying UV keeps native samples",440,16,1,16);
 glb_draw_flat_rect(480,64,16,1,0x001f);
 glb_draw_textured_triangle(480,64,0,32,496,64,0,48,480,65,0,32,0,0,0x110);
 native_pixels("row varying UV keeps native samples",480,64,16,1);
 glb_draw_flat_rect(520,16,1,16,0x001f);
 glb_draw_textured_triangle(520,16,0,0,521,32,0,0,520,32,0,0,0,0,0x110);
 fingerprint("bottom-tip column",520,16,scale);
 printf("checks=%d failures=%d\n",checks,failures);
 gl_renderer_shutdown();SDL_DestroyWindow(win);SDL_Quit();return failures?1:0;
}
