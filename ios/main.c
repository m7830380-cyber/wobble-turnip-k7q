#include <time.h>
#include <math.h>
#include <dirent.h>
#include <libkern/OSCacheControl.h>
#include <sys/mman.h>
// iOS entry: content lives in Documents (user copies garrysmod/ sourceengine/ platform/ via Files).
// The app bundle only carries the Android .so code (lib/) and runtime assets (rt/).
#include "../loader/elf_loader.h"
#include "../shim/bionic.h"
#include "../shim/jni_fake.h"
#include "../shim/platform.h"
#include <SDL.h>
#include <SDL_main.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

char *ios_bundle_path(void);
char *ios_documents_path(void);

static char g_bundle[1024], g_docs[1024];
static void *g_gles, *g_egl;
static void *gl_proc(const char *n) {
  char p[1200];
  if (!g_gles) { snprintf(p, sizeof p, "%s/Frameworks/libGLESv2.framework/libGLESv2", g_bundle); g_gles = dlopen(p, RTLD_NOW | RTLD_GLOBAL); }
  if (!g_egl) { snprintf(p, sizeof p, "%s/Frameworks/libEGL.framework/libEGL", g_bundle); g_egl = dlopen(p, RTLD_NOW | RTLD_GLOBAL); }
  void *r = NULL;
  if (g_gles) r = dlsym(g_gles, n);
  if (!r && g_egl) r = dlsym(g_egl, n);
  return r;
}
static void stbits(void);

static void dinfo(const char*,int); static int g_sdiag = -2; static void halochk(int); static void halopost(int); static short g_tw[65536],g_th[65536];
#define SD(fmt, ...) do { if (g_sdiag >= 0 && g_sdiag < 6000) { g_sdiag++; fprintf(stderr, "STDIAG " fmt "\n", __VA_ARGS__); } } while (0)
static void (*real_blit)(int,int,int,int,int,int,int,int,unsigned,unsigned);
static void fix_blit(int a,int b,int c,int d,int e,int f,int g,int h,unsigned mask,unsigned filt) {
  static int n, calls; unsigned o = filt;
  if (calls++ < 3) fprintf(stderr, "BLITCALL filter 0x%x mask 0x%x\n", filt, mask);
  if (filt != 0x2600 && filt != 0x2601) filt = (filt == 2) ? 0x2601 : 0x2600; /* D3DTEXF: 1=POINT 2=LINEAR */
  if (mask & 0x500) filt = 0x2600;
  if (o != filt && n++ < 10) fprintf(stderr, "BLITFIX filter 0x%x -> 0x%x mask 0x%x %dx%d->%dx%d\n", o, filt, mask, c-a, d-b, g-e, h-f);
  
  SD("blit %dx%d->%dx%d", c-a, d-b, g-e, h-f); stbits();
  real_blit(a,b,c,d,e,f,g,h,mask,filt);
}

static void (*r_bf)(unsigned,unsigned),(*r_bfs)(unsigned,unsigned,unsigned,unsigned),(*r_be)(unsigned),(*r_bes)(unsigned,unsigned),(*r_vp)(int,int,int,int),(*r_up)(unsigned),(*r_de)(unsigned,int,unsigned,const void*),(*r_da)(unsigned,int,int),(*r_bfb)(unsigned,unsigned),(*r_ben)(unsigned);
static void w_bf(unsigned a,unsigned b){SD("blend 0x%x 0x%x",a,b);r_bf(a,b);}
static void w_bfs(unsigned a,unsigned b,unsigned c,unsigned d){SD("blendS 0x%x 0x%x 0x%x 0x%x",a,b,c,d);r_bfs(a,b,c,d);}
static void w_be(unsigned a){SD("beq 0x%x",a);r_be(a);}
static void w_bes(unsigned a,unsigned b){SD("beqS 0x%x 0x%x",a,b);r_bes(a,b);}
static void w_vp(int a,int b,int c,int d){SD("vp %d %d %d %d",a,b,c,d);r_vp(a,b,c,d);}
static void w_up(unsigned a){SD("prog %u",a);r_up(a);}
static void w_de(unsigned m,int c,unsigned t,const void*i){dinfo("dE",c);halochk(c);r_de(m,c,t,i);halopost(c);}
static void w_da(unsigned m,int f,int c){dinfo("dA",c);halochk(c);r_da(m,f,c);halopost(c);}
static void w_bfb(unsigned t,unsigned f){SD("bindfb 0x%x %u",t,f);r_bfb(t,f);}


void *ios_gl_proc(const char *n);
static void (*r_enable)(unsigned), (*r_disable)(unsigned), (*r_sfunc)(unsigned,int,unsigned), (*r_sop)(unsigned,unsigned,unsigned), (*r_smask)(unsigned), (*r_clear)(unsigned), (*r_cmask)(unsigned char,unsigned char,unsigned char,unsigned char);
static void (*r_sfs)(unsigned,unsigned,int,unsigned), (*r_sos)(unsigned,unsigned,unsigned,unsigned), (*r_sms)(unsigned,unsigned);
static void (*r_giv)(unsigned,int*);
static void w_sfs(unsigned f,unsigned fn,int r,unsigned m){ if(g_sdiag<0&&fn!=0x207) g_sdiag=0; SD("funcS 0x%x 0x%x ref %d mask 0x%x",f,fn,r,m); r_sfs(f,fn,r,m); }
static void w_sos(unsigned f,unsigned a,unsigned b,unsigned c){ SD("opS 0x%x 0x%x 0x%x 0x%x",f,a,b,c); r_sos(f,a,b,c); }
static void w_sms(unsigned f,unsigned m){ SD("wmaskS 0x%x 0x%x",f,m); r_sms(f,m); }
static void stbits(void){ if(!r_giv) r_giv=gl_proc("glGetIntegerv"); int fb=0,b=-1; r_giv(0x8CA6,&fb); r_giv(0x0D57,&b); SD("fbo %d stencilbits %d",fb,b); }
/* Upgrade depth-only buffers to depth+stencil so the halo stencil pass works. */
static unsigned char g_dsrb[65536], g_dstx[65536];
static int g_dsl = 0;
#define DSL(fmt, ...) do { if (g_dsl < 200) { g_dsl++; fprintf(stderr, "DSLOG " fmt "\n", __VA_ARGS__); } } while (0)
static void (*r_fgap)(unsigned,unsigned,unsigned,int*);
static void (*r_rbs)(unsigned,unsigned,int,int), (*r_rbsm)(unsigned,int,unsigned,int,int), (*r_frb)(unsigned,unsigned,unsigned,unsigned), (*r_ft2)(unsigned,unsigned,unsigned,unsigned,int);
static void (*r_ts2)(unsigned,int,unsigned,int,int); static void (*r_ti2)(unsigned,int,int,int,int,int,unsigned,unsigned,const void*);
static int isdepth(unsigned f){ return f==0x81A5||f==0x81A6||f==0x81A7||f==0x1902||f==0x8CAC; }
static int curb(unsigned q){ int v=0; if(!r_giv) r_giv=gl_proc("glGetIntegerv"); r_giv(q,&v); return v; }

static int g_dumped;
static void dumpfb(int fb,int w,int h,const char*nm){ void(*rp)(int,int,int,int,unsigned,unsigned,void*)=gl_proc("glReadPixels"); void(*bf)(unsigned,unsigned)=gl_proc("glBindFramebuffer"); int old=0; r_giv(0x8CAA,&old);
 bf(0x8CA8,fb); unsigned char*b=malloc((size_t)w*h*4); rp(0,0,w,h,0x1908,0x1401,b); bf(0x8CA8,old);
 char pth[1024]; snprintf(pth,sizeof pth,"%s/Documents/halo_%s_%dx%d.raw",getenv("HOME"),nm,w,h); FILE*f=fopen(pth,"wb"); if(f){fwrite(b,1,(size_t)w*h*4,f);fclose(f);} free(b); fprintf(stderr,"HALODUMP %s\n",pth); }
static void dumptex(int tx,const char*nm){ void(*gf)(int,unsigned*)=gl_proc("glGenFramebuffers"); void(*df)(int,unsigned*)=gl_proc("glDeleteFramebuffers"); void(*ft)(unsigned,unsigned,unsigned,unsigned,int)=gl_proc("glFramebufferTexture2D"); void(*bf)(unsigned,unsigned)=gl_proc("glBindFramebuffer"); void(*rp)(int,int,int,int,unsigned,unsigned,void*)=gl_proc("glReadPixels");
 int w=g_tw[tx],h=g_th[tx]; if(w<=0||h<=0){w=699;h=322;} int old=0; r_giv(0x8CAA,&old); unsigned f=0; gf(1,&f); bf(0x8CA8,f); ft(0x8CA8,0x8CE0,0xDE1,tx,0);
 unsigned char*b=malloc((size_t)w*h*4); rp(0,0,w,h,0x1908,0x1401,b); bf(0x8CA8,old); df(1,&f);
 char pth[1024]; snprintf(pth,sizeof pth,"%s/Documents/halo_%s_%dx%d.raw",getenv("HOME"),nm,w,h); FILE*fp=fopen(pth,"wb"); if(fp){fwrite(b,1,(size_t)w*h*4,fp);fclose(fp);} free(b); }
static int g_hframe;
static void halochk(int c){ if(g_dumped||c>24) return; int st=0,bl=0,tx=0,vp[4],fb=0,sf=0; r_giv(0x0B90,&st); r_giv(0x0BE2,&bl); r_giv(0x8069,&tx); r_giv(0x0BA2,vp); r_giv(0x8CA6,&fb); r_giv(0x0B92,&sf);
 if(!st||!bl||vp[2]<1000) return; if(++g_hframe<120) return; g_dumped=1; SD("HALOCAP fb %d tex %u sfunc 0x%x",fb,tx,sf); dumptex(tx,"blur"); dumpfb(fb,vp[2],vp[3],"pre"); }
static void halopost(int c){ if(g_dumped==1&&c<=24){ g_dumped=2; int fb=0,vp[4]; r_giv(0x8CA6,&fb); r_giv(0x0BA2,vp); dumpfb(fb,vp[2],vp[3],"post"); } }

static void dinfo(const char*k,int c){ if(g_sdiag<0||g_sdiag>=6000) return; if(!r_giv) r_giv=gl_proc("glGetIntegerv"); int fb=0,vp[4]={0},at=0,tx=0,ws=0,bl=0,sf[4]={0}; r_giv(0x8CA6,&fb); r_giv(0x0BA2,vp); r_giv(0x84E0,&at); r_giv(0x8069,&tx); r_giv(0x0BE2,&bl); r_giv(0x0B90,&ws);
 SD("%s %d fb %d vp %dx%d tex%d %u(%dx%d) blend %d st %d", k,c,fb,vp[2],vp[3],at-0x84C0,tx,tx<65536?g_tw[tx]:-1,tx<65536?g_th[tx]:-1,bl,ws); }
static void (*r_dre)(unsigned,unsigned,unsigned,int,unsigned,const void*),(*r_debv)(unsigned,int,unsigned,const void*,int),(*r_drebv)(unsigned,unsigned,unsigned,int,unsigned,const void*,int);
static void w_dre(unsigned m,unsigned a,unsigned b,int c,unsigned t,const void*i){dinfo("dRE",c);halochk(c);r_dre(m,a,b,c,t,i);halopost(c);}
static int g_hfix;
static int bloomchk(int c){ if(g_dumped!=2||c!=24) return 0; int tx=0,vp[4],fb=0; r_giv(0x8069,&tx); r_giv(0x0BA2,vp); if(vp[2]<1000||tx<=0||tx>=65536||g_tw[tx]>=1000) return 0; g_dumped=3; r_giv(0x8CA6,&fb); dumpfb(fb,vp[2],vp[3],"bloom_pre"); fprintf(stderr,"[halo] bloom dump tex %d %dx%d\n",tx,g_tw[tx],g_th[tx]); return 1; }
static int halofill_pre(int c,float*sv,int*loc){ if(c!=6) return 0; if(!r_giv) r_giv=gl_proc("glGetIntegerv"); int st=0,bl=0,tx=0,pr=0; r_giv(0x0B90,&st); if(!st) return 0; r_giv(0x0BE2,&bl); r_giv(0x8069,&tx); if(!bl||tx<=0||tx>=65536||g_tw[tx]<1000) return 0; int bs=0,bd=0; r_giv(0x80C9,&bs); r_giv(0x80C8,&bd); if(bs!=1||bd!=1) return 0; r_giv(0x8B8D,&pr); if(!pr) return 0;
 static int(*gul)(unsigned,const char*); static void(*guf)(unsigned,int,float*); static void(*u1f)(int,float); if(!gul){gul=gl_proc("glGetUniformLocation");guf=gl_proc("glGetUniformfv");u1f=gl_proc("glUniform1f");}
 int l=gul(pr,"flSRGBWrite"); if(l<0) return 0; guf(pr,l,sv); if(*sv==0.0f) return 0; u1f(l,0.0f); *loc=l; if(g_hfix++<3) fprintf(stderr,"[halo] composite prog %d srgbwrite %.1f -> 0\n",pr,*sv); return 1; }
static void halofill_post(int l,float v){ static void(*u1f)(int,float); if(!u1f) u1f=gl_proc("glUniform1f"); u1f(l,v); }
static void (*r_shsrc)(unsigned,int,const char*const*,const int*); static unsigned char g_blsh[65536],g_blpr[65536];
static int g_pcl; static void blurlog(int c){ if(c!=6||g_pcl>=80) return; if(!r_giv) r_giv=gl_proc("glGetIntegerv"); int vp[4],tx=0,pr=0,st=0,bs=0,bd=0,bl=0,fb=0,eq=0,bsa=0,bda=0; r_giv(0x0BA2,vp); r_giv(0x8069,&tx); r_giv(0x8B8D,&pr); r_giv(0x0B90,&st); r_giv(0x0BE2,&bl); r_giv(0x80C9,&bs); r_giv(0x80C8,&bd); r_giv(0x80CB,&bsa); r_giv(0x80CA,&bda); r_giv(0x8009,&eq); r_giv(0x8CA6,&fb); if(!pr||tx<=0||tx>=65536) return; int blur=pr<65536&&g_blpr[pr]; int comp=st&&bl&&bs==1&&bd==1; if(!blur&&!comp) return;
 static int(*gul)(unsigned,const char*); static void(*guf)(unsigned,int,float*); static void(*gtp)(unsigned,unsigned,int*); if(!gul){gul=gl_proc("glGetUniformLocation");guf=gl_proc("glGetUniformfv");gtp=gl_proc("glGetTexParameteriv");}
 int mn=0,mg=0,ws=0; gtp(0x0DE1,0x2801,&mn); gtp(0x0DE1,0x2800,&mg); gtp(0x0DE1,0x2802,&ws);
 g_pcl++; fprintf(stderr,"[blur3] %s prog %d fb %d vp %d,%d %dx%d tex %d %dx%d filt %x/%x wrap %x blend %d %x %x a %x %x eq %x",comp?"COMP":"BLUR",pr,fb,vp[0],vp[1],vp[2],vp[3],tx,g_tw[tx],g_th[tx],mn,mg,ws,bl,bs,bd,bsa,bda,eq);
 for(int k=0;k<4;k++){ char nm[16]; snprintf(nm,sizeof nm,"pc[%d]",k); int l=gul(pr,nm); if(l<0) break; float v[4]={0}; guf(pr,l,v); fprintf(stderr," pc%d(%g %g %g %g)",k,v[0],v[1],v[2],v[3]); }
 int l=gul(pr,"flSRGBWrite"); if(l>=0){ float v=0; guf(pr,l,&v); fprintf(stderr," srgbw %g",v);} fprintf(stderr,"\n"); }
static int haloblur_pre(int c,float*sv,int*loc){ if(c!=6) return 0; int pr=0,tx=0,vp[4]; r_giv(0x8B8D,&pr); if(pr<=0||pr>=65536||!g_blpr[pr]) return 0; r_giv(0x8069,&tx); r_giv(0x0BA2,vp); if(tx<=0||tx>=65536) return 0; int th=g_th[tx],tw=g_tw[tx]; if(th<=1||vp[3]<=th) return 0;
 static int(*gul)(unsigned,const char*); static void(*guf)(unsigned,int,float*); static void(*u4)(int,int,const float*); if(!gul){gul=gl_proc("glGetUniformLocation");guf=gl_proc("glGetUniformfv");u4=gl_proc("glUniform4fv");}
 int l=gul(pr,"pc[0]"); if(l<0) l=gul(pr,"pc"); if(l<0) return 0; guf(pr,l,sv); float nv[4]={sv[0]*tw/(float)vp[2],sv[1]*th/(float)vp[3],sv[2],sv[3]}; u4(l,1,nv); *loc=l; static int lg; if(lg++<6) fprintf(stderr,"[halo] blur offset %g,%g -> %g,%g (src %dx%d vp %dx%d)\n",sv[0],sv[1],nv[0],nv[1],tw,th,vp[2],vp[3]); return 1; }
static void haloblur_post(int l,float*sv){ static void(*u4)(int,int,const float*); if(!u4) u4=gl_proc("glUniform4fv"); u4(l,1,sv); }
static void w_debv(unsigned m,int c,unsigned t,const void*i,int bv){dinfo("dEBV",c);blurlog(c);halochk(c);float sv=0;int hl=-1;int hf=halofill_pre(c,&sv,&hl);int bm=bloomchk(c);float bsv[4]={0};int bl=-1;int hb=haloblur_pre(c,bsv,&bl);r_debv(m,c,t,i,bv);if(hb)haloblur_post(bl,bsv);if(hf)halofill_post(hl,sv);halopost(c);if(bm){int fb=0;r_giv(0x8CA6,&fb);dumpfb(fb,2796,1290,"bloom_post");}}
static void w_drebv(unsigned m,unsigned a,unsigned b,int c,unsigned t,const void*i,int bv){dinfo("dREBV",c);halochk(c);r_drebv(m,a,b,c,t,i,bv);halopost(c);}static int g_srgbfix=-1; static int srgbfix(void){ if(g_srgbfix<0){ const char*e=getenv("GMOD_SRGB_FIX"); g_srgbfix=!(e&&*e=='0'); } return g_srgbfix; }
static void w_rbs(unsigned t,unsigned f,int w,int h){ if(f==0x8C43&&srgbfix()) f=0x8058; DSL("rbs fmt 0x%x %dx%d",f,w,h); if(isdepth(f)){ DSL("rb depth 0x%x -> DS %dx%d",f,w,h); f=0x88F0; int b=curb(0x8CA7); if(b>=0&&b<65536) g_dsrb[b]=1; } else if(f==0x88F0||f==0x8CAD){ int b=curb(0x8CA7); if(b>=0&&b<65536) g_dsrb[b]=1; } r_rbs(t,f,w,h); }
static void w_rbsm(unsigned t,int n,unsigned f,int w,int h){ if(f==0x8C43&&srgbfix()) f=0x8058; DSL("rbsm n%d fmt 0x%x %dx%d",n,f,w,h); if(isdepth(f)) f=0x88F0; if(f==0x88F0||f==0x8CAD){ int b=curb(0x8CA7); if(b>=0&&b<65536) g_dsrb[b]=1; } r_rbsm(t,n,f,w,h); }
static void w_frb(unsigned t,unsigned a,unsigned rt,unsigned rb){ DSL("frb att 0x%x rb %u ds%d",a,rb,rb<65536?g_dsrb[rb]:-1); if(a==0x8D00&&rb<65536&&g_dsrb[rb]){ a=0x821A;  } r_frb(t,a,rt,rb); }
static void w_ts2(unsigned t,int l,unsigned f,int w,int h){ if(!l){ int b=curb(0x8069); if(b>=0&&b<65536){g_tw[b]=w;g_th[b]=h;} } if(f==0x8C43||f==0x8C41) DSL("ts2 srgb 0x%x %dx%d",f,w,h); if(f<0x8000||isdepth(f)||f==0x88F0||f==0x8CAD) DSL("ts2 fmt 0x%x %dx%d",f,w,h); if(isdepth(f)){ f=0x88F0; int b=curb(0x8069); if(b>=0&&b<65536) g_dstx[b]=1;  } if(f==0x88F0||f==0x8CAD){ int b=curb(0x8069); if(b>=0&&b<65536) g_dstx[b]=1; } r_ts2(t,l,f,w,h); }
static void w_ti2(unsigned t,int l,int f,int w,int h,int bd,unsigned fm,unsigned ty,const void*p){ if(!l){ int b=curb(0x8069); if(b>=0&&b<65536){g_tw[b]=w;g_th[b]=h;} } if((f==0x8C43||f==0x8C41)&&!p&&srgbfix()){ DSL("srgb rt 0x%x -> rgba8 %dx%d",f,w,h); f=(f==0x8C43)?0x8058:0x8051; } if(fm==0x1902||fm==0x84F9) DSL("ti2 if 0x%x fm 0x%x ty 0x%x %dx%d",f,fm,ty,w,h); if(fm==0x1902&&isdepth(f)){ f=0x88F0; fm=0x84F9; ty=0x84FA; p=NULL; int b=curb(0x8069); if(b>=0&&b<65536) g_dstx[b]=1; DSL("teximg depth -> DS %dx%d",w,h); } if(f==0x88F0||f==0x8CAD){ int b=curb(0x8069); if(b>=0&&b<65536) g_dstx[b]=1; } r_ti2(t,l,f,w,h,bd,fm,ty,p); }
static void w_ft2(unsigned t,unsigned a,unsigned tt,unsigned tx,int l){ if(a!=0x8CE0) DSL("ft2 att 0x%x tex %u ds%d",a,tx,tx<65536?g_dstx[tx]:-1); if(a==0x8D00&&tx<65536&&g_dstx[tx]) a=0x821A; r_ft2(t,a,tt,tx,l); }
static void w_enable(unsigned c) { if (c == 0xBE2) SD("enable blend%s","");  if (c == 0xB90) {  SD("enable stencil%s", ""); stbits(); } r_enable(c); }
static void w_disable(unsigned c) { if (c == 0xBE2) SD("disable blend%s","");  if (c == 0xB90) SD("disable stencil%s", ""); r_disable(c); }
static void w_sfunc(unsigned f, int r, unsigned m) { if(g_sdiag<0&&f!=0x207) g_sdiag=0; SD("func 0x%x ref %d mask 0x%x", f, r, m); r_sfunc(f, r, m); }
static void w_sop(unsigned a, unsigned b, unsigned c) { SD("op 0x%x 0x%x 0x%x", a, b, c); r_sop(a, b, c); }
static void w_smask(unsigned m) { SD("wmask 0x%x", m); r_smask(m); }
static void w_clear(unsigned m) { SD("clear 0x%x", m); r_clear(m); }
static void w_cmask(unsigned a, unsigned b, unsigned c, unsigned d) { a&=0xFF; b&=0xFF; c&=0xFF; d&=0xFF; SD("cmask %d%d%d%d", a, b, c, d); r_cmask(a, b, c, d); }
/* AAPCS64 (Android) leaves upper bits of bool/char args undefined; Darwin callees assume extended. */
static void (*r_dmask)(unsigned char); static void w_dmask(unsigned a) { r_dmask(a & 0xFF); }
static void (*r_vap)(unsigned,int,unsigned,unsigned char,int,const void*); static void w_vap(unsigned i,int n,unsigned t,unsigned nm,int st,const void*p){ r_vap(i,n,t,nm&0xFF,st,p); }
static void (*r_scov)(float,unsigned char); static void w_scov(float v,unsigned x){ r_scov(v,x&0xFF); }
#define UM(N) static void (*r_##N)(int,int,unsigned char,const float*); static void w_##N(int l,int c,unsigned t,const float*v){ r_##N(l,c,t&0xFF,v); }
UM(um2) UM(um3) UM(um4) UM(um23) UM(um32) UM(um24) UM(um42) UM(um34) UM(um43)
static FILE*g_shf; static int g_shn;
static void w_shsrc(unsigned sh,int n,const char*const*str,const int*len){
  if(!g_shf){ char pth[1024]; snprintf(pth,sizeof pth,"%s/Documents/gmod_shaders.txt",getenv("HOME")); g_shf=fopen(pth,"w"); }
  if(g_shf && g_shn<4000){ g_shn++; fprintf(g_shf,"\n===== SHADER %u =====\n",sh); for(int i=0;i<n;i++){ if(len&&len[i]>=0) fwrite(str[i],1,len[i],g_shf); else fputs(str[i],g_shf);} fflush(g_shf); }
  for(int i=0;i<n&&sh<65536;i++) if(str[i]&&strstr(str[i],"gmodvr_blur_ps")) g_blsh[sh]=1;
  r_shsrc(sh,n,str,len); }
static void (*r_attsh)(unsigned,unsigned);
static void w_attsh(unsigned p,unsigned sh){ if(p<65536&&sh<65536&&g_blsh[sh]) g_blpr[p]=1; if(g_shf){ fprintf(g_shf,"\n##### ATTACH prog %u shader %u\n",p,sh); fflush(g_shf);} r_attsh(p,sh); }

static void *my_eglGetProcAddress(const char *n) { return ios_gl_proc(n); }
void *ios_gl_proc(const char *n) {
  if (!strcmp(n, "eglGetProcAddress")) return (void*)my_eglGetProcAddress;
#define W(nm, r, w) if (!strcmp(n, nm)) { if (!r) r = gl_proc(nm); return r ? (void*)w : NULL; }
  W("glShaderSource", r_shsrc, w_shsrc) W("glAttachShader", r_attsh, w_attsh) W("glDrawRangeElements", r_dre, w_dre) W("glDrawElementsBaseVertex", r_debv, w_debv) W("glDrawElementsBaseVertexEXT", r_debv, w_debv) W("glDrawElementsBaseVertexOES", r_debv, w_debv) W("glDrawRangeElementsBaseVertex", r_drebv, w_drebv) W("glDrawRangeElementsBaseVertexEXT", r_drebv, w_drebv) W("glDrawRangeElementsBaseVertexOES", r_drebv, w_drebv) W("glBlendFunc", r_bf, w_bf) W("glBlendFuncSeparate", r_bfs, w_bfs) W("glBlendEquation", r_be, w_be) W("glBlendEquationSeparate", r_bes, w_bes) W("glViewport", r_vp, w_vp) W("glUseProgram", r_up, w_up) W("glDrawElements", r_de, w_de) W("glDrawArrays", r_da, w_da) W("glBindFramebuffer", r_bfb, w_bfb) W("glEnable", r_enable, w_enable) W("glDisable", r_disable, w_disable) W("glStencilFunc", r_sfunc, w_sfunc)
  W("glStencilOp", r_sop, w_sop) W("glStencilMask", r_smask, w_smask) W("glClear", r_clear, w_clear) W("glColorMask", r_cmask, w_cmask) W("glRenderbufferStorage", r_rbs, w_rbs) W("glRenderbufferStorageMultisample", r_rbsm, w_rbsm)
  W("glFramebufferRenderbuffer", r_frb, w_frb) W("glTexStorage2D", r_ts2, w_ts2) W("glTexImage2D", r_ti2, w_ti2) W("glFramebufferTexture2D", r_ft2, w_ft2) W("glStencilFuncSeparate", r_sfs, w_sfs) W("glStencilOpSeparate", r_sos, w_sos) W("glStencilMaskSeparate", r_sms, w_sms) W("glDepthMask", r_dmask, w_dmask) W("glVertexAttribPointer", r_vap, w_vap) W("glSampleCoverage", r_scov, w_scov)
  W("glUniformMatrix2fv", r_um2, w_um2) W("glUniformMatrix3fv", r_um3, w_um3) W("glUniformMatrix4fv", r_um4, w_um4)
  W("glUniformMatrix2x3fv", r_um23, w_um23) W("glUniformMatrix3x2fv", r_um32, w_um32) W("glUniformMatrix2x4fv", r_um24, w_um24)
  W("glUniformMatrix4x2fv", r_um42, w_um42) W("glUniformMatrix3x4fv", r_um34, w_um34) W("glUniformMatrix4x3fv", r_um43, w_um43)
  if (!strcmp(n, "glBlitFramebuffer") || !strcmp(n, "glBlitFramebufferANGLE") || !strcmp(n, "glBlitFramebufferEXT")) {
    if (!real_blit) real_blit = gl_proc("glBlitFramebuffer");
    return real_blit ? (void*)fix_blit : NULL;
  }
  return gl_proc(n);
}
void *glctx_resolve(const char *n); void *glctx_resolve3(const char *n);
static int lib_private(const char *n) {
  // libraries statically linked into the .so files (FreeType etc.). Must bind to the module's own copy,
  // never to a same-named symbol that happens to exist in an iOS framework (different version/ABI -> hangs)
  static const char *pre[] = {"FT_", "FTC_", "TT_", "T1_", "ft_", "tt_", "af_", "ps_", "cff_", "hb_", "png_", "jpeg_", "ogg_", "vorbis_", "opus_", "speex_", "mpg123_", "lua_", "luaL_", "luaJIT_", "ZSTD_", "LZ4_", "XXH", 0};
  for (int i = 0; pre[i]; i++) if (!strncmp(n, pre[i], strlen(pre[i]))) return 1;
  return 0;
}
static int g_msgboxes;
static int my_ShowSimpleMessageBox(unsigned flags, const char *title, const char *msg, void *win) {
  (void)win; fprintf(stderr, "\n*** MESSAGEBOX [%u] %s: %s\n", flags, title ? title : "", msg ? msg : "");
  if (++g_msgboxes > 3) return 0;
  return SDL_ShowSimpleMessageBox(flags, title, msg, NULL);
}
/* Noclip: GDroid's CGameMovement::FullNoClipMove (libclient 0x6084d4 / libserver 0x6a2828, 0.1.31) is the HL2/SDK one
   (wishspeed = maxspeed * sv_noclipspeed, with acceleration + upmove). Retail GMod (PC server_srv.so 0x592cb0) does:
   wish = norm(norm(fwd*fm) + norm(right*sm) + (0,0,jump)); vel = wish * sv_noclipspeed * 100 (x3 IN_SPEED, x0.1 IN_DUCK);
   origin += vel * frametime. On a PC server the phone's prediction flew faster than the server -> snapped back.
   Replace both with the retail version. mv offsets (arm64): viewangles +0x14, buttons +44, fm +52, sm +56, vel +72, origin +200. */
static void (*nc_anglevec[2])(const float *, float *, float *, float *);
static void ***nc_globals[2];
static void nc_move(int w, void *gm, float factor) {
  char *mv = *(char **)((char *)gm + 32); float f[3], r[3], u[3];
  nc_anglevec[w]((float *)(mv + 0x14), f, r, u);
  int bt = *(int *)(mv + 44); float fm = *(float *)(mv + 52), sm = *(float *)(mv + 56), a[3], b[3], c[3] = {0, 0, (bt & 2) ? 1.f : 0.f}, wv[3];
  for (int i = 0; i < 3; i++) { a[i] = f[i] * fm; b[i] = r[i] * sm; }
  float la = 1.f / sqrtf(a[0]*a[0] + a[1]*a[1] + a[2]*a[2] + 1e-10f), lb = 1.f / sqrtf(b[0]*b[0] + b[1]*b[1] + b[2]*b[2] + 1e-10f), lc = 1.f / sqrtf(c[2]*c[2] + 1e-10f);
  for (int i = 0; i < 3; i++) wv[i] = a[i]*la + b[i]*lb + c[i]*lc;
  float lw = 1.f / sqrtf(wv[0]*wv[0] + wv[1]*wv[1] + wv[2]*wv[2] + 1e-10f), sp = factor * 100.f;
  if (bt & 0x20000) sp *= 3.f; if (bt & 4) sp *= 0.1f;
  float ft = *(float *)((char *)**nc_globals[w] + 16), *vel = (float *)(mv + 72), *org = (float *)(mv + 200);
  for (int i = 0; i < 3; i++) { vel[i] = wv[i] * lw * sp; org[i] += vel[i] * ft; }
}
static double nc_now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }
static double nc_seen;  /* last time client prediction ran noclip movement */
static void nc_cl(void *gm, float factor, float accel) { (void)accel; nc_seen = nc_now(); nc_move(0, gm, factor); }
/* Retail noclip normalizes fwd*fm and right*sm separately, so any stick angle becomes one of 8 directions (feels like WASD).
   While noclipping, dither each usercmd between the two nearest 45-degree directions (error diffusion), so the average
   path follows the joystick. Server and prediction both see the same cmds -> no rubber-banding.
   ClientModeShared::CreateMove(this, frametime, CUserCmd *) wrapper libclient 0x5b8d1c; cmd forwardmove +28, sidemove +32. */
static int (*nc_cm_orig)(void *, float, void *);
static int nc_cm(void *self, float ft, void *cmd) {
  int r = nc_cm_orig(self, ft, cmd);
  if (cmd && nc_now() - nc_seen < 0.25) {
    float *fm = (float *)((char *)cmd + 28), *sm = (float *)((char *)cmd + 32), mag = sqrtf(*fm * *fm + *sm * *sm);
    if (mag > 1.f) {
      static float acc; const float q = (float)M_PI / 4.f;
      float th = atan2f(*sm, *fm) / q, k = floorf(th), t = th - k;
      acc += t; float d = k; if (acc >= 1.f) { acc -= 1.f; d = k + 1.f; }
      float c = cosf(d * q), sn = sinf(d * q);
      *fm = fabsf(c) < 0.01f ? 0.f : c * mag; *sm = fabsf(sn) < 0.01f ? 0.f : sn * mag;
    }
  }
  return r;
}
static void nc_cm_patch(void) {
  so_module *mm = so_find("libclient.so"); if (!mm) return;
  uint32_t *fnc = (uint32_t *)(mm->base + 0x5b8d1c);
  if (fnc[0] != 0xfc1e0fe8 || fnc[1] != 0xa900fbfd) { fprintf(stderr, "[noclip] CreateMove: unexpected %08x\n", fnc[0]); return; }
  nc_cm_orig = (void *)fnc; int n = 0;
  uintptr_t slots[2] = {0xe0d110, 0xe9f038};
  for (int i = 0; i < 2; i++) { void **sl = (void **)(mm->base + slots[i]);
    if (*sl != (void *)fnc) { fprintf(stderr, "[noclip] vtable slot %lx = %p, skipped\n", (unsigned long)slots[i], *sl); continue; }
    uintptr_t pg = (uintptr_t)sl & ~(uintptr_t)16383; mprotect((void *)pg, 16384, PROT_READ | PROT_WRITE);
    *sl = (void *)nc_cm; n++; }
  fprintf(stderr, "[noclip] joystick any-direction dither on (%d slots)\n", n);
}

static void nc_sv(void *gm, float factor, float accel) { (void)accel; nc_move(1, gm, factor); }
static void nc_patch(const char *lib, uintptr_t fo, uintptr_t av, uintptr_t gl, int w, void *fnp) {
  so_module *mm = so_find(lib); if (!mm) { fprintf(stderr, "[noclip] %s not loaded\n", lib); return; }
  uint32_t *ins = (uint32_t *)(mm->base + fo);
  if (ins[0] != 0xd10243ff || ins[1] != 0x6d0433ed) { fprintf(stderr, "[noclip] %s: unexpected %08x %08x\n", lib, ins[0], ins[1]); return; }
  nc_anglevec[w] = (void *)(mm->base + av); nc_globals[w] = (void ***)(mm->base + gl);
  uint32_t t[4] = {0x58000050, 0xd61f0200, (uint32_t)(uintptr_t)fnp, (uint32_t)((uintptr_t)fnp >> 32)};
  extern void *so_wptr(void *); extern int so_is_mirrored(void *); int a = 0, b = 0;
  if (so_is_mirrored(ins)) memcpy(so_wptr(ins), t, 16);
  else { uintptr_t pg = (uintptr_t)ins & ~(uintptr_t)16383; a = mprotect((void *)pg, 32768, PROT_READ | PROT_WRITE); memcpy(ins, t, 16); b = mprotect((void *)pg, 32768, PROT_READ | PROT_EXEC); }
  sys_icache_invalidate(ins, 16); fprintf(stderr, "[noclip] %s: retail GMod noclip movement (%d %d)\n", lib, a, b);
}
/* libengine 0.1.31 netadr_t::IsReservedAdr (0x5d179c): what a sv_lan 1 server lets in. Add Tailscale/CGNAT 100.64.0.0/10
   so friends on the same tailnet count as LAN. netadr_t: int type (1 loopback, 3 ip) | uint8 ip[4] | port */
static int ts_reserved(const uint8_t *a) {
  int t = *(const int *)a; if (t == 1) return 1; if (t != 3) return 0; const uint8_t *ip = a + 4;
  return ip[0] == 10 || ip[0] == 127 || (ip[0] == 172 && (ip[1] & 0xf0) == 16) || (ip[0] == 192 && ip[1] == 168) ||
         (ip[0] == 100 && (ip[1] & 0xc0) == 64);
}
static void ts_patch(void) {
  so_module *e = so_find("libengine.so"); if (!e) return;
  uint32_t *ins = (uint32_t *)(e->base + 0x5d179c);
  if (ins[0] != 0xb9400008 || ins[1] != 0x7100051f) { fprintf(stderr, "[tailscale] lan check: unexpected %08x\n", ins[0]); return; }
  uint32_t t[4] = {0x58000050, 0xd61f0200, (uint32_t)(uintptr_t)ts_reserved, (uint32_t)((uintptr_t)ts_reserved >> 32)};
  extern void *so_wptr(void *); extern int so_is_mirrored(void *); int a = 0, b = 0;
  if (so_is_mirrored(ins)) memcpy(so_wptr(ins), t, 16);
  else { uintptr_t pg = (uintptr_t)ins & ~(uintptr_t)16383; a = mprotect((void *)pg, 32768, PROT_READ | PROT_WRITE); memcpy(ins, t, 16); b = mprotect((void *)pg, 32768, PROT_READ | PROT_EXEC); }
  sys_icache_invalidate(ins, 16); fprintf(stderr, "[tailscale] LAN servers accept 100.64.0.0/10 (%d %d)\n", a, b);
}
/* libtogl 0.1.31 (path 2 = this GPU has no native S3TC): every DXT1 texture is transcoded to ETC2 RGB8 (0x9274), which has
   NO alpha. D3D9 DXT1 has 1-bit alpha ($alphatest sprites, cutouts, decals) -> the transparent texels became BLACK.
   The DXT1 branches of the upload function (vaddr 0xa2cb4 linear / 0xa2cc4 sRGB, 4 insns each) now jump to dxt1_stub:
   if the mip really uses transparent texels, upload it through the RGBA8 CPU decode (0xa2d70) as 0x83f1 / 0x8c4d
   (DXT1 with alpha); else keep ETC2. Regs there: x27 data, w25 width, w20 height, x8 bytes, w26 GL format. */
uintptr_t g_dxt1_etc2, g_dxt1_dec; int g_dxt1_n;
int dxt1_has_alpha(const uint8_t *d, int w, int h, int64_t n) {
  if (!d || w < 1 || h < 1) return 0;
  int64_t bl = (int64_t)((w + 3) / 4) * ((h + 3) / 4) * 8; if (n > 0 && n < bl) bl = n;
  for (int64_t o = 0; o + 8 <= bl; o += 8) {
    uint16_t c0 = d[o] | d[o + 1] << 8, c1 = d[o + 2] | d[o + 3] << 8; if (c0 > c1) continue;
    uint32_t ix = d[o + 4] | d[o + 5] << 8 | d[o + 6] << 16 | (uint32_t)d[o + 7] << 24;
    for (int k = 0; k < 16; k++) if (((ix >> (2 * k)) & 3) == 3) { if (g_dxt1_n++ < 8) fprintf(stderr, "[dxt1] %dx%d has 1-bit alpha -> RGBA8 decode\n", w, h); return 1; }
  }
  return 0;
}
#define DXT1_STUB(name, srgb, fmt) \
__asm__(".text\n.p2align 2\n.globl _" #name "\n_" #name ":\n" \
  "stp x29, x30, [sp, #-160]!\n stp x0, x1, [sp, #16]\n stp x2, x3, [sp, #32]\n stp x4, x5, [sp, #48]\n stp x6, x7, [sp, #64]\n" \
  "stp x8, x9, [sp, #80]\n stp x10, x11, [sp, #96]\n stp x12, x13, [sp, #112]\n stp x14, x15, [sp, #128]\n" \
  "mov x0, x27\n mov w1, w25\n mov w2, w20\n mov x3, x8\n bl _dxt1_has_alpha\n mov w17, w0\n" \
  "ldp x0, x1, [sp, #16]\n ldp x2, x3, [sp, #32]\n ldp x4, x5, [sp, #48]\n ldp x6, x7, [sp, #64]\n" \
  "ldp x8, x9, [sp, #80]\n ldp x10, x11, [sp, #96]\n ldp x12, x13, [sp, #112]\n ldp x14, x15, [sp, #128]\n ldp x29, x30, [sp], #160\n" \
  "cbz w17, 1f\n mov w26, #" #fmt "\n adrp x16, _g_dxt1_dec@PAGE\n ldr x16, [x16, _g_dxt1_dec@PAGEOFF]\n br x16\n" \
  "1:\n mov w9, #" #srgb "\n mov w16, #0\n mov w15, #1\n adrp x17, _g_dxt1_etc2@PAGE\n ldr x17, [x17, _g_dxt1_etc2@PAGEOFF]\n br x17\n");
DXT1_STUB(dxt1_stub_lin, 0, 0x83f1)
DXT1_STUB(dxt1_stub_srgb, 1, 0x8c4d)
extern char dxt1_stub_lin[], dxt1_stub_srgb[];
static void dxt1_patch(void) {
  so_module *tg = so_find("libtogl.so"); if (!tg) return;
  uint32_t *a = (uint32_t *)(tg->base + 0xa2cb4), *b = (uint32_t *)(tg->base + 0xa2cc4);
  if (a[0] != 0x2a1f03e9 || a[3] != 0x1400000b || b[0] != 0x2a1f03f0 || b[3] != 0x14000007) { fprintf(stderr, "[dxt1] unexpected %08x %08x\n", a[0], b[0]); return; }
  g_dxt1_etc2 = (uintptr_t)tg->base + 0xa2cec; g_dxt1_dec = (uintptr_t)tg->base + 0xa2d70;
  extern void *so_wptr(void *); extern int so_is_mirrored(void *);
  uint32_t ta[4] = {0x58000050, 0xd61f0200, (uint32_t)(uintptr_t)dxt1_stub_lin, (uint32_t)((uintptr_t)dxt1_stub_lin >> 32)};
  uint32_t tb[4] = {0x58000050, 0xd61f0200, (uint32_t)(uintptr_t)dxt1_stub_srgb, (uint32_t)((uintptr_t)dxt1_stub_srgb >> 32)};
  int r1 = 0, r2 = 0;
  if (so_is_mirrored(a)) { memcpy(so_wptr(a), ta, 16); memcpy(so_wptr(b), tb, 16); }
  else { uintptr_t pg = (uintptr_t)a & ~(uintptr_t)16383; r1 = mprotect((void *)pg, 32768, PROT_READ | PROT_WRITE);
    memcpy(a, ta, 16); memcpy(b, tb, 16); r2 = mprotect((void *)pg, 32768, PROT_READ | PROT_EXEC); }
  sys_icache_invalidate(a, 32); fprintf(stderr, "[dxt1] DXT1 with 1-bit alpha -> RGBA8 instead of ETC2 RGB (%d %d)\n", r1, r2);
}
/* libtogl 0.1.31: a shader the D3D->GLSL translator can't handle calls tier0 Error() = game exits (Viewfinder's VHS
   signal shaders on "Linux"). Turn the 3 translator Error() calls into Warning(): the shader then just fails to
   compile/link and addons that probe their output (Viewfinder) fall back on their own. */
static void togl_soft_patch(void) {
  so_module *tg = so_find("libtogl.so"); if (!tg) return;
  static const uint32_t at[3] = {0x52760, 0x554f0, 0x56d28}, was[3] = {0x94026124, 0x940255c0, 0x94024fb2};
  extern void *so_wptr(void *); extern int so_is_mirrored(void *); int n = 0;
  for (int i = 0; i < 3; i++) {
    uint32_t *ins = (uint32_t *)(tg->base + at[i]);
    if (*ins != was[i]) { fprintf(stderr, "[togl] translate-error site %x unexpected %08x\n", at[i], *ins); continue; }
    uint32_t nw = was[i] + 0x40;  /* bl Error@plt (0xeabf0) -> bl Warning@plt (0xeacf0) */
    if (so_is_mirrored(ins)) *(uint32_t *)so_wptr(ins) = nw;
    else { uintptr_t pg = (uintptr_t)ins & ~(uintptr_t)16383; mprotect((void *)pg, 16384, PROT_READ | PROT_WRITE); *ins = nw; mprotect((void *)pg, 16384, PROT_READ | PROT_EXEC); }
    sys_icache_invalidate(ins, 4); n++;
  }
  fprintf(stderr, "[togl] shader translation errors are non-fatal now (%d/3)\n", n);
}
static int not_debugging(void) { return 0; }
/* LuaJIT machine code crashes on iOS (debugger freezes the faulting thread) -> interpreter only */
static void *lua_real(const char *n) { so_module *m = so_find("liblua_shared.so"); return m ? so_sym(m, n) : NULL; }
static void lua_patch_once(void) {
  static int done; if (done) return; done = 1;
  so_module *m = so_find("liblua_shared.so"); if (!m) { fprintf(stderr, "[lua] patch: module not found\n"); return; }
  uint64_t *c = (uint64_t *)(m->base + 0x26d468); uintptr_t pg = (uintptr_t)c & ~(uintptr_t)16383;
  int r = mprotect((void *)pg, 16384, PROT_READ | PROT_WRITE);
  fprintf(stderr, "[lua] ctr cache was %llx (mprotect %d)\n", (unsigned long long)*c, r); *c = 0x8444c004ULL;
  /* libclient/libserver are dlopen'ed by the engine later than so_init_all, so patch here (first Lua state) */
  nc_patch("libclient.so", 0x6084d4, 0xbde834, 0xfbe678, 0, (void *)nc_cl);
  nc_patch("libserver.so", 0x6a2828, 0xd2813c, 0x1007450, 1, (void *)nc_sv);
  nc_cm_patch();
  ts_patch();
  so_module *cl = so_find("libclient.so");
  if (cl) { uint32_t *ins = (uint32_t *)(cl->base + 0x9b6d90); uintptr_t p2 = (uintptr_t)ins & ~(uintptr_t)16383;
    extern void *so_wptr(void *); extern int so_is_mirrored(void *);
    if (*ins == 0x36000360) { int a = 0, b = 0;
      if (so_is_mirrored(ins)) *(uint32_t *)so_wptr(ins) = 0x1400001b;
      else { a = mprotect((void *)p2, 16384, PROT_READ | PROT_WRITE); *ins = 0x1400001b;
             b = mprotect((void *)p2, 16384, PROT_READ | PROT_EXEC); }
      sys_icache_invalidate(ins, 4);
      fprintf(stderr, "[halo] transparent-entity refusal patched (%d %d)\n", a, b); }
    else fprintf(stderr, "[halo] patch: unexpected insn %08x\n", *ins);
    /* native physgun halo refuses held models with any translucent material (e.g. car glass:
       "Model material models/combine_apc/glass (translucent=1 ...)") -> stock full-screen blur, 60 -> 23 fps.
       Skip only the translucent test (tbnz w0,#0 -> nop); fullframe / power-of-two checks stay. */
    uint32_t *t2 = (uint32_t *)(cl->base + 0x9b7024); uintptr_t p3 = (uintptr_t)t2 & ~(uintptr_t)16383;
    if (*t2 == 0x370002e0) { int a = 0, b = 0;
      if (so_is_mirrored(t2)) *(uint32_t *)so_wptr(t2) = 0xd503201f;
      else { a = mprotect((void *)p3, 16384, PROT_READ | PROT_WRITE); *t2 = 0xd503201f;
             b = mprotect((void *)p3, 16384, PROT_READ | PROT_EXEC); }
      sys_icache_invalidate(t2, 4);
      fprintf(stderr, "[halo] translucent-material refusal patched (%d %d)\n", a, b); }
    else fprintf(stderr, "[halo] translucent patch: unexpected insn %08x\n", *t2); }
}
static void *my_luaL_newstate(void) { lua_patch_once();
  void *(*f)(void) = lua_real("luaL_newstate"); void *L = f(); 
  int (*sm)(void *, int, int) = lua_real("luaJIT_setmode"); if (L && sm) fprintf(stderr, "[lua] JIT off %p: %d\n", L, sm(L, 0, 0));
  return L; }
static void *my_lua_newstate(void *a, void *ud) { lua_patch_once();
  void *(*f)(void *, void *) = lua_real("lua_newstate"); void *L = f(a, ud);
  int (*sm)(void *, int, int) = lua_real("luaJIT_setmode"); if (L && sm) fprintf(stderr, "[lua] JIT off %p: %d\n", L, sm(L, 0, 0));
  return L; }
static void *host(const char *n) {
  if (!strcmp(n, "Plat_IsInDebugSession")) return (void *)not_debugging;
  if (!strcmp(n, "SDL_ShowSimpleMessageBox")) return (void *)my_ShowSimpleMessageBox;
  if (!strcmp(n, "luaL_newstate") && lua_real(n)) return (void *)my_luaL_newstate;
  if (!strcmp(n, "lua_newstate") && lua_real(n)) return (void *)my_lua_newstate;
  if (lib_private(n)) return NULL;
  { void *g = glctx_resolve(n); if (!g) g = glctx_resolve3(n); if (g) return g; }
  void *p = platform_resolve(n);
  if (!p && !strncmp(n, "SDL_", 4)) p = dlsym(RTLD_DEFAULT, n);
  if (!p && (!strncmp(n, "gl", 2) || !strncmp(n, "egl", 3))) p = ios_gl_proc(n);
  if (!p) p = dlsym(RTLD_DEFAULT, n);
  return p;
}
static long stub0(void) { return 0; }
static void *late(const char *n) { fprintf(stderr, "stub %s\n", n); return (void *)stub0; }

static int exists(const char *a, const char *b) {
  char p[1200]; struct stat st; snprintf(p, sizeof p, "%s/%s", a, b); return stat(p, &st) == 0;
}

extern int csops(int pid, unsigned int ops, void *useraddr, size_t usersize);
extern int so_jit_mode(void), so_jit_traced(void);
static int jit_enabled(void) { unsigned int f = 0; csops(getpid(), 0, &f, sizeof f);
  if (!(f & 0x10000000)) return 0;
  return so_jit_mode() == 1 || so_jit_traced(); /* TXM (iOS 26, A15+) needs StikDebug still attached */ }
__attribute__((unused)) static void wait_for_jit(void) {
  while (!jit_enabled()) {
    const SDL_MessageBoxButtonData btn[] = {{SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 1, "Retry"}, {0, 0, "Quit"}};
    SDL_MessageBoxData d = {SDL_MESSAGEBOX_WARNING, NULL, "GMod", "JIT is not enabled.\n\nLaunch with JIT: TrollStore -> long-press GMod -> Open with JIT, or StikDebug.\niOS 26: in StikDebug long-press GMod -> Assign Script -> universal.js, then launch.\nThen tap Retry.", 2, btn, NULL};
    int b = 0; SDL_ShowMessageBox(&d, &b); if (b == 0) exit(0);
  }
}

static int g_jit_ready; // set once the libs got their JIT memory; on iOS 26 we detach from StikDebug right after, so P_TRACED goes 0
int ios_jit_ok(void) { return g_jit_ready || jit_enabled(); } // live: StikDebug can attach while the launcher is open
__attribute__((unused)) static void wait_for_content(void) {
  wait_for_jit();
  while (!exists(g_docs, "garrysmod/garrysmod_dir.vpk") && !exists(g_docs, "garrysmod/gameinfo.txt")) {
    char msg[2048];
    snprintf(msg, sizeof msg,
             "Game files not found.\n\nOpen the Files app -> On My iPhone -> this app, and copy these folders from your "
             "Garry's Mod install:\n  garrysmod\n  sourceengine\n  platform\n\nThen tap Retry.\n\n(%s)", g_docs);
    const SDL_MessageBoxButtonData btn[] = {{SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 1, "Retry"}};
    SDL_MessageBoxData d = {SDL_MESSAGEBOX_INFORMATION, NULL, "GMod", msg, 1, btn, NULL};
    int b; SDL_ShowMessageBox(&d, &b);
  }
}

#include <signal.h>
#include <sys/ucontext.h>
void so_describe(const char *tag, void *a);
static void on_crash(int sig, siginfo_t *si, void *uc_) {
  ucontext_t *uc = uc_;
  fprintf(stderr, "\n*** signal %d addr %p\n", sig, si->si_addr);
  so_describe("pc", (void *)uc->uc_mcontext->__ss.__pc);
  so_describe("lr", (void *)uc->uc_mcontext->__ss.__lr);
  uint64_t *fp = (uint64_t *)uc->uc_mcontext->__ss.__fp;
  for (int i = 0; i < 24 && fp && ((uintptr_t)fp & 7) == 0; i++) { so_describe("bt", (void *)fp[1]); fp = (uint64_t *)fp[0]; }
  signal(sig, SIG_DFL); raise(sig);
}


#include <mach/mach.h>
#include <pthread.h>
#include <dispatch/dispatch.h>
static char g_logpath[1200];
static void dump_threads(void) {
  thread_act_array_t th; mach_msg_type_number_t n; thread_t self = mach_thread_self();
  if (task_threads(mach_task_self(), &th, &n)) return;
  fprintf(stderr, "\n=== WATCHDOG: log idle, %u threads ===\n", n);
  for (unsigned i = 0; i < n; i++) {
    if (th[i] == self) continue;
    thread_suspend(th[i]);
    arm_thread_state64_t st; mach_msg_type_number_t c = ARM_THREAD_STATE64_COUNT;
    if (!thread_get_state(th[i], ARM_THREAD_STATE64, (thread_state_t)&st, &c)) {
      fprintf(stderr, "--- thread %u\n", i);
      so_describe("pc", (void *)arm_thread_state64_get_pc(st));
      so_describe("lr", (void *)arm_thread_state64_get_lr(st));
      uint64_t *fp = (uint64_t *)arm_thread_state64_get_fp(st);
      for (int k = 0; k < 40 && fp && ((uintptr_t)fp & 7) == 0; k++) { so_describe("bt", (void *)fp[1]); fp = (uint64_t *)fp[0]; }
    }
    thread_resume(th[i]);
  }
}
#include <os/proc.h>
#include <mach/task_info.h>
static void mem_log(int force) {
  static int peak, lastmb = -1000; task_vm_info_data_t vi; mach_msg_type_number_t c = TASK_VM_INFO_COUNT;
  if (task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&vi, &c)) return;
  int mb = (int)(vi.phys_footprint >> 20), av = (int)(os_proc_available_memory() >> 20);
  if (mb > peak) peak = mb;
  if (force || mb - lastmb >= 100 || lastmb - mb >= 100 || av < 300) { lastmb = mb;
    fprintf(stderr, "[mem] footprint %d MB (peak %d), available before kill %d MB\n", mb, peak, av); }
}
/* sampling profiler of the render thread while fps is low: 600 samples ~3 s, top self + inclusive addresses */
extern volatile unsigned g_render_thread; extern volatile int g_prof_want;
#define PH 8192
static uintptr_t ph_k[PH]; static int ph_s[PH], ph_i[PH];
static void ph_add(uintptr_t a, int self) { a &= 0x0000000fffffffffULL; if (!a) return; unsigned h = (unsigned)((a >> 2) * 2654435761u) % PH;
  for (int k = 0; k < PH; k++, h = (h + 1) % PH) { if (ph_k[h] == a || !ph_k[h]) { ph_k[h] = a; if (self) ph_s[h]++; else ph_i[h]++; return; } } }
static void ph_top(int *v, const char *tag, int total) { for (int r = 0; r < 25; r++) { int b = -1; for (int k = 0; k < PH; k++) if (ph_k[k] && v[k] > 0 && (b < 0 || v[k] > v[b])) b = k;
    if (b < 0) break; char t[64]; snprintf(t, sizeof t, "[prof] %s %3d%%", tag, v[b] * 100 / total); so_describe(t, (void *)ph_k[b]); v[b] = -v[b]; } }
static void *profiler(void *u) {
  for (int round = 0; round < 3;) {
    usleep(200000); if (!g_prof_want || !g_render_thread) continue;
    memset(ph_k, 0, sizeof ph_k); memset(ph_s, 0, sizeof ph_s); memset(ph_i, 0, sizeof ph_i); int n = 0;
    thread_t t = g_render_thread;
    for (int i = 0; i < 600; i++) { usleep(5000);
      if (thread_suspend(t)) break;
      arm_thread_state64_t st; mach_msg_type_number_t c = ARM_THREAD_STATE64_COUNT;
      if (!thread_get_state(t, ARM_THREAD_STATE64, (thread_state_t)&st, &c)) { n++;
        uintptr_t pc = (uintptr_t)arm_thread_state64_get_pc(st); ph_add(pc, 1); ph_add((uintptr_t)arm_thread_state64_get_lr(st), 0);
        uint64_t *fp = (uint64_t *)arm_thread_state64_get_fp(st); uintptr_t seen[24]; int ns = 0;
        for (int k = 0; k < 24 && fp && ((uintptr_t)fp & 7) == 0 && (uintptr_t)fp > 0x100000000ULL; k++) { uintptr_t ra = fp[1] & 0x0000000fffffffffULL; int dup = 0;
          for (int q = 0; q < ns; q++) if (seen[q] == ra) dup = 1; if (!dup) { seen[ns++] = ra; ph_add(ra, 0); } fp = (uint64_t *)fp[0]; } }
      thread_resume(t); }
    if (n < 50) continue; round++;
    fprintf(stderr, "[prof] round %d: %d samples of render thread (fps low)\n", round, n);
    ph_top(ph_s, "self", n); ph_top(ph_i, "incl", n);
    usleep(3000000);
  }
  return NULL;
}
static void *watchdog(void *a) {
  off_t last = -1; int idle = 0, dumps = 0;
  mem_log(1);
  dispatch_source_t ps = dispatch_source_create(DISPATCH_SOURCE_TYPE_MEMORYPRESSURE, 0, DISPATCH_MEMORYPRESSURE_WARN | DISPATCH_MEMORYPRESSURE_CRITICAL, dispatch_get_global_queue(0, 0));
  dispatch_source_set_event_handler(ps, ^{ fprintf(stderr, "[mem] iOS memory pressure %s\n", dispatch_source_get_data(ps) & DISPATCH_MEMORYPRESSURE_CRITICAL ? "CRITICAL" : "warning"); mem_log(1); });
  dispatch_resume(ps);
  for (;;) {
    sleep(1);
    mem_log(0);
    { static so_module *cl; static char last[200]; static int lg[6]; if(!cl) cl=so_find("libclient.so"); /* 0.1.31 offsets */
      if(cl){ const char *r=(const char*)(cl->base+0x1467648); if(r[0]&&strncmp(r,last,191)){ strncpy(last,r,191); fprintf(stderr,"[nhalo] reason: %.191s\n",r); }
        int v[6]; v[0]=*(volatile int*)(cl->base+0x14751d0); v[1]=*(volatile unsigned char*)(cl->base+0x1467630); v[2]=*(volatile int*)(cl->base+0x14678a8);
        uint64_t q=*(volatile uint64_t*)(cl->base+0x14675d0); v[3]=q!=0; const uint64_t*vv=(const uint64_t*)(cl->base+0x1474f50); v[4]=vv[0]!=vv[1];
        uint64_t cv=*(volatile uint64_t*)(cl->base+0x14674f8); v[5]=cv?*(volatile int*)(cv+88):-1;
        if(memcmp(v,lg,sizeof v)){ memcpy(lg,v,sizeof v); fprintf(stderr,"[nhalo] gates f90=%d b010=%d e288=%d fb0=%d vec=%d cvar=%d\n",v[0],v[1],v[2],v[3],v[4],v[5]); } } }
    struct stat st; if (stat(g_logpath, &st)) continue;
    if (st.st_size != last) { last = st.st_size; idle = 0; continue; }
    if (++idle == 8 && dumps < 2) { dumps++; dump_threads(); idle = -20; }
  }
  return a;
}

static int g_ac; static char **g_av;
int main(int argc, char **argv) {
  snprintf(g_bundle, sizeof g_bundle, "%s", ios_bundle_path());
  snprintf(g_docs, sizeof g_docs, "%s", ios_documents_path());
  char lib[1200], wr[1200], rt[1200], lnk[1200], log[1200];
  snprintf(log, sizeof log, "%s/gmod_log.txt", g_docs);
  freopen(log, "w", stderr); setvbuf(stderr, NULL, _IONBF, 0);
  snprintf(g_logpath, sizeof g_logpath, "%s", log);
  { pthread_t t; pthread_create(&t, NULL, watchdog, NULL); pthread_detach(t); }
  (void)profiler; /* profiler disabled: frame-pointer walk crashed on LuaJIT frames */
  setenv("SO_MAP", "1", 1); setenv("SO_VERBOSE", "1", 1); setenv("JNI_VERBOSE", "1", 1); setenv("JNI_VERBOSE", "1", 1);
  { struct sigaction sa = {0}; sa.sa_sigaction = on_crash; sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    static char alt[65536]; stack_t ss = {alt, 0, sizeof alt}; sigaltstack(&ss, NULL);
    sigaction(SIGSEGV, &sa, NULL); sigaction(SIGBUS, &sa, NULL); sigaction(SIGILL, &sa, NULL); }
  SDL_Init(SDL_INIT_VIDEO);  // needed for message box; engine re-inits
  { extern void ios_kbfix_install(void); ios_kbfix_install(); }
  fprintf(stderr, "[status] jit %d\n", ios_jit_ok()); // launcher shows it and blocks play without it

  snprintf(lib, sizeof lib, "%s/lib", g_bundle);
  /* iOS 26 TXM: do every debugger request NOW, while StikDebug is still in the foreground-adjacent state
     (it gets suspended while the user sits in the launcher -> a later request would hang forever), then detach */
  so_skip("libSDL2.so"); so_skip("libopenxr_loader.so");
  int reserved = 0; if (jit_enabled()) { so_jit_reserve_dir(lib); extern void so_jit_detach(void); so_jit_detach(); reserved = 1; g_jit_ready = 1; }
  snprintf(rt, sizeof rt, "%s/rt", g_bundle);
  char ur[1200]; snprintf(ur, sizeof ur, "%s/../Library/user", g_docs); mkdir(ur, 0755); { char rp[1200]; if (realpath(ur, rp)) snprintf(ur, sizeof ur, "%s", rp); } snprintf(wr, sizeof wr, "%s/garrysmod", ur); mkdir(wr, 0755); { char rp[1200]; if (realpath(wr, rp)) snprintf(wr, sizeof wr, "%s", rp); }
  { extern void workshop_set_root(const char *); workshop_set_root(g_docs); }
  { extern char **ios_run_launcher(const char *, const char *, int *); g_av = ios_run_launcher(g_docs, wr, &g_ac); }
  if (!reserved) { fprintf(stderr, "[jit] enabled while in the launcher, preparing now\n"); so_jit_reserve_dir(lib); extern void so_jit_detach(void); so_jit_detach(); }
  snprintf(lnk, sizeof lnk, "%s/garrysmod/runtime", g_docs);
  unlink(lnk); symlink(rt, lnk);  // bundle path changes per install
  { // Windows GMod has no linux_fonts; engine (FreeType) needs them under platform/resource/linux_fonts
    const char *fn[] = {"dejavusans.ttf", "dejavusans-bold.ttf", "dejavusans-oblique.ttf", "dejavusans-boldoblique.ttf", "liberationmono-regular.ttf"};
    char d[1300], a[1400], b[1400]; snprintf(d, sizeof d, "%s/platform/resource/linux_fonts", g_docs); mkdir(d, 0755);
    for (int i = 0; i < 5; i++) {
      snprintf(a, sizeof a, "%s/platform/resource/linux_fonts/%s", rt, fn[i]); snprintf(b, sizeof b, "%s/%s", d, fn[i]);
      struct stat st; if (!stat(b, &st) && st.st_size > 0) continue;
      FILE *in = fopen(a, "rb"), *out = in ? fopen(b, "wb") : NULL; char buf[65536]; size_t n;
      if (in && out) while ((n = fread(buf, 1, sizeof buf, in)) > 0) fwrite(buf, 1, n, out);
      if (in) fclose(in); if (out) fclose(out);
      fprintf(stderr, "font %s -> %s\n", fn[i], out ? "copied" : "FAILED");
    }
  }
  { char c[2600]; const char *bs[2]={wr,g_docs}; for(int k=0;k<2;k++){ snprintf(c,sizeof c,"%s/shadercache/glprogram",bs[k]); DIR*dd=opendir(c); struct dirent*e; int nn=0; if(dd){ while((e=readdir(dd))){ if(e->d_name[0]=='.')continue; char f[2900]; snprintf(f,sizeof f,"%s/%s",c,e->d_name); if(!unlink(f))nn++; } closedir(dd);} fprintf(stderr,"[shcache] %s cleared %d\n",c,nn);} }
  setenv("APP_LIB_PATH", lib, 1);
  setenv("APP_DATA_PATH", g_docs, 1);
  setenv("VALVE_GAME_PATH", g_docs, 1);
  setenv("SOURCEVR_SHARED_CONTENT_PATH", g_docs, 1);
  { /* engine builds "<write path>/cache/sound/v2/<64 hex>.sound.cache" into a 260-byte buffer and refuses the whole
       sound cache when it doesn't fit (-> "Failed to load sound"). LiveContainer nests the container -> too long.
       Use a short symlink in the outer (host) container instead. */
    const char *ew = wr; static char sw[600];
    if (strlen(wr) + 92 >= 260) {
      const char *m = strstr(g_docs, "/Documents/Data/Application/");
      if (m) { snprintf(sw, sizeof sw, "%.*s/gmw", (int)(m - g_docs), g_docs);
        char cur[1200]; ssize_t n = readlink(sw, cur, sizeof cur - 1); if (n > 0) cur[n] = 0;
        if (n <= 0 || strcmp(cur, wr)) { unlink(sw); symlink(wr, sw); }
        struct stat st; if (!stat(sw, &st) && S_ISDIR(st.st_mode) && strlen(sw) + 92 < 260) ew = sw; }
      fprintf(stderr, "[snd] write path %zu chars too long for sound cache -> %s\n", strlen(wr), ew == wr ? "NO short path found" : ew);
    }
    setenv("SOURCEVR_WRITE_GAME_PATH", ew, 1); }
  setenv("SOURCEVR_GAME", "garrysmod", 1); setenv("SOURCE_GAME", "garrysmod", 1); setenv("EXTRAS_VPK_PATH", rt, 1); setenv("APP_MOD_LIB", lib, 1); setenv("LANG", "en_US.UTF-8", 1);
  bionic_set_root(g_docs);
  setenv("SO_MAP", "1", 1); setenv("SO_VERBOSE", "1", 1);

  platform_init();
  { extern void webview_init(void); webview_init(); }
  { extern void touchedit_jni_init(void); touchedit_jni_init(); }
  { extern void workshop_jni_init(void); workshop_jni_init(); }
  { extern void mp_jni_init(void); mp_jni_init(); }
  bionic_gl_proc = ios_gl_proc;
  so_set_libdir(lib);
  so_skip("libSDL2.so");
  so_skip("libopenxr_loader.so");  // VR only; its ctor crashes on iOS
  bionic_init(host);
  so_set_late_resolver(late);
  so_set_resolver(bionic_resolve);
  char p[1300]; snprintf(p, sizeof p, "%s/liblauncher.so", lib);
  int u = 0; so_module *m = so_load(p, &u);
  if (!m) { fprintf(stderr, "load failed\n");
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "GMod", "Loading the game libraries failed (JIT memory). Send Documents/gmod_log.txt.", NULL);
    return 1; }
  fprintf(stderr, "loaded, unresolved=%d\n", u);
  bionic_thread_enter();
  { /* liblua's own internal PLT calls bypass the resolver: patch its GOT slots (APK 0.1.31 offsets) */
    so_module *m = so_find("liblua_shared.so");
    if (m) {
      /* __clear_cache reads CTR_EL0 (mrs) unless cached at .bss+0x26d468; mrs ctr_el0 traps on iOS -> pre-fill */
      { uint64_t *c = (uint64_t *)(m->base + 0x26d468); uintptr_t pg = (uintptr_t)c & ~(uintptr_t)16383;
        mprotect((void *)pg, 16384, PROT_READ | PROT_WRITE);
        fprintf(stderr, "[lua] ctr cache was %llx\n", (unsigned long long)*c); *c = 0x8444c004ULL; }
      struct { size_t off; const char *n; void *hook; } g[] = {
        {0x254488, "luaL_newstate", (void *)my_luaL_newstate}, {0x254180, "lua_newstate", (void *)my_lua_newstate}};
      for (int i = 0; i < 2; i++) {
        void **slot = (void **)(m->base + g[i].off); void *real = so_sym(m, g[i].n);
        uintptr_t pg = (uintptr_t)slot & ~(uintptr_t)16383;
        mprotect((void *)pg, 16384, PROT_READ | PROT_WRITE);
        fprintf(stderr, "[lua] GOT %s %p real %p -> %s\n", g[i].n, *slot, real, *slot == real ? "hooked" : "MISMATCH");
        if (*slot == real) *slot = g[i].hook;
      }
    }
  }
  so_init_all();
  dxt1_patch(); togl_soft_patch();
  { /* libshaderapidx9 0.1.31: the dynamic index buffer is 32768 indices; big addon meshes ask for more
       ("too many indices for index buffer (32802>32768)") -> tier0 Error() -> the game exits. Make it 65536. */
    so_module *sa = so_find("libshaderapidx9.so");
    if (sa) { uint32_t *ins = (uint32_t *)(sa->base + 0x460a8);
      extern void *so_wptr(void *); extern int so_is_mirrored(void *);
      if (*ins == 0x52900008) { uintptr_t pg = (uintptr_t)ins & ~(uintptr_t)16383; int a = 0, b = 0;
        if (so_is_mirrored(ins)) *(uint32_t *)so_wptr(ins) = 0x52a00028;  /* mov w8, #0x10000 */
        else { a = mprotect((void *)pg, 16384, PROT_READ | PROT_WRITE); *ins = 0x52a00028; b = mprotect((void *)pg, 16384, PROT_READ | PROT_EXEC); }
        sys_icache_invalidate(ins, 4); fprintf(stderr, "[ib] dynamic index buffer 32768 -> 65536 (%d %d)\n", a, b); }
      else fprintf(stderr, "[ib] patch: unexpected insn %08x\n", *ins); } }
  int (*jni_onload)(void *, void *) = so_sym(m, "JNI_OnLoad");
  if (jni_onload) jni_onload(jni_vm(), NULL);
  int (*lma)(int, char **) = so_sym(m, "LauncherMainAndroid");
  int r = lma(g_ac, g_av);
  fprintf(stderr, "LauncherMainAndroid returned %d\n", r);
  return r;
}
