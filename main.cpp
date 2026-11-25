// Mini-Rust: survival sandbox. C++ -> WebAssembly (Emscripten), touch controls for iPhone.
// =====================================================================================================
//  PERFORMANCE NOTES (what the engine does so that a big map stays fast)
//   * terrain:   5-unit lattice cache (heights / normals / biome, 80x80 unit blocks) -> static indexed meshes, frustum culled
//   * nodes:     plants (GLB models: trees / bushes / cacti, 3 detail levels, instanced) and rocks live in 64x64 chunks (hash grid, SoA); rocks and pebbles are baked into 32x32 static batches
//                baked once, rebuilt only when a node is harvested / respawns or the detail level changes; horizon occlusion culling
//   * grass:     16x16 static batches, the wind runs in the vertex shader
//   * shadows:   one cached shadow map, redrawn only when the sun moves, the player leaves the middle or a caster changes
//   * pieces:    spatial hash grid (collision, aiming, snapping), cached matrices, sleeping doors, instanced drawing
//   * streaming: everything near the player is generated / baked with a small per-frame time budget, far chunks are released
//   * adaptation lattice and keep-clear map are sparse (memory grows with road length, not with the map area)
//   * draw order: opaque -> terrain -> grass -> alpha-tested -> water (early z, no overdraw of the heavy terrain shader)
//  Profiling: build with -DPROF, open the browser console: one "PROF {...}" line every 15 frames (CPU ms, draw calls, MB uploaded).
//  Recommended build: see build.sh  (-O3 -flto -sGL_TRACK_ERRORS=0 -sMALLOC=emmalloc -sALLOW_MEMORY_GROWTH=1 ...)
// =====================================================================================================
#include <emscripten.h>
#include <emscripten/html5.h>
#define GL_GLEXT_PROTOTYPES 1
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <cmath>
#include <cstdio>
#include <vector>
#include <algorithm>
#include <queue>
#include <cstring>
#include <cstdlib>
#include <unordered_map>
using namespace std;


#ifdef PROF   // build with -DPROF to get a per-frame CPU / draw call report in the browser console (zero cost otherwise)
static double pT[32]; static long pC[8]; static int pFr = 0;
struct PS { int i; double t0; PS(int i_) : i(i_), t0(emscripten_get_now()) {} ~PS() { pT[i] += emscripten_get_now() - t0; } };
#define PS_CAT2(a, b) a##b
#define PS_CAT(a, b) PS_CAT2(a, b)
#define PROF_SCOPE(i) PS PS_CAT(_ps, __LINE__)(i)
#define glDrawArrays(a, b, c) (pC[0]++, pC[2] += (c), glDrawArrays(a, b, c))
#define glDrawElements(a, b, c, d) (pC[0]++, pC[2] += (b), glDrawElements(a, b, c, d))
#define glDrawElementsInstancedANGLE(a, b, c, d, e) (pC[0]++, pC[2] += (long)(b)*(e), glDrawElementsInstancedANGLE(a, b, c, d, e))
#define glBufferData(a, b, c, d) (pC[1] += (b), glBufferData(a, b, c, d))
static int profNodes(); static int profChunks();
static void profFrame() {
  if (++pFr % 15) return;
  char b[600]; snprintf(b, sizeof b, "PROF {\"frames\":15,\"upd\":%.2f,\"stream\":%.2f,\"shadow\":%.2f,\"world\":%.2f,\"frame\":%.2f,\"draws\":%ld,\"uploadKB\":%ld,\"verts\":%ld,\"occ\":%ld,\"nodes\":%d,\"chunks\":%d}",
    pT[1]/15, pT[12]/15, pT[3]/15, pT[15]/15, pT[0]/15, pC[0]/15, pC[1]/15/1024, pC[2]/15, pC[3]/15, profNodes(), profChunks());
  EM_ASM({ console.log(UTF8ToString($0)); }, b);
  memset(pT, 0, sizeof pT); memset(pC, 0, sizeof pC);
}
#else
#define PROF_SCOPE(i)
#endif
struct V { float x, y, z, r, g, b; };
struct Blk { float x, z, w, d; bool wall; float y; };       // building piece
struct Fin { int id; float x, y, bx, by; };        // finger

static vector<V> vb; static vector<Blk> blks;
static GLint uM, uFog, uLit, uTx, uSd, uEy, uTm, uLm, uSm, uSs, uShLm; static GLuint prog, shProg, shFbo, shTex; static bool shOk = false; static vector<V> sv; static float txOn = 1; static int W = 0, H = 0; static float K = 1;
static float px = 0, py = 0, pz = 0, vy = 0, yaw = 0, pitch = 0, hp = 100, food = 100;
static float tod = .1f, cd = 0, hudT = 0, msgT = 0; static int wood = 400, stone = 0, metal = 0, sulfur = 0, mode = 0; static float thirst = 100;
static char msg[96] = ""; static bool inBush = false; static Fin st = {-1}, lk = {-1};
static double last = 0; static unsigned rs = 12345;

EM_JS(int, gk, (int i), { var v = window.G[i] | 0; if (i == 1 || i == 12) window.G[i] = 0; return v; });
EM_JS(float, gf, (int i), { var v = window.G[i] || 0; if (i > 4) window.G[i] = 0; return v; });
EM_JS(void, sfx, (int id), { if (window.audioEvent) window.audioEvent(id); });
EM_JS(int, glbN, (const char* nm, int k), { var m = window.GL3 && window.GL3[UTF8ToString(nm)]; if (!m) return 0; return k == 0 ? m.v.length/6 : m.i.length; });
EM_JS(void, glbV, (const char* nm, float* dst), { HEAPF32.set(window.GL3[UTF8ToString(nm)].v, dst >> 2); });
EM_JS(void, glbI, (const char* nm, unsigned short* dst), { HEAPU16.set(window.GL3[UTF8ToString(nm)].i, dst >> 1); });
EM_JS(int, texUp, (int id, int k), { var im = window.TSLOT && window.TSLOT[k]; if (!im) return 0; var g = GLctx; g.activeTexture(g.TEXTURE0); g.bindTexture(g.TEXTURE_2D, GL.textures[id]); g.pixelStorei(g.UNPACK_FLIP_Y_WEBGL, false); g.texImage2D(g.TEXTURE_2D, 0, g.RGBA, g.RGBA, g.UNSIGNED_BYTE, im); if (k == 5) { g.texParameteri(g.TEXTURE_2D, g.TEXTURE_MIN_FILTER, g.NEAREST); g.texParameteri(g.TEXTURE_2D, g.TEXTURE_MAG_FILTER, g.NEAREST); g.texParameteri(g.TEXTURE_2D, g.TEXTURE_WRAP_S, g.CLAMP_TO_EDGE); g.texParameteri(g.TEXTURE_2D, g.TEXTURE_WRAP_T, g.CLAMP_TO_EDGE); } else { g.generateMipmap(g.TEXTURE_2D); g.texParameteri(g.TEXTURE_2D, g.TEXTURE_MIN_FILTER, g.LINEAR_MIPMAP_LINEAR); g.texParameteri(g.TEXTURE_2D, g.TEXTURE_MAG_FILTER, g.LINEAR); g.texParameteri(g.TEXTURE_2D, g.TEXTURE_WRAP_S, g.REPEAT); g.texParameteri(g.TEXTURE_2D, g.TEXTURE_WRAP_T, g.REPEAT); } return 1; });
EM_JS(float, glbB, (const char* nm, int k), { var m = window.GL3 && window.GL3[UTF8ToString(nm)]; if (!m) return 0; return k < 6 ? m.bb[k] : k == 6 ? m.topw : k == 7 ? m.topd : m.piv[0]; });
// ---- plant models: parsed / simplified by the loader in index.html (window.TR / TRT / TRNAMES / TRDY), uploaded and drawn further down ----
EM_JS(int, trReady, (), { return window.TRDY ? 1 : 0; });
EM_JS(int, trNTex, (), { return window.TRT ? window.TRT.length : 0; });
EM_JS(int, trN, (int s, int lod, int part, int k), { var m = window.TR && window.TR[window.TRNAMES[s]]; if (!m) return 0; var q = m.lod[lod][part ? 'cut' : 'op']; return k == 0 ? q.v.length/6 : q.i.length; });
EM_JS(void, trV, (int s, int lod, int part, float* dst), { HEAPF32.set(window.TR[window.TRNAMES[s]].lod[lod][part ? 'cut' : 'op'].v, dst >> 2); });
EM_JS(void, trI, (int s, int lod, int part, unsigned short* dst), { HEAPU16.set(window.TR[window.TRNAMES[s]].lod[lod][part ? 'cut' : 'op'].i, dst >> 1); });
EM_JS(float, trB, (int s, int k), { var m = window.TR && window.TR[window.TRNAMES[s]]; return m ? m.bb[k] : 0; });
EM_JS(int, trSlot, (int s, int k), { var m = window.TR && window.TR[window.TRNAMES[s]]; return m && k < m.slots.length ? m.slots[k] : -1; });
EM_JS(int, trTexUp, (int id, int gi), { var t = window.TRT && window.TRT[gi]; if (!t || !t.d) return 0; var g = GLctx; g.activeTexture(g.TEXTURE0); g.bindTexture(g.TEXTURE_2D, GL.textures[id]); g.pixelStorei(g.UNPACK_FLIP_Y_WEBGL, false); g.pixelStorei(g.UNPACK_PREMULTIPLY_ALPHA_WEBGL, false); g.texImage2D(g.TEXTURE_2D, 0, g.RGBA, t.w, t.h, 0, g.RGBA, g.UNSIGNED_BYTE, t.d); g.generateMipmap(g.TEXTURE_2D); g.texParameteri(g.TEXTURE_2D, g.TEXTURE_MIN_FILTER, g.LINEAR_MIPMAP_LINEAR); g.texParameteri(g.TEXTURE_2D, g.TEXTURE_MAG_FILTER, g.LINEAR); g.texParameteri(g.TEXTURE_2D, g.TEXTURE_WRAP_S, g.REPEAT); g.texParameteri(g.TEXTURE_2D, g.TEXTURE_WRAP_T, g.REPEAT); return 1; });
EM_JS(int, tpGet, (float* o), { var t = window.TP; if (!t) return 0; window.TP = null; for (var i = 0; i < 8; i++) HEAPF32[(o >> 2) + i] = +t[i] || 0; return 1; });   // test hook: window.TP = [x, z, yaw, pitch] teleports the player
EM_JS(void, tpHist, (int* a, int n), { window.TPH = Array.from(HEAP32.subarray(a >> 2, (a >> 2) + n)); });
EM_JS(void, trStat, (int tris, int inst), { window.TRS = [tris, inst]; });
EM_JS(void, tpInfo, (float x, float z, float h, float b), { window.TPI = [x, z, h, b]; });
EM_JS(void, aud, (int sf, int sp, int bu, float td, int jm), { if (window.audioState) window.audioState(sf, sp, bu, td, jm); });
EM_JS(void, hud, (const char* s), { if (window.hudUpdate) window.hudUpdate(UTF8ToString(s)); });
EM_JS(double, wcf, (int i, double d), { return window.WC && window.WC.length > i ? +window.WC[i] : d; });   // world settings from the menu
EM_JS(void, wready, (double seed, int np, double rk, double tk), { if (window.worldReady) window.worldReady(seed, np, rk, tk); });
EM_JS(void, mapImg, (const unsigned char* p, int m, float e), { var c = document.createElement('canvas'); c.width = c.height = m; var g = c.getContext('2d'), d = g.createImageData(m, m); d.data.set(HEAPU8.subarray(p, p + m*m*4)); g.putImageData(d, 0, 0); window.mapCanvas = c; window.MAPE = e; });

static const char* VS = "attribute vec3 p;attribute vec3 c;attribute vec4 i0;attribute vec4 i1;attribute vec4 i2;attribute vec4 i3;uniform mat4 m;uniform mat4 lm;uniform mat4 mm;uniform float tx;uniform float tm;uniform float inst;varying vec3 vc;varying float d;varying vec3 wp;varying vec4 ls;varying float wo;varying float vsn;"
  "void main(){vec3 q=p,cc=c;float o=0.;if(tx>1.5&&tx<2.5){o=.16*sin(tm*.7)+.1*sin(dot(p.xz,vec2(.06,.045))-tm*1.15);q.y+=o;}"
  "else if(tx>7.5&&c.r>1.5){cc.r-=2.;float w=sin(tm*2.+p.x*.7+p.z*.5)*.03;q.x+=w;q.z+=w*.5;}"   // grass tips (red channel +2) sway in the vertex shader
  "mat4 M=inst>.5?mat4(i0,i1,i2,i3):mm;vsn=M[0][3];vec4 w4=M*vec4(q,1.);w4.w=1.;gl_Position=m*w4;vc=cc;d=gl_Position.w;wp=w4.xyz;ls=lm*w4;wo=o;}";
static const char* FS = "#extension GL_OES_standard_derivatives : enable\n"
  "precision highp float;varying vec3 vc;varying float d;varying vec3 wp;varying vec4 ls;varying float wo;varying float vsn;uniform vec3 fog;uniform vec3 sd;uniform vec3 ey;uniform float lit;uniform float tx;uniform float tm;uniform sampler2D sm;uniform float ss;uniform sampler2D t0;uniform sampler2D t1;uniform sampler2D t2;uniform sampler2D t3;uniform sampler2D t4;uniform sampler2D t5;uniform float gt;uniform sampler2D rm;uniform vec3 rmo;"
  "float h(vec2 p){vec3 q=fract(vec3(p.xyx)*.1031);q+=dot(q,q.yzx+33.33);return fract((q.x+q.y)*q.z);}"
  "float n(vec2 p){vec2 i=floor(p),f=fract(p);f=f*f*f*(f*(f*6.-15.)+10.);return mix(mix(h(i),h(i+vec2(1.,0.)),f.x),mix(h(i+vec2(0.,1.)),h(i+vec2(1.,1.)),f.x),f.y);}"
  "float fb(vec2 p){float a=.5,s=0.;for(int i=0;i<5;i++){s+=a*n(p);p=mat2(.8,.6,-.6,.8)*p*2.03+vec2(17.1,9.2);a*=.5;}return s;}"
  "float wv(vec2 p){p=mat2(.8,.6,-.6,.8)*p;float a=n(p*.35+vec2(tm*.25,tm*.15))*.6;p=mat2(.8,.6,-.6,.8)*p*2.1;return a+n(p-vec2(tm*.45,tm*.3))*.28+n(p*2.3+tm*.6)*.12;}"
  "float up(vec4 c){return dot(c,vec4(1.,1./255.,1./65025.,1./16581375.));}"
  "float shw(float bs){vec3 q=ls.xyz/ls.w*.5+.5;if(q.x<0.||q.x>1.||q.y<0.||q.y>1.||q.z>1.)return 1.;float s=(q.z-bs>up(texture2D(sm,q.xy)))?0.:1.;for(int i=0;i<8;i++){float a=float(i)*.785;vec2 o=vec2(cos(a),sin(a))*.0011;s+=(q.z-bs>up(texture2D(sm,q.xy+o)))?0.:1.;}float e=max(abs(q.x-.5),abs(q.y-.5))*2.;return mix(1.,s/9.,1.-smoothstep(.8,1.,e));}"
  "void main(){if(tx>.5&&tx<1.5&&d>=110.){gl_FragColor=vec4(fog,1.);return;}vec3 c=vc;float a=1.;"
  "if(tx<.5){vec3 al=vc;vec3 N=normalize(cross(dFdx(wp),dFdy(wp)));if(dot(N,ey-wp)<0.)N=-N;bool pc=vc.b>19.5,lfx=vc.b>29.5,rk=vc.r>5.&&!pc;"
  "if(rk){float m10=floor(vc.r/10.),mt=m10-1.,sn=0.;if(mt>7.5){mt-=8.;sn=1.;}vec3 bs=mt<.5?vec3(.62,.62,.64):(mt<1.5?vec3(.3,.2,.16):(mt<2.5?vec3(.2,.22,.17):(mt<3.5?vec3(.42,.4,.38):(mt<4.5?vec3(.2,.42,.16):(mt<5.5?vec3(.05,.15,.05):(mt<6.5?vec3(.08,.18,.07):vec3(.3,.26,.12)))))));"
  "float st=n(vec2(wp.y*5.+n(wp.xz*1.1)*3.,(wp.x+wp.z)*.35)),gr=n(wp.xz*16.+wp.y*13.),vz=abs(n(wp.xz*2.4+wp.y*2.1)-.5),vein=1.-smoothstep(0.,.05,vz);al=bs*(.7+.45*st)*(.82+.3*gr);"
  "if(mt<.5){al=mix(al,vec3(.35,.36,.4),vein*.7);al=mix(al,vec3(.9,.9,.92),smoothstep(.72,.8,n(wp.xz*9.+wp.y*8.))*.5);al=mix(al,vec3(.2,.36,.12),smoothstep(.62,.9,N.y)*smoothstep(.5,.7,n(wp.xz*5.))*.4*(1.-sn));}else if(mt<1.5){al=mix(al,vec3(.12,.07,.06),vein*.7);al=mix(al,vec3(.8,.4,.14),smoothstep(.66,.72,n(wp.xz*10.+wp.y*9.))*.85);}else if(mt<2.5){al=mix(al,vec3(.88,.82,.16),vein*.95);al=mix(al,vec3(.8,.78,.2),smoothstep(.7,.76,n(wp.xz*8.+wp.y*7.))*.6);}else if(mt>3.5&&mt<4.5){al*=.78+.3*abs(sin(vc.g*50.27))+.15*h(floor(wp.xz*11.+wp.y*7.));}"
  "if(mt>5.5&&mt<6.5)sn=1.;if(sn>.5)al=mix(al,vec3(.93,.96,1.),smoothstep(.3,.7,N.y)*.92);}"
  "if(pc){float id=floor(vc.b-19.5);bool lq=id>9.5;if(lq)id-=10.;vec2 tc=vc.rg;vec4 tq=id<.5?texture2D(t0,tc):(id<1.5?texture2D(t1,tc):(id<2.5?texture2D(t2,tc):(id<3.5?texture2D(t3,tc):(id<4.5?texture2D(t4,tc):texture2D(t5,tc)))));if(lq&&tq.a<.5)discard;al=tq.rgb;if(vsn>.01)al=mix(al,vec3(.93,.96,1.),smoothstep(.1,.6,N.y)*vsn*(lq?.95:.5));}"
  "vec3 L=normalize(sd);float dl=dot(N,L),dif=lfx?(max(dl,0.)+.25*max(-dl,0.)):max(dl,0.),hm=.5+.5*N.y,si=clamp(L.y*2.5+.2,0.,1.),nt=1.-clamp(L.y*4.+.6,0.,1.);"
  "float g=n(wp.xz*5.+wp.y*3.)*.55+n(wp.xz*19.+wp.y*11.)*.45;c=al*(lfx?.84+.28*g:((rk||pc)?1.:.72+.56*g));float sh=1.;if(ss>.5)sh=shw(.0012+.004*(1.-dif));c*=(.22+.3*hm)*mix(vec3(.9,.95,1.1),vec3(.5,.65,1.25),nt)*mix(lit,.62,nt)+si*dif*sh*vec3(.95,.88,.72)+vec3(.45,.55,.95)*.5*max(-dl,0.)*nt;if(gt>.5){vec3 gc=gt<1.5?vec3(.22,.6,1.):vec3(1.,.25,.2);c=gc*(.5+.6*dif+.3*hm);a=.6;}}"
  "else if(tx<1.5){vec2 p=wp.xz;float hh=wp.y,sl=1.-vc.g,bio=vc.b,li=vc.r,big=n(p*.07),mid=n(p*.6),fine=n(p*3.)*.5+n(p*13.)*.5;"
  "vec3 gr=mix(vec3(.2,.4,.12),vec3(.34,.5,.16),mid);gr=mix(gr,gr*vec3(1.35,1.1,.55),smoothstep(.45,.8,big)*.8);vec3 sa=mix(vec3(.74,.66,.46),vec3(.83,.75,.55),fine),ds=mix(vec3(.82,.6,.34),vec3(.92,.72,.44),mid),sw=mix(vec3(.86,.9,.97),vec3(.98,.99,1.),fine),rkc=mix(vec3(.46,.44,.4),vec3(.6,.57,.52),fine)*(.78+.3*n(vec2(hh*1.3+n(p*.05)*4.,(p.x+p.y)*.015)))*(.9+.15*n(p*.9));"
  "float sf=1.-smoothstep(.8,2.3,hh+(n(p*.35)-.5)*1.6+(fine-.5)*.6),dz=smoothstep(.2,.8,bio)*(1.-smoothstep(1.2,1.8,bio)),sz=smoothstep(1.2,1.8,bio);"
  "vec3 al=mix(gr,sa,sf);al=mix(al,ds*(.9+.2*sin(p.x*.5+p.y*.3+n(p*.15)*6.)),dz);al=mix(al,sw,sz*(1.-.5*sf));al*=mix(.62,1.,smoothstep(-.3,.5,hh));vec3 desertRock=mix(vec3(.48,.27,.14),vec3(.78,.48,.22),fine);vec3 temperateRock=mix(vec3(.34,.35,.32),vec3(.56,.52,.45),fine);vec3 winterRock=mix(vec3(.52,.55,.57),vec3(.8,.82,.84),fine);vec3 biomeRock=mix(temperateRock,desertRock,dz);biomeRock=mix(biomeRock,winterRock,sz);al=mix(al,biomeRock*mix(1.,1.12,sz),max(smoothstep(.1,.2,sl+(fine-.5)*.05),smoothstep(38.,60.,hh+(big-.5)*20.)*smoothstep(.03,.08,sl)));"
  "vec4 rv=texture2D(rm,(wp.xz-rmo.xy)/rmo.z);if(rv.r+rv.g+rv.b>.01){float g2=n(p*22.)*.5+n(p*7.)*.5,ra=smoothstep(.44,.56,rv.r),rg=smoothstep(.03,.3,rv.r),tq=smoothstep(.3,.62,rv.g+(fine-.5)*.3),pa=smoothstep(.46,.54,rv.b),mq=smoothstep(.46,.54,rv.a);vec3 tc=mix(mix(vec3(.4,.3,.19),vec3(.5,.39,.25),g2),mix(vec3(.66,.5,.3),vec3(.72,.57,.35),g2),dz);tc=mix(tc,vec3(.55,.5,.46),sz*.7);vec3 ac=vec3(.21,.21,.22)*(.8+.5*g2)*(.9+.2*n(p*1.5)),gv=mix(vec3(.45,.4,.34),vec3(.55,.5,.42),g2),rc=mix(gv,ac,ra);rc=mix(rc,sw,sz*.35*(1.-ra));vec3 cc=mix(vec3(.5),vec3(.62,.62,.6),g2)*(.88+.2*n(p*.8));al=mix(al,tc,tq*.92);al=mix(al,rc,rg);al=mix(al,cc,pa);al=mix(al,rv.b>.5?vec3(.92,.74,.1):vec3(.9,.82,.5),mq*max(ra,pa));}"
  "c=al*li*(.7+.55*(mid*.35+fine*.65));float nt=1.-clamp(normalize(sd).y*4.+.6,0.,1.);c*=mix(lit,.62,nt)*mix(vec3(1.),vec3(.55,.7,1.25),nt);if(ss>.5)c*=1.-(1.-shw(.004))*.45*clamp(sd.y*2.5+.2,0.,1.);}"
  "else if(tx<2.5){vec2 p=wp.xz;float dp=vc.r+wo,e=.25,fd=1.-smoothstep(25.,90.,d),h0=wv(p),hx=wv(p+vec2(e,0.)),hz=wv(p+vec2(0.,e));vec3 N=normalize(vec3((h0-hx)*2.*fd,1.,(h0-hz)*2.*fd)),V=normalize(ey-wp),L=normalize(sd);float nt=1.-clamp(L.y*4.+.6,0.,1.),fr=pow(1.-max(dot(N,V),0.),4.);"
  "vec3 base=mix(mix(vec3(.3,.78,.72),vec3(.08,.5,.72),clamp(dp/1.5,0.,1.)),vec3(.03,.22,.5),clamp((dp-1.5)/3.,0.,1.));vec3 Ls=L.y>0.?L:-L;float sp=pow(max(dot(reflect(-V,N),Ls),0.),140.);"
  "c=mix(base,fog*1.08,clamp(fr*.9+.1,0.,1.))+sp*mix(vec3(1.,.95,.8)*3.,vec3(.7,.8,1.)*1.2,nt)*clamp(Ls.y*3.,0.,1.);c*=mix(.5+.5*lit,.5,nt)*mix(vec3(1.),vec3(.6,.75,1.2),nt);"
  "float fm=1.-smoothstep(0.,.6,dp+(n(p*1.3+tm*.15)-.5)*.7);fm=clamp(fm*(.55+.45*sin(dp*10.+tm*1.8+n(p*.8)*5.)),0.,1.)*step(-.3,dp);c=mix(c,vec3(.95,.98,1.)*mix(.6+.4*lit,.55,nt),fm);a=max(clamp(.4+dp*.45,.4,.97),fm);}"
  "else if(tx>7.5){c=vc*(.8+.4*n(wp.xz*4.));float nt=1.-clamp(normalize(sd).y*4.+.6,0.,1.);c*=mix(lit,.62,nt)*mix(vec3(1.),vec3(.55,.7,1.25),nt);if(ss>.5)c*=1.-(1.-shw(.004))*.45*clamp(sd.y*2.5+.2,0.,1.);}"
  "else if(tx>6.5){vec3 rd=normalize(vc);if(rd.y<.02)discard;float t=(260.-ey.y)/rd.y;vec2 P=(ey.xz+rd.xz*t)*.0032+vec2(tm*.0035,tm*.0012);float d0=fb(P)+.08*(n(P*9.)-.5);vec2 sl=normalize(sd.xz+vec2(.0001,0.))*.03;float d1=fb(P+sl)+.08*(n(P*9.+sl*9.)-.5);"
  "float dens=smoothstep(.5,.76,d0),fade=smoothstep(.02,.2,rd.y),lg=clamp(.6+(d0-d1)*4.5,0.,1.),nt=1.-clamp(normalize(sd).y*4.+.6,0.,1.);vec3 lc=mix(vec3(.5,.58,.76),vec3(1.,.98,.94),lg)*(1.-.3*dens*dens);lc+=vec3(.3,.28,.22)*(1.-dens)*dens*2.*lg;c=lc*mix(.45+.55*lit,.4,nt)*mix(vec3(1.),vec3(.55,.65,1.1),nt);a=dens*fade*.96;}"
  "else if(tx>5.5){c=vc;}"
  "else if(tx>3.5){vec2 q=(vc.xy-.5)*2.;float r=length(q);vec3 tn=mix(vec3(1.,.93,.75),vec3(.72,.82,1.),vc.z);c=(tn*(exp(-r*6.)*.55+exp(-r*18.)*.8)+vec3(1.,1.,.96)*(1.-smoothstep(.055,.075,r))*1.6)*mix(1.,.6,vc.z);}"
  "float ff=tx>3.5?0.:clamp(d/110.,0.,1.);gl_FragColor=vec4(mix(c,fog,ff),a);}";

EM_JS(void, shfail, (const char* s), { alert('Shader error:\n' + UTF8ToString(s)); });
static GLuint sh(GLenum t, const char* s) {
  GLuint o = glCreateShader(t); glShaderSource(o, 1, &s, 0); glCompileShader(o);
  GLint ok = 0; glGetShaderiv(o, GL_COMPILE_STATUS, &ok);
  if (!ok) { char log[600]; glGetShaderInfoLog(o, 600, 0, log); shfail(log); }
  return o;
}
static void tv(float x, float y, float z, float r, float g, float b) { vb.push_back({x, y, z, r, g, b}); }
// @@WORLD_BEGIN  (pure CPU code, no GL: seed, terrain, platforms, roads, trails, road mask, minimap)
static unsigned SD = 0;   // world seed, mixed into every vn() so that every seed gives a different island
static float vn(int x, int z) { unsigned h = x*374761393u + z*668265263u + SD; h = (h ^ (h >> 13))*1274126177u; return ((h ^ (h >> 16)) & 0xffff)/65535.f; }
static float pn(float x, float z) {
  int ix = (int)floorf(x), iz = (int)floorf(z); float fx = x-ix, fz = z-iz; fx = fx*fx*(3-2*fx); fz = fz*fz*(3-2*fz);
  return (vn(ix,iz)*(1-fx) + vn(ix+1,iz)*fx)*(1-fz) + (vn(ix,iz+1)*(1-fx) + vn(ix+1,iz+1)*fx)*fz;
}
static float MS = 1.4f, bioRot = 0.f, WE = 616.f;   // map scale (1 = the original island), biome rotation, half-size of the world
static float biomeAt(float x, float z) {   // 0 temperate, 1 desert, 2 winter; continuous land zones, no artificial radial ring
  float bn = atan2f(z, x)/6.2832f + .5f + bioRot + (pn(x*.006f, z*.006f) - .5f)*.18f;
  bn -= floorf(bn);
  auto sm = [](float a, float b, float x) { float t=fminf(1.f,fmaxf(0.f,(x-a)/(b-a))); return t*t*(3.f-2.f*t); };
  float d = sm(.07f, .16f, bn) * (1.f - sm(.30f, .39f, bn));
  float sn = sm(.56f, .63f, bn) * (1.f - sm(.82f, .9f, bn));
  // Break the old ring look with broad, low-frequency land masks while keeping Seed variation.
  float land = .72f + .28f*pn(x*.0028f + 17.f, z*.0028f - 9.f);
  d *= land; sn *= .82f + .18f*pn(x*.0035f - 21.f, z*.0035f + 13.f);
  return d + 2.f*sn;
}
static inline float sst(float a, float b, float x) { float t = fminf(1.f, fmaxf(0.f, (x - a)/(b - a))); return t*t*(3.f - 2.f*t); }
static inline float rdg(float v) { v = 1.f - fabsf(2.f*v - 1.f); return v*v; }   // ridged noise: sharp crest where the value noise crosses .5
// natural terrain, island radius ~330*MS. Pure function of (x,z) and the seed, so every chunk / lattice node agrees (no seams).
// Layers: irregular coast -> plains / hill regions -> carved lowland valleys -> separate mountain massifs (ridged, domain warped,
// varying height) cut by passes and gorges -> broken rock ledges and cliffs -> sea cliffs -> fine surface roughness.
static float hgtB(float x, float z) {
  float ang = atan2f(z, x), cu = cosf(ang), su = sinf(ang), r = hypotf(x, z);
  float rr = 330.f*MS*(.72f + .45f*pn(cu*1.7f + 5, su*1.7f + 5) + .1f*(pn(cu*7.f, su*7.f) - .5f)), dc = rr - r;
  float coast = fminf(fmaxf(dc/33.f, 0.f), 1.f), inland = sst(25.f, 110.f, dc), centre = sst(20.f, 80.f, r);
  float ux = .8f*x - .6f*z, uz = .6f*x + .8f*z;   // rotated frame: the layers' value-noise lattices do not line up (no boxy shapes)
  float wx = ux + (pn(x*.006f + 3, z*.006f) - .5f)*120.f + (pn(uz*.017f, ux*.017f + 3) - .5f)*30.f, wz = uz + (pn(x*.006f, z*.006f + 8) - .5f)*120.f + (pn(ux*.017f + 9, uz*.017f) - .5f)*30.f;   // domain warp: bent ridges, no grid look
  float reg = pn(wx*.0036f + 17, wz*.0036f - 5)*.55f + pn(wz*.0052f - 3, wx*.0052f + 29)*.3f + pn(wx*.013f, wz*.013f + 21)*.15f;   // where the massifs are
  float mt = sst(.32f, .64f, reg)*inland*centre, m2 = mt*mt;
  float hil = sst(.3f, .72f, pn(x*.0045f - 9, z*.0045f + 13));                              // plains vs hill country
  float e = pn(x*.012f, z*.012f)*.55f + pn(x*.03f + 9, z*.03f)*.3f + pn(x*.07f, z*.07f + 4)*.15f;
  float h = (e - .32f)*(8.f + 20.f*hil)*coast + fminf(fmaxf(dc, -60.f), 120.f)*.06f - 1.f;
  // lowlands: long winding valleys pulled down towards a flat floor above the beach
  float vr = fabsf(pn(wx*.0065f + 31, wz*.0065f - 7) - .5f)*2.f, val = (1.f - sst(.03f, .22f, vr))*inland*(1.f - mt);
  float fl = 3.6f + 1.2f*pn(x*.02f, z*.02f + 30);
  if (h > fl) h += (fl - h)*val*.85f;
  float pv = fabsf(pn(wx*.0047f - 40, wz*.0047f + 40) - .5f)*2.f, pv2 = fabsf(pn(x*.0061f + 13, z*.0061f - 27) - .5f)*2.f;
  float pass = .03f + .97f*sst(.16f, .5f, pv)*sst(.13f, .4f, pv2);   // two crossing networks of passes / gorges
  // mountains: ridged multifractal, each massif with its own height; passes and gorges cut through them
  if (mt > 0.f) {
    float r1 = rdg(pn(wx*.011f, wz*.011f)), r2 = rdg(pn(wx*.027f + 5, wz*.027f)), r3 = rdg(pn(x*.062f, z*.062f + 2));
    float ridge = r1*.62f + r2*(.25f + .3f*r1)*.55f + r3*r1*.13f;
    float amp = 38.f + 62.f*pn(x*.0042f + 70, z*.0042f);
    h += (ridge*amp*m2*(.55f + .45f*mt) + 7.f*m2)*pass;
  }
  // cliffs and broken ledges: stepped risers with irregular edges, strongest in the mountains and in rocky hill patches
  float rk = pass*(1.f - val)*fminf(1.f, sst(.5f, .68f, pn(x*.017f + 3, z*.017f))*(.35f + .65f*hil) + m2*.6f*sst(.35f, .7f, pn(x*.021f - 4, z*.021f + 1)));
  if (h > 3.5f && rk > 0.f) {
    float jt = (pn(x*.045f, z*.045f + 9) - .5f)*.9f + (pn(x*.08f, z*.08f) - .5f)*.35f, st = 3.f + 5.f*pn(x*.011f - 12, z*.011f + 6), t = (h - 3.5f)/st + jt, f0 = floorf(t), f = t - f0;
    float hs = 3.5f + (f0 + f*.22f + sst(.5f, .8f, f)*.78f - jt)*st;
    h += (hs - h)*rk*.75f*sst(5.f, 10.f, h);
  }
  float cl = fminf(1.f, fmaxf(0.f, (pn(cu*2.3f + 11, su*2.3f + 11) - .48f)*7.f));   // sea cliffs
  if (cl > 0.f) { float top = 9.f + 12.f*pn(x*.012f, z*.012f + 7); h = fmaxf(h, h + (top - h)*cl*fminf(1.f, fmaxf(0.f, (dc - 14.f)/18.f))*(1.f - sst(45.f, 90.f, dc))); }
  h += ((pn(x*.075f + 1, z*.075f) - .5f)*1.1f + (pn(x*.095f, z*.095f + 5) - .5f)*(.45f + 1.2f*rk))*sst(2.5f, 6.f, h);   // surface roughness
  if (h < -.2f) h = -.2f + (h + .2f)*3.f;
  return h;
}
static const float AC = 2.5f; static int AN = 0; static float AO = 0.f;   // terrain adaptation lattice (roads / platforms level the ground), stored sparsely in 16x16 node blocks
struct ABlock { float w[256], wh[256]; unsigned short gen[256]; };   // weight, weight*height, id of the polyline that wrote the node
static vector<ABlock*> aTab; static int ABN = 0; static unsigned short aGen = 0;
static void adjInit(int an) { for (size_t i = 0; i < aTab.size(); i++) delete aTab[i]; AN = an; ABN = (an >> 4) + 1; aTab.assign((size_t)ABN*ABN, (ABlock*)0); }
static inline ABlock* adjBlk(int i, int j, bool make) {
  ABlock*& b = aTab[(size_t)(j >> 4)*ABN + (i >> 4)];
  if (!b && make) { b = new ABlock(); memset(b->w, 0, sizeof b->w); memset(b->wh, 0, sizeof b->wh); memset(b->gen, 0, sizeof b->gen); }
  return b;
}
static void adjAt(float x, float z, float& w, float& wh) {
  w = wh = 0.f; if (!AN) return;
  float fx = (x - AO)/AC, fz = (z - AO)/AC; int ix = (int)floorf(fx), iz = (int)floorf(fz);
  if (ix < 0 || iz < 0 || ix >= AN - 1 || iz >= AN - 1) return;
  float u = fx - ix, v = fz - iz, a[4] = {0, 0, 0, 0}, b[4] = {0, 0, 0, 0}; bool any = false;
  for (int k = 0; k < 4; k++) { int i = ix + (k & 1), j = iz + (k >> 1); const ABlock* B = aTab[(size_t)(j >> 4)*ABN + (i >> 4)]; if (B) { int q = (j & 15)*16 + (i & 15); a[k] = B->w[q]; b[k] = B->wh[q]; any = true; } }
  if (!any) return;
  w = (a[0]*(1 - u) + a[1]*u)*(1 - v) + (a[2]*(1 - u) + a[3]*u)*v;
  wh = (b[0]*(1 - u) + b[1]*u)*(1 - v) + (b[2]*(1 - u) + b[3]*u)*v;
}
static float hgt(float x, float z) {   // terrain height = natural terrain, levelled under roads, trails and platforms
  float h = hgtB(x, z), w, wh; adjAt(x, z, w, wh);
  if (w > 1e-4f) h += (wh/w - h)*fminf(w, 1.f);
  return h;
}
// ---- terrain cache: heights, normals, biome and water quads of every 80x80 unit block are computed once and shared by the mesh, physics, nodes and grass ----
static const int TCN = 16, TCV = TCN + 1;   // 16x16 cells of 5 units per block
struct TChunk { int cx, cz; unsigned vbo, wvbo; bool up, wup; int wn; float y0, y1; float h[TCV*TCV]; V v[TCV*TCV]; vector<V> wv; };
static unordered_map<long long, TChunk*> tcm; static vector<TChunk*> tcPool; static TChunk* tcLast = 0;
static inline long long ckey(int a, int b) { return ((long long)a << 32) ^ (long long)(unsigned)b; }
static TChunk* tcMake(int cx, int cz) {
  TChunk* c; if (!tcPool.empty()) { c = tcPool.back(); tcPool.pop_back(); } else { c = new TChunk(); c->vbo = c->wvbo = 0; }
  c->cx = cx; c->cz = cz; c->up = c->wup = false; c->wv.clear(); float y0 = 1e9f, y1 = -1e9f;
  for (int j = 0; j < TCV; j++) for (int i = 0; i < TCV; i++) {
    float x = (cx*TCN + i)*5.f, z = (cz*TCN + j)*5.f, h = hgt(x, z);
    float dx = hgt(x - 2, z) - hgt(x + 2, z), dz = hgt(x, z - 2) - hgt(x, z + 2), l = sqrtf(dx*dx + 16 + dz*dz);
    c->h[j*TCV + i] = h; V t = {x, h, z, .5f + .5f*fmaxf(0.f, (dx*.4f + 3.f - dz*.5f)/l), 4/l, biomeAt(x, z)}; c->v[j*TCV + i] = t;
    y0 = fminf(y0, h); y1 = fmaxf(y1, h);
  }
  for (int j = 0; j < TCN; j++) for (int i = 0; i < TCN; i++) {
    float a = c->h[j*TCV + i], b = c->h[j*TCV + i + 1], d = c->h[(j+1)*TCV + i], e = c->h[(j+1)*TCV + i + 1];
    if (fminf(fminf(a, b), fminf(d, e)) > .5f) continue;
    static const int Q[6][2] = {{0,0},{1,0},{1,1},{0,0},{1,1},{0,1}};
    for (int k = 0; k < 6; k++) { int ii = i + Q[k][0], jj = j + Q[k][1]; V t = {(cx*TCN + ii)*5.f, -.2f, (cz*TCN + jj)*5.f, -.2f - c->h[jj*TCV + ii], 0, 0}; c->wv.push_back(t); }
  }
  c->wn = (int)c->wv.size(); if (c->wn) y0 = fminf(y0, -.2f);
  c->y0 = y0; c->y1 = y1; return c;
}
static TChunk* tcGet(int cx, int cz) {   // find or build (the last hit is remembered: physics and node placement ask for the same block again and again)
  if (tcLast && tcLast->cx == cx && tcLast->cz == cz) return tcLast;
  long long k = ckey(cx, cz); auto it = tcm.find(k); if (it != tcm.end()) return tcLast = it->second;
  TChunk* c = tcMake(cx, cz); tcm[k] = c; return tcLast = c;
}
static float meshH(float x, float z, float* slope = 0) {   // height of the rendered terrain triangle at (x,z), read from the cache
  float fx = x/5.f, fz = z/5.f; int gx = (int)floorf(fx), gz = (int)floorf(fz); float u = fx - gx, v = fz - gz;
  const float* H = tcGet(gx >> 4, gz >> 4)->h + (gz & 15)*TCV + (gx & 15);
  float h00 = H[0], h10 = H[1], h01 = H[TCV], h11 = H[TCV + 1], h, sx, sz;
  if (u >= v) { h = h00 + (h10 - h00)*u + (h11 - h10)*v; sx = (h10 - h00)/5.f; sz = (h11 - h10)/5.f; }
  else { h = h00 + (h11 - h01)*u + (h01 - h00)*v; sx = (h11 - h01)/5.f; sz = (h01 - h00)/5.f; }
  if (slope) *slope = sqrtf(sx*sx + sz*sz);
  return h;
}

// ===================== procedural world: platforms, roads, trails (pure CPU, no GL) =====================
struct Poly { int type; vector<float> x, z, h, s; };   // type 0 = road, 1 = trail; points every ~2 units
// Platform = placeholder for a future monument (RT): centre x,z, rotation yaw, half sizes hx,hz, ground height h. Roads lead to its centre, trees keep off it.
// Later a real RT can simply be placed on every entry of plats[] (platProps() draws the placeholder pole / flag / bollards).
struct Plat { float x, z, yaw, hx, hz, h; };
struct Cfg { double seed; float size; int plat, loops, trails; };
static vector<Poly> polys; static vector<Plat> plats;
// ---- tunables (the seed, map size and the number of platforms / roads / trails come from the menu in index.html) ----
// RHW/RSH: half width of the asphalt / of the gravel shoulder.  THW/TSH: the same for a trail.  PHX/PHZ: half size of a platform (24 x 18 units).
static const float RHW = 3.f, RSH = 1.6f, THW = .8f, TSH = .8f, PHX = 12.f, PHZ = 9.f;
static const float RMINH = 3.4f, TMINH = 3.f;          // roads / trails never go lower than this (the beach sand ends here)
static float spX = 0, spZ = 0, spYaw = 0;              // spawn point (next to a road)
static struct { int nPlat, nRoad, nTrail, unlinked; float roadLen, trailLen; } WS;
static unsigned gsd = 1;
static float grnd() { gsd ^= gsd << 13; gsd ^= gsd >> 17; gsd ^= gsd << 5; return (gsd & 0xffffff)/16777216.f; }
static float sstep(float a, float b, float x) { float t = fminf(fmaxf((x - a)/(b - a), 0.f), 1.f); return t*t*(3.f - 2.f*t); }
static float segD(float x, float z, float x0, float z0, float x1, float z1, float* tt = 0) {   // distance point -> segment
  float dx = x1 - x0, dz = z1 - z0, l2 = dx*dx + dz*dz, t = l2 > 1e-8f ? ((x - x0)*dx + (z - z0)*dz)/l2 : 0.f; t = fminf(fmaxf(t, 0.f), 1.f);
  if (tt) *tt = t; return hypotf(x - (x0 + dx*t), z - (z0 + dz*t));
}
static float rectSdf(const Plat& p, float x, float z) {   // signed distance to the platform rectangle (negative inside)
  float c = cosf(p.yaw), s = sinf(p.yaw), dx = x - p.x, dz = z - p.z, lx = dx*c - dz*s, lz = dx*s + dz*c, qx = fabsf(lx) - p.hx, qz = fabsf(lz) - p.hz;
  return hypotf(fmaxf(qx, 0.f), fmaxf(qz, 0.f)) + fminf(fmaxf(qx, qz), 0.f);
}
// ---- terrain adaptation: flatten the ground under roads and platforms ----
static void adjPut(int i, int j, float w, float ht) {
  ABlock* B = adjBlk(i, j, true); int q = (j & 15)*16 + (i & 15);
  float wo = B->w[q], ho = wo > 1e-5f ? B->wh[q]/wo : ht, a = wo*wo, b = w*w, hn = a + b > 1e-9f ? (a*ho + b*ht)/(a + b) : ht, wn = fmaxf(wo, w);
  B->w[q] = wn; B->wh[q] = wn*hn; B->gen[q] = aGen;
}
static void stampSeg(float x0, float z0, float h0, float x1, float z1, float h1, float core, float blend, float wmax, bool lockOld) {   // lockOld: nodes that an older polyline / platform already levelled completely are left alone
  float R = core + blend;
  int i0 = max(0, (int)floorf((fminf(x0, x1) - R - AO)/AC)), i1 = min(AN - 1, (int)ceilf((fmaxf(x0, x1) + R - AO)/AC));
  int j0 = max(0, (int)floorf((fminf(z0, z1) - R - AO)/AC)), j1 = min(AN - 1, (int)ceilf((fmaxf(z0, z1) + R - AO)/AC));
  for (int j = j0; j <= j1; j++) for (int i = i0; i <= i1; i++) {
    if (lockOld) { const ABlock* B = adjBlk(i, j, false); if (B) { int q = (j & 15)*16 + (i & 15); if (B->w[q] >= .999f && B->gen[q] != aGen) continue; } }
    float t, d = segD(AO + i*AC, AO + j*AC, x0, z0, x1, z1, &t); if (d >= R) continue;
    adjPut(i, j, wmax*(1.f - sstep(core, R, d)), h0 + (h1 - h0)*t);
  }
}
static void stampPlat(const Plat& p, float core, float blend) {
  float R = hypotf(p.hx, p.hz) + core + blend;
  int i0 = max(0, (int)floorf((p.x - R - AO)/AC)), i1 = min(AN - 1, (int)ceilf((p.x + R - AO)/AC)), j0 = max(0, (int)floorf((p.z - R - AO)/AC)), j1 = min(AN - 1, (int)ceilf((p.z + R - AO)/AC));
  for (int j = j0; j <= j1; j++) for (int i = i0; i <= i1; i++) {
    float d = rectSdf(p, AO + i*AC, AO + j*AC) - core; adjPut(i, j, 1.f - sstep(0.f, blend, fmaxf(d, 0.f)), p.h);
  }
}
// ---- keep-clear map (trees / rocks stay off roads and platforms), sparse 32x32 cell blocks ----
static const float CC = 2.f; static int CN = 0, CBN = 0; static vector<unsigned char*> cTab;
static void clrInit(int cn) { for (size_t i = 0; i < cTab.size(); i++) delete[] cTab[i]; CN = cn; CBN = (cn >> 5) + 1; cTab.assign((size_t)CBN*CBN, (unsigned char*)0); }
static inline void clrSet(int i, int j) { unsigned char*& b = cTab[(size_t)(j >> 5)*CBN + (i >> 5)]; if (!b) { b = new unsigned char[1024]; memset(b, 0, 1024); } b[(j & 31)*32 + (i & 31)] = 1; }
static void clrSeg(float x0, float z0, float x1, float z1, float r) {
  int i0 = max(0, (int)floorf((fminf(x0, x1) - r + WE)/CC)), i1 = min(CN - 1, (int)floorf((fmaxf(x0, x1) + r + WE)/CC));
  int j0 = max(0, (int)floorf((fminf(z0, z1) - r + WE)/CC)), j1 = min(CN - 1, (int)floorf((fmaxf(z0, z1) + r + WE)/CC));
  for (int j = j0; j <= j1; j++) for (int i = i0; i <= i1; i++) if (segD(-WE + (i + .5f)*CC, -WE + (j + .5f)*CC, x0, z0, x1, z1) <= r) clrSet(i, j);
}
static void clrPlat(const Plat& p, float m) {
  float R = hypotf(p.hx, p.hz) + m + 2.f;
  int i0 = max(0, (int)floorf((p.x - R + WE)/CC)), i1 = min(CN - 1, (int)floorf((p.x + R + WE)/CC)), j0 = max(0, (int)floorf((p.z - R + WE)/CC)), j1 = min(CN - 1, (int)floorf((p.z + R + WE)/CC));
  for (int j = j0; j <= j1; j++) for (int i = i0; i <= i1; i++) if (rectSdf(p, -WE + (i + .5f)*CC, -WE + (j + .5f)*CC) <= m) clrSet(i, j);
}
static bool blockedAt(float x, float z) {
  if (!CN) return false; int i = (int)floorf((x + WE)/CC), j = (int)floorf((z + WE)/CC);
  if (i < 0 || j < 0 || i >= CN || j >= CN) return false;
  const unsigned char* b = cTab[(size_t)(j >> 5)*CBN + (i >> 5)]; return b && b[(j & 31)*32 + (i & 31)];
}
// ---- route-finding grid ----
static const float GC = 4.f; static int GN = 0; static float GO = 0.f;
static vector<float> gH, gS, gNz, gRd, gTd; static vector<unsigned char> gOk, gPl;   // gOk bits: 1 road, 2 trail, 4 relaxed road; gPl = platform id + 1
static int cellAt(float x, float z) { int i = (int)floorf((x - GO)/GC), j = (int)floorf((z - GO)/GC); i = min(max(i, 0), GN - 1); j = min(max(j, 0), GN - 1); return j*GN + i; }
static float sampG(const vector<float>& g, float x, float z) {   // bilinear sample of a per-cell field
  float fx = (x - GO)/GC - .5f, fz = (z - GO)/GC - .5f; int i = (int)floorf(fx), j = (int)floorf(fz); float u = fx - i, v = fz - j;
  int i1 = min(max(i + 1, 0), GN - 1), j1 = min(max(j + 1, 0), GN - 1); i = min(max(i, 0), GN - 1); j = min(max(j, 0), GN - 1);
  return (g[j*GN + i]*(1 - u) + g[j*GN + i1]*u)*(1 - v) + (g[j1*GN + i]*(1 - u) + g[j1*GN + i1]*u)*v;
}
static float mulAt(int c, int cls, int pa, int pb) {   // cost multiplier of a cell for a route class (0 road, 1 trail, 2 relaxed road); < 0 = impassable
  if (!(gOk[c] & (cls == 0 ? 1 : cls == 1 ? 2 : 4))) return -1.f;
  unsigned char pl = gPl[c]; bool foreign = pl && pl != pa + 1 && pl != pb + 1; if (foreign && cls == 1) return -1.f;   // trails keep out of platforms
  float s = gS[c], m = (1.f + 5.f*s*s)*(1.f + .5f*gNz[c])*(foreign ? .9f : 1.f), dr = gRd[c], dt = gTd[c];                 // roads simply drive through a platform that is in the way
  if (cls != 1) { if (dr < 2.8f) m *= .3f; else if (dr < 16.f) m *= 1.f + 2.4f*(1.f - (dr - 2.8f)/13.2f); }          // merge into roads, don't run beside them
  else {
    if (dr < 2.8f) m *= .5f; else if (dr < 12.f) m *= 1.f + 1.6f*(1.f - (dr - 2.8f)/9.2f);
    if (dt < 2.4f) m *= .4f; else if (dt < 8.f) m *= 1.f + 1.f*(1.f - (dt - 2.4f)/5.6f);
  }
  return m;
}
static bool astar(int s, int t, int cls, int pa, int pb, vector<int>& out) {   // 16-neighbour A* (knight moves keep the paths from looking grid-like)
  static vector<float> G; static vector<int> P; static vector<unsigned char> C;
  struct Q { float f; int c; bool operator<(const Q& o) const { return f > o.f; } };
  static const int D[16][2] = {{1,0},{-1,0},{0,1},{0,-1},{1,1},{1,-1},{-1,1},{-1,-1},{2,1},{2,-1},{-2,1},{-2,-1},{1,2},{-1,2},{1,-2},{-1,-2}};
  int N = GN*GN, tx = t % GN, tz = t / GN, guard = 0; G.assign(N, 1e30f); P.assign(N, -1); C.assign(N, 0);
  priority_queue<Q> pq; G[s] = 0.f; pq.push({0.f, s});
  while (!pq.empty()) {
    Q q = pq.top(); pq.pop(); int c = q.c; if (C[c]) continue; C[c] = 1; if (c == t) break; if (++guard > 700000) break;
    int ci = c % GN, cj = c / GN; float mc = mulAt(c, cls, pa, pb); if (mc < 0.f) mc = 1.f;
    for (int k = 0; k < 16; k++) {
      int dx = D[k][0], dz = D[k][1], ni = ci + dx, nj = cj + dz; if (ni < 0 || nj < 0 || ni >= GN || nj >= GN) continue;
      int n2 = nj*GN + ni; if (C[n2]) continue;
      float mn = mulAt(n2, cls, pa, pb); if (mn < 0.f) continue;
      int ax = dx < 0 ? -dx : dx, az = dz < 0 ? -dz : dz; bool pass = true;   // no corner cutting through blocked cells
      if (ax == 1 && az == 1) pass = mulAt(cj*GN + ni, cls, pa, pb) >= 0.f && mulAt(nj*GN + ci, cls, pa, pb) >= 0.f;
      else if (ax == 2) { int sx = dx > 0 ? 1 : -1; pass = mulAt(cj*GN + ci + sx, cls, pa, pb) >= 0.f && mulAt(nj*GN + ci + sx, cls, pa, pb) >= 0.f; }
      else if (az == 2) { int sz = dz > 0 ? 1 : -1; pass = mulAt((cj + sz)*GN + ci, cls, pa, pb) >= 0.f && mulAt((cj + sz)*GN + ni, cls, pa, pb) >= 0.f; }
      if (!pass) continue;
      float len = sqrtf((float)(dx*dx + dz*dz))*GC, dh = fabsf(gH[n2] - gH[c])/len, ng = G[c] + len*.5f*(mc + mn)*(1.f + 14.f*dh*dh);
      if (ng < G[n2]) { G[n2] = ng; P[n2] = c; pq.push({ng + .55f*hypotf((float)(ni - tx), (float)(nj - tz))*GC, n2}); }
    }
  }
  if (!C[t]) return false;
  out.clear(); for (int c = t; c >= 0; c = P[c]) out.push_back(c); reverse(out.begin(), out.end()); return true;
}
static void chaikin(vector<float>& X, vector<float>& Z) {   // corner cutting, end points stay fixed
  vector<float> nx, nz; nx.push_back(X[0]); nz.push_back(Z[0]);
  for (size_t i = 0; i + 1 < X.size(); i++) {
    nx.push_back(.75f*X[i] + .25f*X[i+1]); nz.push_back(.75f*Z[i] + .25f*Z[i+1]);
    nx.push_back(.25f*X[i] + .75f*X[i+1]); nz.push_back(.25f*Z[i] + .75f*Z[i+1]);
  }
  nx.push_back(X.back()); nz.push_back(Z.back()); X.swap(nx); Z.swap(nz);
}
static void resample(vector<float>& X, vector<float>& Z, float sp) {   // uniform spacing along the polyline
  vector<float> nx, nz; nx.push_back(X[0]); nz.push_back(Z[0]); float carry = 0.f;
  for (size_t i = 0; i + 1 < X.size(); i++) {
    float ax = X[i], az = Z[i], bx = X[i+1], bz = Z[i+1], len = hypotf(bx - ax, bz - az); if (len < 1e-5f) continue;
    float d = sp - carry;
    while (d <= len) { float t = d/len; nx.push_back(ax + (bx - ax)*t); nz.push_back(az + (bz - az)*t); d += sp; }
    carry = len - (d - sp);
  }
  if (hypotf(nx.back() - X.back(), nz.back() - Z.back()) > .4f) { nx.push_back(X.back()); nz.push_back(Z.back()); } else { nx.back() = X.back(); nz.back() = Z.back(); }
  X.swap(nx); Z.swap(nz);
}
static void profile(Poly& P) {   // height of the road surface along the route: smooth, limited grade, limited cut / fill
  int n = (int)P.x.size(); bool road = P.type == 0; vector<float> hb(n), h(n); vector<char> pin(n, 0);
  for (int i = 0; i < n; i++) { float w, wh; adjAt(P.x[i], P.z[i], w, wh); hb[i] = hgt(P.x[i], P.z[i]); pin[i] = w > .65f; }   // pinned: already on a platform / another road
  h = hb; float cut = road ? 2.4f : 1.f, minH = road ? RMINH : TMINH, grade = road ? .22f : .4f;
  for (int it = 0; it < 90; it++) for (int i = 1; i + 1 < n; i++) if (!pin[i]) {
    h[i] += ((h[i-1] + h[i+1])*.5f - h[i])*.5f; h[i] = fmaxf(fminf(fmaxf(h[i], hb[i] - cut), hb[i] + cut), minH);
  }
  vector<float> lim(n, grade);   // per-segment grade limit; between two pinned points it is raised just enough to make the join feasible
  for (int a = 0, i = 1; i < n; i++) if (pin[i] || i == n - 1) {
    if (pin[a] && pin[i] && i > a + 1) { float g = 1.15f*fabsf(h[i] - h[a])/fmaxf(P.s[i] - P.s[a], 1.f); if (g > grade) for (int k = a + 1; k <= i; k++) lim[k] = g; }
    a = i;
  }
  for (int pass = 0; pass < 3; pass++) {
    for (int i = 1; i < n; i++) if (!pin[i]) { float l = lim[i]*hypotf(P.x[i] - P.x[i-1], P.z[i] - P.z[i-1]); h[i] = fminf(fmaxf(h[i], h[i-1] - l), h[i-1] + l); }
    for (int i = n - 2; i >= 0; i--) if (!pin[i]) { float l = lim[i+1]*hypotf(P.x[i+1] - P.x[i], P.z[i+1] - P.z[i]); h[i] = fminf(fmaxf(h[i], h[i+1] - l), h[i+1] + l); }
  }
  P.h = h;
}
static void finalizePoly(Poly& P) {   // arc length, height profile, flatten the terrain, keep-clear, distance fields
  int n = (int)P.x.size(); if (n < 2) return;
  P.s.assign(n, 0.f); for (int i = 1; i < n; i++) P.s[i] = P.s[i-1] + hypotf(P.x[i] - P.x[i-1], P.z[i] - P.z[i-1]);
  profile(P);
  bool road = P.type == 0; float core = road ? RHW + RSH + 2.4f : THW + 1.f, blend = road ? 9.f : 4.5f, wmax = road ? 1.f : .6f, kc = road ? 5.6f : 1.8f, dr = road ? 18.f : 9.f;
  vector<float>& DF = road ? gRd : gTd; aGen = (unsigned short)(aGen + 1); if (!aGen) aGen = 1;   // every polyline gets its own id (platforms use 0)
  for (int i = 0; i + 1 < n; i++) {
    float x0 = P.x[i], z0 = P.z[i], x1 = P.x[i+1], z1 = P.z[i+1];
    stampSeg(x0, z0, P.h[i], x1, z1, P.h[i+1], core, blend, wmax, true); clrSeg(x0, z0, x1, z1, kc);
    int i0 = max(0, (int)floorf((fminf(x0, x1) - dr - GO)/GC)), i1 = min(GN - 1, (int)floorf((fmaxf(x0, x1) + dr - GO)/GC)), j0 = max(0, (int)floorf((fminf(z0, z1) - dr - GO)/GC)), j1 = min(GN - 1, (int)floorf((fmaxf(z0, z1) + dr - GO)/GC));
    for (int j = j0; j <= j1; j++) for (int ii = i0; ii <= i1; ii++) { float d = segD(GO + (ii + .5f)*GC, GO + (j + .5f)*GC, x0, z0, x1, z1); if (d < DF[j*GN + ii]) DF[j*GN + ii] = d; }
  }
  polys.push_back(P);
}
static float addSections(vector<float>& X, vector<float>& Z, int type) {   // keep only the parts that are not on an existing road / trail
  int n = (int)X.size(); vector<char> on(n); float added = 0.f;
  for (int i = 0; i < n; i++) { float dr = sampG(gRd, X[i], Z[i]); on[i] = type == 0 ? dr < 2.8f : (dr < 2.8f || sampG(gTd, X[i], Z[i]) < 2.4f); }
  int i = 0;
  while (i < n) {
    if (on[i]) { i++; continue; }
    int a = i; while (i < n && !on[i]) i++; int b = i - 1, s0 = a > 0 ? a - 1 : a, s1 = b + 1 < n ? b + 1 : b; float len = 0.f;
    for (int k = s0; k < s1; k++) len += hypotf(X[k+1] - X[k], Z[k+1] - Z[k]);
    if (len < 6.f) continue;
    Poly P; P.type = type; for (int k = s0; k <= s1; k++) { P.x.push_back(X[k]); P.z.push_back(Z[k]); }
    finalizePoly(P); added += len;
  }
  return added;
}
static float route(float x0, float z0, float x1, float z1, int cls, int pa, int pb, int type) {   // returns new length, or -1 if there is no way
  int s = cellAt(x0, z0), t = cellAt(x1, z1); vector<int> path; if (!astar(s, t, cls, pa, pb, path)) return -1.f;
  vector<float> X, Z; X.push_back(x0); Z.push_back(z0);
  for (size_t k = 1; k + 1 < path.size(); k++) { X.push_back(GO + (path[k] % GN + .5f)*GC); Z.push_back(GO + (path[k]/GN + .5f)*GC); }
  X.push_back(x1); Z.push_back(z1);
  for (int k = 0; k < 3; k++) chaikin(X, Z);
  resample(X, Z, 2.f);
  for (int it = 0; it < 6; it++) for (size_t k = 1; k + 1 < X.size(); k++) { X[k] = (X[k-1] + 2*X[k] + X[k+1])*.25f; Z[k] = (Z[k-1] + 2*Z[k] + Z[k+1])*.25f; }
  return addSections(X, Z, type);
}
// ---- platforms ----
static bool siteOk(float x, float z, float& hm, float& rg) {
  float mn = 1e9f, mx = -1e9f, sum = 0.f;
  for (int i = -2; i <= 2; i++) for (int j = -2; j <= 2; j++) { float h = hgtB(x + i*9.f, z + j*9.f); if (h < 4.2f) return false; mn = fminf(mn, h); mx = fmaxf(mx, h); sum += h; }
  hm = sum/25.f; rg = mx - mn; return mx <= 45.f && rg <= 6.f;
}
static void placePlats(int N) {   // best-candidate sampling: every new platform goes as far as possible from the others
  plats.clear(); if (N <= 0) return;
  int land = 0; for (size_t i = 0; i < gH.size(); i++) if (gH[i] >= 4.f) land++;
  float minD = N > 1 ? .6f*sqrtf((float)land*GC*GC/N) : 0.f;
  for (int k = 0; k < N; k++) for (int relax = 0; relax < 6; relax++) {
    float bs = -1e9f, bx = 0, bz = 0, bh = 0; bool found = false;
    for (int tr = 0; tr < 80; tr++) {
      float x = (grnd()*2 - 1)*WE*.9f, z = (grnd()*2 - 1)*WE*.9f, hm, rg; if (!siteOk(x, z, hm, rg)) continue;
      float dmin = 1e9f; for (size_t q = 0; q < plats.size(); q++) dmin = fminf(dmin, hypotf(plats[q].x - x, plats[q].z - z));
      if (dmin < minD*(1.f - .17f*relax)) continue;
      float sc = fminf(dmin, 900.f) - 6.f*rg; if (sc > bs) { bs = sc; bx = x; bz = z; bh = hm; found = true; }
    }
    if (found) { Plat p = {bx, bz, grnd()*6.2832f, PHX, PHZ, bh}; plats.push_back(p); break; }
  }
}
static void makeTrails(int T) {   // trails start at any point of any road, on either side, and wander off over the land
  float tot = 0.f; for (size_t k = 0; k < polys.size(); k++) if (polys[k].type == 0) tot += polys[k].s.back();
  if (tot < 20.f) return;
  for (int k = 0; k < T; k++) for (int at = 0; at < 10; at++) {
    float r = grnd()*tot, acc = 0.f; int pi = -1;
    for (size_t q = 0; q < polys.size(); q++) if (polys[q].type == 0) { acc += polys[q].s.back(); pi = (int)q; if (r <= acc) break; }
    if (pi < 0) return;
    int n = (int)polys[pi].x.size(); if (n < 8) continue;
    int i = 3 + (int)(grnd()*(n - 6)); float sx = polys[pi].x[i], sz = polys[pi].z[i], tx = 0, tz = 0; bool ok = false;
    for (int q = 0; q < 24 && !ok; q++) {
      float a = grnd()*6.2832f, d = 60.f + grnd()*190.f; tx = sx + cosf(a)*d; tz = sz + sinf(a)*d;
      if (fabsf(tx) > WE*.95f || fabsf(tz) > WE*.95f) continue;
      int c = cellAt(tx, tz); if (!(gOk[c] & 2) || gPl[c] || gRd[c] < 22.f || gTd[c] < 12.f) continue; ok = true;
    }
    if (!ok) continue;
    if (route(sx, sz, tx, tz, 1, -1, -1, 1) > 25.f) break;
  }
}
static void pickSpawn() {   // next to the road that is closest to the centre of the island, looking at it
  float bd = 1e30f; int bp = -1, bi = 0;
  for (size_t q = 0; q < polys.size(); q++) if (polys[q].type == 0) for (int i = 4; i + 4 < (int)polys[q].x.size(); i += 2) { float d = hypotf(polys[q].x[i], polys[q].z[i]); if (d < bd) { bd = d; bp = (int)q; bi = i; } }
  if (bp >= 0) {
    const Poly& P = polys[bp]; float tx = P.x[bi + 2] - P.x[bi - 2], tz = P.z[bi + 2] - P.z[bi - 2], l = hypotf(tx, tz) + 1e-6f; tx /= l; tz /= l;
    for (int sg = 1; sg >= -1; sg -= 2) {
      float sx = P.x[bi] - tz*sg*7.f, sz = P.z[bi] + tx*sg*7.f;
      if (hgt(sx, sz) >= 3.f) { spX = sx; spZ = sz; spYaw = atan2f(-(P.x[bi] - sx), -(P.z[bi] - sz)); return; }
    }
    spX = P.x[bi]; spZ = P.z[bi]; spYaw = atan2f(-tx, -tz); return;
  }
  for (float r = 0; r < WE; r += 8.f) for (int k = 0; k < 16; k++) { float a = k*.3927f, x = cosf(a)*r, z = sinf(a)*r; if (hgt(x, z) >= 3.5f) { spX = x; spZ = z; spYaw = 0; return; } }
  spX = spZ = spYaw = 0;
}
static void genWorld(const Cfg& c) {
  unsigned sd = (unsigned)c.seed; SD = sd*2654435761u + 0x9E3779B9u; SD ^= SD >> 15; SD *= 2246822519u; SD ^= SD >> 13;
  gsd = SD | 1u; MS = fminf(fmaxf(c.size, .6f), 2.4f); WE = 440.f*MS; bioRot = ((SD >> 5) & 0xffff)/65536.f;
  polys.clear(); plats.clear(); WS.nPlat = WS.nRoad = WS.nTrail = WS.unlinked = 0; WS.roadLen = WS.trailLen = 0.f;
  aGen = 0; adjInit((int)ceilf(2*WE/AC) + 3); AO = -WE - AC;
  clrInit((int)ceilf(2*WE/CC) + 1);
  GN = (int)ceilf(2*WE/GC); GO = -WE; int NC = GN*GN;
  gH.assign(NC, 0.f); gS.assign(NC, 0.f); gNz.assign(NC, 0.f); gRd.assign(NC, 1e9f); gTd.assign(NC, 1e9f); gOk.assign(NC, 0); gPl.assign(NC, 0);
  for (int j = 0; j < GN; j++) for (int i = 0; i < GN; i++) { float x = GO + (i + .5f)*GC, z = GO + (j + .5f)*GC; gH[j*GN + i] = hgtB(x, z); gNz[j*GN + i] = pn(x*.02f + 7.f, z*.02f + 3.f); }
  for (int j = 0; j < GN; j++) for (int i = 0; i < GN; i++) {
    int i0 = max(i - 1, 0), i1 = min(i + 1, GN - 1), j0 = max(j - 1, 0), j1 = min(j + 1, GN - 1);
    gS[j*GN + i] = hypotf((gH[j*GN + i1] - gH[j*GN + i0])/((i1 - i0)*GC), (gH[j1*GN + i] - gH[j0*GN + i])/((j1 - j0)*GC));
  }
  {
    vector<unsigned char> rl(NC), tr(NC), r1(NC);
    for (int c2 = 0; c2 < NC; c2++) { r1[c2] = gH[c2] >= RMINH && gS[c2] <= .42f; rl[c2] = gH[c2] >= RMINH - .3f && gS[c2] <= .6f; tr[c2] = gH[c2] >= TMINH && gS[c2] <= .7f; }
    auto erode = [&](int R, int i, int j) { for (int dj = -R; dj <= R; dj++) for (int di = -R; di <= R; di++) { int ii = i + di, jj = j + dj; if (ii < 0 || jj < 0 || ii >= GN || jj >= GN || !rl[jj*GN + ii]) return false; } return true; };
    for (int j = 0; j < GN; j++) for (int i = 0; i < GN; i++) {
      int c2 = j*GN + i; unsigned char f = 0;
      if (r1[c2] && erode(2, i, j)) f |= 1;     // roads keep 8+ units away from the beach and from steep ground
      if (tr[c2]) f |= 2;
      if (rl[c2] && erode(1, i, j)) f |= 4;
      gOk[c2] = f;
    }
  }
  placePlats(min(max(c.plat, 0), 24));
  for (size_t k = 0; k < plats.size(); k++) {
    const Plat& p = plats[k]; stampPlat(p, 2.f, 14.f); clrPlat(p, 3.f);
    float R = hypotf(p.hx, p.hz) + 6.f;
    for (int j = max(0, (int)floorf((p.z - R - GO)/GC)); j <= min(GN - 1, (int)floorf((p.z + R - GO)/GC)); j++) for (int i = max(0, (int)floorf((p.x - R - GO)/GC)); i <= min(GN - 1, (int)floorf((p.x + R - GO)/GC)); i++)
      if (hypotf(GO + (i + .5f)*GC - p.x, GO + (j + .5f)*GC - p.z) <= R) { gPl[j*GN + i] = (unsigned char)(k + 1); gOk[j*GN + i] |= 7; }
  }
  int n = (int)plats.size(); WS.nPlat = n;
  if (n > 1) {
    vector<int> uf(n); for (int i = 0; i < n; i++) uf[i] = i;
    auto fnd = [&](int a) { while (uf[a] != a) { uf[a] = uf[uf[a]]; a = uf[a]; } return a; };
    vector<pair<float, pair<int, int> > > mst, extra; vector<char> in(n, 0); vector<float> best(n, 1e30f); vector<int> from(n, 0); in[0] = 1;
    for (int j = 1; j < n; j++) best[j] = hypotf(plats[j].x - plats[0].x, plats[j].z - plats[0].z);
    for (int it = 1; it < n; it++) {
      int b = -1; for (int j = 0; j < n; j++) if (!in[j] && (b < 0 || best[j] < best[b])) b = j;
      in[b] = 1; mst.push_back(make_pair(best[b], make_pair(from[b], b)));
      for (int j = 0; j < n; j++) if (!in[j]) { float d = hypotf(plats[j].x - plats[b].x, plats[j].z - plats[b].z); if (d < best[j]) { best[j] = d; from[j] = b; } }
    }
    float pl = c.loops <= 0 ? 0.f : c.loops == 1 ? .4f : .85f;   // how many loops (extra roads between near neighbours) on top of the spanning tree
    for (int i = 0; i < n; i++) {
      vector<pair<float, int> > nb; for (int j = 0; j < n; j++) if (j != i) nb.push_back(make_pair(hypotf(plats[j].x - plats[i].x, plats[j].z - plats[i].z), j));
      sort(nb.begin(), nb.end());
      for (int q = 0; q < 3 && q < (int)nb.size(); q++) {
        int j = nb[q].second, a = min(i, j), b = max(i, j); bool dup = false;
        for (size_t e = 0; e < mst.size(); e++) if (min(mst[e].second.first, mst[e].second.second) == a && max(mst[e].second.first, mst[e].second.second) == b) dup = true;
        for (size_t e = 0; e < extra.size(); e++) if (extra[e].second.first == a && extra[e].second.second == b) dup = true;
        if (!dup && grnd() < pl) extra.push_back(make_pair(nb[q].first, make_pair(a, b)));
      }
    }
    sort(mst.begin(), mst.end()); sort(extra.begin(), extra.end());
    for (size_t e = 0; e < mst.size(); e++) {
      int a = mst[e].second.first, b = mst[e].second.second; float add = route(plats[a].x, plats[a].z, plats[b].x, plats[b].z, 0, a, b, 0);
      if (add < 0.f) add = route(plats[a].x, plats[a].z, plats[b].x, plats[b].z, 2, a, b, 0);
      if (add >= 0.f) uf[fnd(a)] = fnd(b);
    }
    for (size_t e = 0; e < extra.size(); e++) { int a = extra[e].second.first, b = extra[e].second.second; float add = route(plats[a].x, plats[a].z, plats[b].x, plats[b].z, 0, a, b, 0); if (add < 0.f) route(plats[a].x, plats[a].z, plats[b].x, plats[b].z, 2, a, b, 0); }
    for (int i = 1; i < n; i++) if (fnd(i) != fnd(0)) {   // anything still cut off: try to hook it to its nearest connected neighbour
      int bj = -1; float bd = 1e30f; for (int j = 0; j < n; j++) if (fnd(j) == fnd(0)) { float d = hypotf(plats[j].x - plats[i].x, plats[j].z - plats[i].z); if (d < bd) { bd = d; bj = j; } }
      if (bj >= 0 && route(plats[i].x, plats[i].z, plats[bj].x, plats[bj].z, 2, i, bj, 0) >= 0.f) uf[fnd(i)] = fnd(bj); else WS.unlinked++;
    }
  }
  makeTrails(min(max(c.trails, 0), 40));
  for (size_t q = 0; q < polys.size(); q++) { if (polys[q].type == 0) { WS.nRoad++; WS.roadLen += polys[q].s.back(); } else { WS.nTrail++; WS.trailLen += polys[q].s.back(); } }
  pickSpawn();
  vector<float>().swap(gS); vector<float>().swap(gNz); vector<float>().swap(gRd); vector<float>().swap(gTd); vector<unsigned char>().swap(gOk); vector<unsigned char>().swap(gPl);
}
// ---- road mask: a 256x256 unit window around the player, 512x512 texels (RGBA = road, trail, platform, markings), sampled by the terrain shader ----
static const int RWN = 512; static const float RWS = 256.f;
static unsigned char rbuf[RWN*RWN*4]; static float rox = 1e9f, roz = 1e9f;
static void rasterWindow(float cx, float cz) {
  rox = floorf(cx/32.f + .5f)*32.f - RWS*.5f; roz = floorf(cz/32.f + .5f)*32.f - RWS*.5f; memset(rbuf, 0, sizeof rbuf);
  const float T = RWS/RWN, x1w = rox + RWS, z1w = roz + RWS;
  for (size_t q = 0; q < polys.size(); q++) {
    const Poly& P = polys[q]; bool road = P.type == 0; float hw = road ? RHW : THW, wr = road ? RSH : TSH, reach = hw + wr + .6f; int n = (int)P.x.size();
    for (int k = 0; k + 1 < n; k++) {
      float ax = P.x[k], az = P.z[k], bx = P.x[k+1], bz = P.z[k+1];
      float mnx = fminf(ax, bx) - reach, mxx = fmaxf(ax, bx) + reach, mnz = fminf(az, bz) - reach, mxz = fmaxf(az, bz) + reach;
      if (mxx < rox || mnx > x1w || mxz < roz || mnz > z1w) continue;
      int i0 = max(0, (int)floorf((mnx - rox)/T)), i1 = min(RWN - 1, (int)ceilf((mxx - rox)/T)), j0 = max(0, (int)floorf((mnz - roz)/T)), j1 = min(RWN - 1, (int)ceilf((mxz - roz)/T));
      for (int j = j0; j <= j1; j++) for (int i = i0; i <= i1; i++) {
        float d = segD(rox + (i + .5f)*T, roz + (j + .5f)*T, ax, az, bx, bz); if (d > reach) continue;
        unsigned char b = (unsigned char)(fminf(1.f, fmaxf(0.f, .5f - (d - hw)/(2*wr)))*255.f + .5f), *t = rbuf + (j*RWN + i)*4 + (road ? 0 : 1); if (b > *t) *t = b;
      }
    }
    if (!road || n < 8) continue;
    for (float s0 = 8.f; s0 + 5.f < P.s.back() - 8.f; s0 += 10.f) {   // dashed centre line, drawn as box distance fields so it stays crisp
      int a = 0, b = 0; while (a + 1 < n && P.s[a+1] < s0) a++; while (b + 1 < n && P.s[b+1] < s0 + 5.f) b++;
      float ta = (s0 - P.s[a])/fmaxf(P.s[a+1] - P.s[a], 1e-4f), tb = (s0 + 5.f - P.s[b])/fmaxf(P.s[b+1] - P.s[b], 1e-4f);
      float ax = P.x[a] + (P.x[a+1] - P.x[a])*ta, az = P.z[a] + (P.z[a+1] - P.z[a])*ta, bx = P.x[b] + (P.x[b+1] - P.x[b])*tb, bz = P.z[b] + (P.z[b+1] - P.z[b])*tb;
      float mx = (ax + bx)*.5f, mz = (az + bz)*.5f, L = hypotf(bx - ax, bz - az), ux = (bx - ax)/(L + 1e-6f), uz = (bz - az)/(L + 1e-6f), R = L*.5f + 1.5f;
      if (mx + R < rox || mx - R > x1w || mz + R < roz || mz - R > z1w) continue;
      int i0 = max(0, (int)floorf((mx - R - rox)/T)), i1 = min(RWN - 1, (int)ceilf((mx + R - rox)/T)), j0 = max(0, (int)floorf((mz - R - roz)/T)), j1 = min(RWN - 1, (int)ceilf((mz + R - roz)/T));
      for (int j = j0; j <= j1; j++) for (int i = i0; i <= i1; i++) {
        float wx = rox + (i + .5f)*T - mx, wz = roz + (j + .5f)*T - mz, lx = wx*ux + wz*uz, lz = -wx*uz + wz*ux, qx = fabsf(lx) - L*.5f, qz = fabsf(lz) - .17f;
        float sd = hypotf(fmaxf(qx, 0.f), fmaxf(qz, 0.f)) + fminf(fmaxf(qx, qz), 0.f);
        unsigned char b2 = (unsigned char)(fminf(1.f, fmaxf(0.f, .5f - sd*.5f))*255.f + .5f), *t = rbuf + (j*RWN + i)*4 + 3; if (b2 > *t) *t = b2;
      }
    }
  }
  for (size_t q = 0; q < plats.size(); q++) {
    const Plat& p = plats[q]; float R = hypotf(p.hx, p.hz) + 3.f;
    if (p.x + R < rox || p.x - R > x1w || p.z + R < roz || p.z - R > z1w) continue;
    int i0 = max(0, (int)floorf((p.x - R - rox)/T)), i1 = min(RWN - 1, (int)ceilf((p.x + R - rox)/T)), j0 = max(0, (int)floorf((p.z - R - roz)/T)), j1 = min(RWN - 1, (int)ceilf((p.z + R - roz)/T));
    for (int j = j0; j <= j1; j++) for (int i = i0; i <= i1; i++) {
      float sd = rectSdf(p, rox + (i + .5f)*T, roz + (j + .5f)*T), ss = fabsf(sd + .9f) - .3f; unsigned char* t = rbuf + (j*RWN + i)*4;
      unsigned char b = (unsigned char)(fminf(1.f, fmaxf(0.f, .5f - sd/2.4f))*255.f + .5f), m = (unsigned char)(fminf(1.f, fmaxf(0.f, .5f - ss*.5f))*255.f + .5f); if (b > t[2]) t[2] = b; if (sd < 1.8f) t[3] = m;
    }
  }
}
static bool roadCov(float x, float z) {   // CPU side lookup of the same mask (keeps grass off roads)
  int i = (int)floorf((x - rox)/(RWS/RWN)), j = (int)floorf((z - roz)/(RWS/RWN)); if (i < 0 || j < 0 || i >= RWN || j >= RWN) return false;
  const unsigned char* t = rbuf + (j*RWN + i)*4; return t[0] > 6 || t[1] > 40 || t[2] > 6;
}
// ---- minimap (whole island, hill-shaded, with roads, trails and platforms) ----
static void mmLine(unsigned char* img, int M, float x0, float z0, float x1, float z1, float hw, float r, float g, float b, float a) {
  float k = (M - 1)/(2*WE), ax = (x0 + WE)*k, ay = (z0 + WE)*k, bx = (x1 + WE)*k, by = (z1 + WE)*k, R = hw + 1.5f;
  int i0 = max(0, (int)floorf(fminf(ax, bx) - R)), i1 = min(M - 1, (int)ceilf(fmaxf(ax, bx) + R)), j0 = max(0, (int)floorf(fminf(ay, by) - R)), j1 = min(M - 1, (int)ceilf(fmaxf(ay, by) + R));
  for (int j = j0; j <= j1; j++) for (int i = i0; i <= i1; i++) {
    float cv = fminf(1.f, fmaxf(0.f, hw + .5f - segD((float)i, (float)j, ax, ay, bx, by)))*a; if (cv <= 0.f) continue;
    unsigned char* q = img + (j*M + i)*4; q[0] = (unsigned char)(q[0]*(1 - cv) + r*255*cv); q[1] = (unsigned char)(q[1]*(1 - cv) + g*255*cv); q[2] = (unsigned char)(q[2]*(1 - cv) + b*255*cv);
  }
}
static void fillMap(unsigned char* img, int M) {
  const float E = WE;
  for (int j = 0; j < M; j++) for (int i = 0; i < M; i++) {
    float x = (i/(M - 1.f)*2 - 1)*E, z = (j/(M - 1.f)*2 - 1)*E, h = sampG(gH, x, z), bio = biomeAt(x, z), r, g, b;
    if (h < -.2f) { float t = fminf(1.f, (-.2f - h)/4); r = .2f - .15f*t; g = .5f - .3f*t; b = .75f - .2f*t; }
    else if (h < 1.6f) { r = .85f; g = .78f; b = .55f; }
    else if (bio > 1.5f) { r = .93f; g = .96f; b = 1.f; }
    else if (bio > .5f) { r = .85f; g = .65f; b = .38f; }
    else { float t = fminf(1.f, h/18.f); r = .25f + .35f*t; g = .5f - .05f*t; b = .2f + .25f*t; }
    float sh = 1.f + fmaxf(-.4f, fminf(.4f, (sampG(gH, x + 4, z) - h)*.1f)); if (h < -.2f) sh = 1.f;
    unsigned char* q = img + (j*M + i)*4; q[0] = (unsigned char)fminf(255.f, r*sh*255); q[1] = (unsigned char)fminf(255.f, g*sh*255); q[2] = (unsigned char)fminf(255.f, b*sh*255); q[3] = 255;
  }
  for (int pass = 1; pass >= 0; pass--) for (size_t q = 0; q < polys.size(); q++) if (polys[q].type == pass) {   // trails first, roads on top
    const Poly& P = polys[q]; for (int k = 0; k + 1 < (int)P.x.size(); k++) {
      if (pass == 0) mmLine(img, M, P.x[k], P.z[k], P.x[k+1], P.z[k+1], 1.25f, .14f, .14f, .16f, 1.f);
      else mmLine(img, M, P.x[k], P.z[k], P.x[k+1], P.z[k+1], .6f, .55f, .32f, .12f, .95f);
    }
  }
  float k = (M - 1)/(2*E);
  for (size_t q = 0; q < plats.size(); q++) {   // platforms: yellow squares with a dark outline
    int cx = (int)((plats[q].x + E)*k + .5f), cz = (int)((plats[q].z + E)*k + .5f);
    for (int dj = -5; dj <= 5; dj++) for (int di = -5; di <= 5; di++) {
      int ii = cx + di, jj = cz + dj; if (ii < 0 || jj < 0 || ii >= M || jj >= M) continue; unsigned char* t = img + (jj*M + ii)*4; bool edge = abs(di) == 5 || abs(dj) == 5;
      t[0] = edge ? 20 : 255; t[1] = edge ? 20 : 205; t[2] = edge ? 24 : 30;
    }
  }
}
// @@WORLD_END
static float rnd() { rs = rs * 1664525u + 1013904223u; return (rs >> 8) / 16777216.f; }
static void mul(float* o, const float* a, const float* b) {
  float t[16];
  for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) { t[j*4+i] = 0; for (int k = 0; k < 4; k++) t[j*4+i] += a[k*4+i] * b[j*4+k]; }
  for (int i = 0; i < 16; i++) o[i] = t[i];
}
static void box(float x, float y, float z, float w, float h, float d, float r, float g, float b) {
  float x0 = x-w/2, x1 = x+w/2, y0 = y, y1 = y+h, z0 = z-d/2, z1 = z+d/2;
  float P[8][3] = {{x0,y0,z0},{x1,y0,z0},{x1,y1,z0},{x0,y1,z0},{x0,y0,z1},{x1,y0,z1},{x1,y1,z1},{x0,y1,z1}};
  static const int F[6][4] = {{0,1,2,3},{5,4,7,6},{4,0,3,7},{1,5,6,2},{3,2,6,7},{4,5,1,0}};
  static const float S[6] = {.7f,.7f,.8f,.8f,1.f,.5f};
  static const int k[6] = {0,1,2,0,2,3};
  for (int f = 0; f < 6; f++) for (int i = 0; i < 6; i++) { const float* p = P[F[f][k[i]]]; vb.push_back({p[0], p[1], p[2], r*S[f], g*S[f], b*S[f]}); }
}
static void disc(float cx, float cy, float rad, float r, float g, float b, int n = 16, float rot = 0) {
  auto Q = [&](float u, float v) { vb.push_back({u/W*2-1, 1-v/H*2, 0, r, g, b}); };
  for (int i = 0; i < n; i++) {
    float a0 = rot + i*6.2832f/n, a1 = rot + (i+1)*6.2832f/n;
    Q(cx, cy); Q(cx + rad*cosf(a0), cy + rad*sinf(a0)); Q(cx + rad*cosf(a1), cy + rad*sinf(a1));
  }
}
static GLuint streamVbo;   // the dynamic vertex buffer (sky, HUD, ghost)
static void flush(const float* m, const float* fog, float lit) {
  glUniformMatrix4fv(uM, 1, 0, m); glUniform3fv(uFog, 1, fog); glUniform1f(uLit, lit); glUniform1f(uTx, txOn);
  glBindBuffer(GL_ARRAY_BUFFER, streamVbo); glVertexAttribPointer(0, 3, GL_FLOAT, 0, 24, (void*)0); glVertexAttribPointer(1, 3, GL_FLOAT, 0, 24, (void*)12);
  glBufferData(GL_ARRAY_BUFFER, vb.size()*sizeof(V), vb.data(), GL_STREAM_DRAW);
  glDrawArrays(GL_TRIANGLES, 0, (GLsizei)vb.size()); vb.clear();
}
static void say(const char* s) { snprintf(msg, sizeof msg, "%s", s); msgT = 1.5f; }
static void push(float& x, float& z, float x0, float z0, float x1, float z1) {
  float nx = fminf(fmaxf(x, x0), x1), nz = fminf(fmaxf(z, z0), z1), dx = x-nx, dz = z-nz, d = hypotf(dx, dz);
  if (d < .4f && d > 1e-4f) { x = nx + dx/d*.4f; z = nz + dz/d*.4f; }
}
static Blk ghost() {
  float fx = -sinf(yaw), fz = -cosf(yaw);
  Blk b = {roundf((px+fx*5)/4)*4, roundf((pz+fz*5)/4)*4, 4, 4, mode == 2};
  if (b.wall) { if (fabsf(fx) > fabsf(fz)) { b.x += fx > 0 ? 2 : -2; b.w = .4f; } else { b.z += fz > 0 ? 2 : -2; b.d = .4f; } }
  b.y = meshH(b.x, b.z);
  return b;
}
// ===================== building system (GLB pieces, ghost, snapping) =====================
struct Pc { int type; float x, y, z, yaw; int hinge; float open, tgt; };   // 0 foundation 1 wall 2 doorway 3 window 4 half 5 low 6 ceiling 7 door
static vector<Pc> pcs;
static const float SC = .5f;   // model scale
static float CELLW = 3.f, FT = 4.2f, CT = .12f, DOORHW = .46f, DOORH = 1.98f, DPIV = .9189f;   // re-measured from the loaded models
static float WHT[8] = {0, 2.72f, 2.72f, 2.72f, 1.25f, .9f, 0, 0}, FIT[8] = {1, 1, 1, 1, 1, 1, 1, 1}, HW[8] = {1.8f, 1.36f, 1.36f, 1.36f, 1.3f, 1.3f, 1.65f, .5f};
static const int COST[8] = {40, 20, 20, 20, 12, 8, 25, 15};
static const char* MN[8] = {"foundation", "wall", "doorway", "window_wall", "half_wall", "low_wall", "ceiling", "door"};
static const float ID4[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
static GLuint mVbo[8], mIbo[8], mTex[6], dummyTex, rTex; static bool mTexOk[6]; static int mCnt[8]; static bool mReady = false; static GLint uMm = -1, uShMm = -1, uGt = -1, uRm = -1, uRmo = -1;
static float boff = 0, brot = 0; static int bhinge = 1;
struct Gh { bool on, ok; int type; float x, y, z, yaw; int hinge; char why[48]; };
static Gh gh = {};
#define WHY(s) snprintf(gh.why, sizeof gh.why, "%s", s)
static void loc2w(const Pc& p, float lx, float lz, float& wx, float& wz) { float c = cosf(p.yaw), s = sinf(p.yaw); wx = p.x + lx*c + lz*s; wz = p.z - lx*s + lz*c; }
static void w2l(const Pc& p, float x, float z, float& lx, float& lz) { float c = cosf(p.yaw), s = sinf(p.yaw), dx = x - p.x, dz = z - p.z; lx = dx*c - dz*s; lz = dx*s + dz*c; }
static float topOf(const Pc& p) { return p.type == 0 ? p.y + FT : p.type == 6 ? p.y + CT : p.y + WHT[p.type]; }
static void pcMat(float* M, const Pc& p) {
  float s = SC, c = cosf(p.yaw), sn = sinf(p.yaw), sx = s*FIT[p.type], sz = p.type == 6 ? s*FIT[6] : s, mx = (p.type == 7 && p.hinge < 0) ? -1.f : 1.f;
  float B[16] = {c*sx*mx, 0, -sn*sx*mx, 0,  0, s, 0, 0,  sn*sz, 0, c*sz, 0,  p.x, p.y, p.z, 1};
  if (p.type == 7 && p.open > .001f) { float a = -p.open*1.5708f, ca = cosf(a), sa = sinf(a); float L[16] = {ca, 0, -sa, 0,  0, 1, 0, 0,  sa, 0, ca, 0,  DPIV*(1 - ca), 0, DPIV*sa, 1}; mul(M, B, L); }
  else for (int i = 0; i < 16; i++) M[i] = B[i];
}
static void drawModel(int m, const float* M, bool sh) {
  if (!mCnt[m]) return;
  glUniformMatrix4fv(sh ? uShMm : uMm, 1, 0, M);
  glBindBuffer(GL_ARRAY_BUFFER, mVbo[m]); glVertexAttribPointer(0, 3, GL_FLOAT, 0, 24, (void*)0); glVertexAttribPointer(1, 3, GL_FLOAT, 0, 24, (void*)12);
  glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, mIbo[m]); glDrawElements(GL_TRIANGLES, mCnt[m], GL_UNSIGNED_SHORT, 0);
}
static void useStream() { glBindBuffer(GL_ARRAY_BUFFER, streamVbo); glVertexAttribPointer(0, 3, GL_FLOAT, 0, 24, (void*)0); glVertexAttribPointer(1, 3, GL_FLOAT, 0, 24, (void*)12); glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0); }
static void loadModels() {
  glGenTextures(6, mTex);
  for (int k = 0; k < 6; k++) { glActiveTexture(GL_TEXTURE0); int ok = texUp(mTex[k], k); mTexOk[k] = ok != 0; glActiveTexture(GL_TEXTURE2 + k); glBindTexture(GL_TEXTURE_2D, ok ? mTex[k] : dummyTex); }
  glActiveTexture(GL_TEXTURE0); glGenBuffers(8, mVbo); glGenBuffers(8, mIbo);
  for (int m = 0; m < 8; m++) {
    int nv = glbN(MN[m], 0), ni = glbN(MN[m], 1); if (!nv || !ni) continue;
    vector<V> v(nv); vector<unsigned short> ix(ni); glbV(MN[m], (float*)v.data()); glbI(MN[m], ix.data());
    glBindBuffer(GL_ARRAY_BUFFER, mVbo[m]); glBufferData(GL_ARRAY_BUFFER, nv*sizeof(V), v.data(), GL_STATIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, mIbo[m]); glBufferData(GL_ELEMENT_ARRAY_BUFFER, ni*2, ix.data(), GL_STATIC_DRAW); mCnt[m] = ni;
  }
  if (mCnt[0]) { float tw = glbB(MN[0], 6), td = glbB(MN[0], 7); if (tw > .5f) { CELLW = fmaxf(tw, td)*SC; FT = glbB(MN[0], 4)*SC; } HW[0] = CELLW*.5f; }
  for (int m = 1; m <= 6; m++) if (mCnt[m]) {   // walls / ceiling are stretched to the foundation deck width
    float w = glbB(MN[m], 3) - glbB(MN[m], 0), f = CELLW/(w*SC); FIT[m] = fabsf(f - 1.f) > .03f ? fminf(f, 1.6f) : 1.f; HW[m] = w*SC*FIT[m]*.5f;
    if (m == 6) CT = glbB(MN[m], 4)*SC; else WHT[m] = glbB(MN[m], 4)*SC;
  }
  if (mCnt[7]) { FIT[7] = FIT[2]; DOORHW = (glbB(MN[7], 3) - glbB(MN[7], 0))*SC*FIT[7]*.5f; DOORH = glbB(MN[7], 4)*SC; DPIV = glbB(MN[7], 8); }
  useStream(); mReady = true;
}
static void pushOBB(float& x, float& z, float cx, float cz, float yaw, float hx, float hz) {
  float c = cosf(yaw), s = sinf(yaw), dx = x - cx, dz = z - cz, lx = dx*c - dz*s, lz = dx*s + dz*c;
  float qx = fminf(fmaxf(lx, -hx), hx), qz = fminf(fmaxf(lz, -hz), hz), ex = lx - qx, ez = lz - qz, d = hypotf(ex, ez);
  if (d >= .4f) return;
  if (d > 1e-4f) { lx = qx + ex/d*.4f; lz = qz + ez/d*.4f; }
  else { float a = hx - fabsf(lx), b = hz - fabsf(lz); if (a < b) lx = (lx < 0 ? -1.f : 1.f)*(hx + .4f); else lz = (lz < 0 ? -1.f : 1.f)*(hz + .4f); }
  x = cx + lx*c + lz*s; z = cz - lx*s + lz*c;
}
static const float STEP = .6f;   // auto-step / mantle height
// ===================== spatial partitioning: building pieces and world nodes live in hash grids =====================
static bool shDirty = true;   // something that casts a shadow changed -> the cached shadow map must be redrawn
struct PcX { float c, s; alignas(16) float M[16]; };   // per piece cache (SoA): cos/sin of the yaw and the model matrix
static vector<PcX> pcx; static unordered_map<long long, vector<int> > pgr; static vector<int> pdoor;   // 8x8 unit grid, door list (only doors animate)
static void pcAdd(const Pc& p) {
  int i = (int)pcs.size(); pcs.push_back(p); PcX x; x.c = cosf(p.yaw); x.s = sinf(p.yaw); pcMat(x.M, p); pcx.push_back(x);
  pgr[ckey((int)floorf(p.x/8.f), (int)floorf(p.z/8.f))].push_back(i); if (p.type == 7) pdoor.push_back(i);
}
static void pcClear() { pcs.clear(); pcx.clear(); pgr.clear(); pdoor.clear(); }
static void pcNear(float x, float z, float r, vector<int>& out) {   // pieces whose centre lies within r of (x,z)
  out.clear(); if (pcs.empty()) return;
  int i0 = (int)floorf((x - r)/8.f), i1 = (int)floorf((x + r)/8.f), j0 = (int)floorf((z - r)/8.f), j1 = (int)floorf((z + r)/8.f);
  for (int j = j0; j <= j1; j++) for (int i = i0; i <= i1; i++) { auto it = pgr.find(ckey(i, j)); if (it != pgr.end()) out.insert(out.end(), it->second.begin(), it->second.end()); }
}
struct PcView {   // range-for over a list of piece indices (keeps the old loop bodies unchanged)
  const vector<int>* ids;
  struct It { const int* p; const Pc& operator*() const { return pcs[*p]; } It& operator++() { ++p; return *this; } bool operator!=(const It& o) const { return p != o.p; } };
  It begin() const { return It{ids->data()}; } It end() const { return It{ids->data() + ids->size()}; }
};
static inline void w2lc(const Pc& p, float x, float z, float& lx, float& lz) {   // world -> local with the cached cos/sin
  const PcX& k = pcx[(size_t)(&p - pcs.data())]; float dx = x - p.x, dz = z - p.z; lx = dx*k.c - dz*k.s; lz = dx*k.s + dz*k.c;
}
static void doorStep(float dt) {   // doors sleep once they reached their target
  for (int i : pdoor) { Pc& d = pcs[i]; if (fabsf(d.tgt - d.open) > 1e-3f) { d.open += (d.tgt - d.open)*fminf(1.f, dt*8.f); pcMat(pcx[i].M, d); shDirty = true; } }
}
static vector<int> pnl;   // scratch lists (no allocation per frame)
static void collidePcs() {
  pcNear(px, pz, 4.f, pnl); PcView nv = {&pnl}; float hh = CELLW*.5f;
  for (const auto& p : nv) {
    float top = topOf(p);
    if (p.type == 0) { float lx, lz; w2lc(p, px, pz, lx, lz); if (py < top - STEP && fabsf(lx) < hh + .6f && fabsf(lz) < hh + .6f) pushOBB(px, pz, p.x, p.z, p.yaw, hh, hh); }
    else if (p.type >= 1 && p.type <= 5) {
      if (!(py < top - .1f && py + 1.8f > p.y + .1f)) continue;
      if (p.type == 2) { float ph = (HW[2] - DOORHW)*.5f, pc = DOORHW + ph; for (int sg = -1; sg <= 1; sg += 2) { float cx, cz; loc2w(p, sg*pc, 0, cx, cz); pushOBB(px, pz, cx, cz, p.yaw, ph, .15f); } }
      else pushOBB(px, pz, p.x, p.z, p.yaw, HW[p.type], .15f);
    } else if (p.type == 7 && p.open < .5f && py < p.y + DOORH) pushOBB(px, pz, p.x, p.z, p.yaw, DOORHW, .08f);
  }
}
static float surfaceY(float x, float z, float feet, float base) {
  static vector<int> sl; pcNear(x, z, 4.f, sl); PcView nv = {&sl}; float g = base, hh = CELLW*.5f;
  for (const auto& p : nv) if (p.type == 0 || p.type == 6) { float lx, lz; w2lc(p, x, z, lx, lz); float top = topOf(p); if (fabsf(lx) < hh && fabsf(lz) < hh && feet >= top - STEP) g = fmaxf(g, top); }
  return g;
}
// ---- world nodes (trees, rocks, ore, bushes): 64x64 unit chunks in a hash grid, structure of arrays inside a chunk ----
static const int NCS = 64;
// Plants: one entry per model in models/tree_assets, in the order of NAMES in index.html. The node type of a plant is 20 + index.
enum { P_OAK1 = 20, P_OAK2, P_OAK3, P_BIRCHS, P_BIRCHT1, P_BIRCHT2, P_SPRS, P_SPRM, P_SPRT, P_BUSHT, P_BUSHW, P_BUSHH, P_BUSHD, P_SAG1, P_SAG2, P_BARREL, P_PEAR };
struct Sp { const char* nm; float rad; int kind; };   // rad: trunk collision radius at scale 1 (bushes: size of the soft zone) | kind: 0 tree (wood) | 1 bush (soft, nothing to harvest) | 2 cactus (water) | 3 cactus (wood)
static const int NSP = 17;
static const Sp SPC[NSP] = {
  {"oak_01", .75f, 0}, {"oak_02", .88f, 0}, {"oak_03", .6f, 0},
  {"birch_short_01", .34f, 0}, {"birch_tall_01", .36f, 0}, {"birch_tall_02", .36f, 0},
  {"spruce_small_01", .42f, 0}, {"spruce_mid_01", .5f, 0}, {"spruce_tall_01", .58f, 0},
  {"bush_tall_01", 1.f, 1}, {"bush_wide_01", 1.4f, 1}, {"bush_hedge_01", 1.5f, 1}, {"bush_dry_01", 1.f, 1},
  {"cactus_saguaro_01", .55f, 3}, {"cactus_saguaro_02", .6f, 3}, {"cactus_barrel_01", .55f, 2}, {"cactus_pear_01", .5f, 2}};
static void trLoadStep();   // uploads the plant models, defined with the plant runtime further down
static void streamWorld(bool force);   // defined with the streaming code further down
struct Cell { unsigned vbo[2]; int n[2]; float y0, y1; signed char band; bool dirty, built; };   // one 32x32 static batch: [0] opaque, [1] alpha-tested
struct NChunk { int cx, cz, n, dead; bool mod; vector<float> x, z, y, t, sc, rt, sn; vector<int> type, hp;   // sc / rt / sn: scale, yaw and snow amount of a plant
    vector<signed char> cell; Cell c[4]; };
static unordered_map<long long, NChunk*> nchm;
struct DeadRef { NChunk* c; int i; }; static vector<DeadRef> deadList;   // harvested nodes waiting to respawn (event driven, nothing else is touched)
static int nodeHp(int ty) { if (ty >= 20) { int k = SPC[ty - 20].kind; return k == 1 ? 1 : k == 2 ? 3 : 5; } return ty == 1 || ty == 5 || ty == 6 ? 7 : 1; }
static NChunk* ncFind(int cx, int cz) { auto it = nchm.find(ckey(cx, cz)); return it == nchm.end() ? 0 : it->second; }
static void nodeStep(float dt) {   // collision with the nodes around the player (3x3 chunks) + respawn timers
  int cx = (int)floorf(px/NCS), cz = (int)floorf(pz/NCS);
  for (int j = cz - 1; j <= cz + 1; j++) for (int i = cx - 1; i <= cx + 1; i++) {
    NChunk* c = ncFind(i, j); if (!c) continue;
    for (int k = 0; k < c->n; k++) {
      float nx = c->x[k], nz = c->z[k]; if (c->hp[k] <= 0 || fabsf(nx - px) > 3.f || fabsf(nz - pz) > 3.f) continue;   // cheap reject first: nothing farther than 3 can touch the player
      int ty = c->type[k];
      if (ty >= 20) { const Sp& sp = SPC[ty - 20]; float r = sp.rad*c->sc[k];
        if (sp.kind == 1) { float bx = px - nx, bz = pz - nz; if (bx*bx + bz*bz < r*r*.6f) inBush = true; }   // bushes are soft: walk through, the leaves rustle
        else push(px, pz, nx - r, nz - r, nx + r, nz + r); }
      else push(px, pz, nx - 1.3f, nz - 1.3f, nx + 1.3f, nz + 1.3f);
    }
  }
  for (size_t q = 0; q < deadList.size();) {
    NChunk* c = deadList[q].c; int i = deadList[q].i;
    if ((c->t[i] -= dt) <= 0) { c->hp[i] = nodeHp(c->type[i]); c->c[c->cell[i]].dirty = true; c->dead--; deadList[q] = deadList.back(); deadList.pop_back(); shDirty = true; } else q++;
  }
}
static bool aimPoint(float& ax, float& ay, float& az) {   // view ray against terrain, floor slabs and walls (only the pieces near the ray are tested)
  float cp = cosf(pitch), dx = -sinf(yaw)*cp, dy = sinf(pitch), dz = -cosf(yaw)*cp, ex = px, ey = py + 1.7f, ez = pz, hh = CELLW*.5f + .1f;
  static vector<int> al; pcNear(ex, ez, 13.f, al); PcView nv = {&al};
  for (float t = .6f; t <= 10.f; t += .2f) {
    float x = ex + dx*t, y = ey + dy*t, z = ez + dz*t; ax = x; ay = y; az = z;
    if (y <= meshH(x, z)) return true;
    for (const auto& p : nv) {
      float lx, lz; w2lc(p, x, z, lx, lz);
      if (p.type == 0 || p.type == 6) { float top = topOf(p); if (fabsf(lx) < hh && fabsf(lz) < hh && y <= top && y >= top - 1.2f) { ay = top; return true; } }
      else if (p.type >= 1 && p.type <= 5 && fabsf(lx) < HW[p.type] + .1f && fabsf(lz) < .35f && y >= p.y && y <= p.y + WHT[p.type] + .2f) return true;
    }
  }
  return false;
}
struct Ray { float e[3], d[3]; };
static float rayD(const Ray& r, float sx, float sy, float sz) {   // distance from the view ray to a point (large if behind / too far)
  float vx = sx - r.e[0], vy = sy - r.e[1], vz = sz - r.e[2], t = vx*r.d[0] + vy*r.d[1] + vz*r.d[2];
  if (t < .3f || t > 11.f) return 1e9f;
  float cx = r.e[0] + r.d[0]*t - sx, cy = r.e[1] + r.d[1]*t - sy, cz = r.e[2] + r.d[2]*t - sz;
  return sqrtf(cx*cx + cy*cy + cz*cz) + .03f*t;
}
static float rayArea(const Ray& r, float cx, float cz, float yaw, float y) {   // distance from the ray to a square cell (5 sample points)
  static const float o[5][2] = {{0, 0}, {.5f, 0}, {-.5f, 0}, {0, .5f}, {0, -.5f}};
  Pc e = {0, cx, 0, cz, yaw, 1, 0, 0}; float d = 1e9f, hh = CELLW*.5f;
  for (int i = 0; i < 5; i++) { float sx, sz; loc2w(e, o[i][0]*hh, o[i][1]*hh, sx, sz); d = fminf(d, rayD(r, sx, y, sz)); }
  return d;
}
static void updGhost() {
  float ev9 = gf(9), ev10 = gf(10); int sel = mode == 1 ? gk(8) - 1 : mode == 2 ? 7 : -1;
  gh.on = false; if (!mReady) return;
  if (mode == 1 && sel < 0) { say("Нажмите кнопку меню построек"); return; }
  if (sel < 0) return;
  if (sel == 0) { boff = fminf(fmaxf(boff + ev9*.25f, -3.f), 6.f); brot += ev10*.2618f; } else if (sel == 7 && ev10 > .5f) bhinge = -bhinge;
  gh.on = true; gh.ok = false; gh.type = sel; gh.hinge = bhinge; gh.why[0] = 0; gh.yaw = brot;
  static vector<int> nlist; pcNear(px, pz, 22.f, nlist); PcView nearPcs = {&nlist};   // only the pieces around the player take part
  float cp = cosf(pitch); Ray rr = {{px, py + 1.7f, pz}, {-sinf(yaw)*cp, sinf(pitch), -cosf(yaw)*cp}};
  float ax, ay, az; bool hit = aimPoint(ax, ay, az); gh.x = ax; gh.y = ay; gh.z = az;
  float hh = CELLW*.5f; bool okp = false;
  if (sel == 0) {
    const Pc* nb = 0; float bd = 1.9f, bx = 0, bz = 0;
    for (const auto& p : nearPcs) if (p.type == 0 && hypotf(p.x - px, p.z - pz) < 16.f) for (int k = 0; k < 4; k++) {
      float lx = k == 0 ? CELLW : k == 1 ? -CELLW : 0.f, lz = k == 2 ? CELLW : k == 3 ? -CELLW : 0.f, wx, wz; loc2w(p, lx, lz, wx, wz);
      bool occ = false; for (const auto& q : nearPcs) if (q.type == 0 && fabsf(q.x - wx) < CELLW*.25f && fabsf(q.z - wz) < CELLW*.25f) occ = true; if (occ) continue;
      float d = rayArea(rr, wx, wz, p.yaw, p.y + FT); if (d < bd) { bd = d; nb = &p; bx = wx; bz = wz; }
    }
    bool snap = nb != 0;
    if (!snap && !hit) { WHY("Слишком далеко"); say(gh.why); return; }
    float x = snap ? bx : ax, z = snap ? bz : az, yw = snap ? nb->yaw : brot, mx = -1e9f, mn = 1e9f; Pc t = {0, x, 0, z, yw, 1, 0, 0};
    for (int i = -2; i <= 2; i++) for (int j = -2; j <= 2; j++) { float wx, wz; loc2w(t, i*CELLW*.25f, j*CELLW*.25f, wx, wz); float h = meshH(wx, wz); mx = fmaxf(mx, h); mn = fminf(mn, h); }
    float top = snap ? nb->y + FT : mx + .25f + boff, lo = mx + .15f, hi = mn + FT;
    gh.x = x; gh.z = z; gh.yaw = yw; gh.y = top - FT;
    bool ov = false; for (const auto& q : nearPcs) if (q.type == 0 && hypotf(q.x - x, q.z - z) < CELLW*.9f) ov = true;
    if (ov) WHY("Здесь занято"); else if (mn < -.1f) WHY("В воде"); else if (top < lo - .001f) WHY("Фундамент в земле"); else if (top > hi + .001f) WHY("Слишком высоко: ножки не достают"); else okp = true;
  } else if (sel >= 1 && sel <= 5) {
    float bd = 1.8f; bool found = false;
    for (const auto& s : nearPcs) if ((s.type == 0 || s.type == 6) && hypotf(s.x - px, s.z - pz) < 16.f) {
      float top = s.type == 0 ? s.y + FT : s.y + CT, H = WHT[sel];
      for (int k = 0; k < 4; k++) {
        float lx = k == 0 ? hh : k == 1 ? -hh : 0.f, lz = k == 2 ? hh : k == 3 ? -hh : 0.f, wx, wz; loc2w(s, lx, lz, wx, wz);
        float yw = s.yaw - (k < 2 ? 1.5708f : 0.f), d = 1e9f; Pc e = {1, wx, 0, wz, yw, 1, 0, 0};
        for (int i = -1; i <= 1; i++) { float sx, sz; loc2w(e, i*hh*.6f, 0, sx, sz); d = fminf(d, rayD(rr, sx, top + H*.5f, sz)); }
        if (d < bd) { bd = d; found = true; gh.x = wx; gh.z = wz; gh.y = top; gh.yaw = yw; }
      }
    }
    if (!found) WHY("Наведите на край фундамента");
    else { bool occ = false; for (const auto& q : nearPcs) if (q.type >= 1 && q.type <= 5 && fabsf(q.x - gh.x) < .4f && fabsf(q.z - gh.z) < .4f && fabsf(q.y - gh.y) < .5f) occ = true; if (occ) WHY("Здесь уже стена"); else okp = true; }
  } else if (sel == 6) {
    float bd = 2.1f; bool found = false;
    for (const auto& w : nearPcs) {
      if (hypotf(w.x - px, w.z - pz) > 16.f) continue;
      if (w.type >= 1 && w.type <= 3) {
        float s_ = sinf(w.yaw), c_ = cosf(w.yaw), y0 = w.y + WHT[w.type]; int ps = ((px - w.x)*s_ + (pz - w.z)*c_ > 0) ? 1 : -1;
        for (int sg = -1; sg <= 1; sg += 2) { float x = w.x + sg*s_*hh, z = w.z + sg*c_*hh, d = rayArea(rr, x, z, w.yaw, y0) + (sg == ps ? 0.f : .7f); if (d < bd) { bd = d; found = true; gh.x = x; gh.z = z; gh.yaw = w.yaw; gh.y = y0; } }
      } else if (w.type == 6) {
        for (int k = 0; k < 4; k++) { float lx = k == 0 ? CELLW : k == 1 ? -CELLW : 0.f, lz = k == 2 ? CELLW : k == 3 ? -CELLW : 0.f, wx, wz; loc2w(w, lx, lz, wx, wz); float d = rayArea(rr, wx, wz, w.yaw, w.y + CT); if (d < bd) { bd = d; found = true; gh.x = wx; gh.z = wz; gh.yaw = w.yaw; gh.y = w.y; } }
      }
    }
    if (!found) WHY("Наведите на верх стены или потолка");
    else { bool occ = false; for (const auto& q : nearPcs) if (q.type == 6 && fabsf(q.x - gh.x) < CELLW*.3f && fabsf(q.z - gh.z) < CELLW*.3f && fabsf(q.y - gh.y) < .5f) occ = true; if (occ) WHY("Здесь уже потолок"); else okp = true; }
  } else {
    float bd = 1.8f; bool found = false;
    for (const auto& d : nearPcs) if (d.type == 2 && hypotf(d.x - px, d.z - pz) < 16.f) {
      bool has = false; for (const auto& q : nearPcs) if (q.type == 7 && fabsf(q.x - d.x) < .3f && fabsf(q.z - d.z) < .3f && fabsf(q.y - d.y) < .5f) has = true; if (has) continue;
      float dd = fminf(rayD(rr, d.x, d.y + DOORH*.5f, d.z), rayD(rr, d.x, d.y + DOORH*.9f, d.z)); if (dd < bd) { bd = dd; found = true; gh.x = d.x; gh.y = d.y; gh.z = d.z; gh.yaw = d.yaw; }
    }
    if (!found) WHY("Наведите на дверной проём"); else okp = true;
  }
  if (okp && wood < COST[sel]) { WHY("Не хватает дерева"); okp = false; }
  gh.ok = okp;
  if (!okp && gh.why[0]) say(gh.why);
}
static void place() {
  if (!gh.on) { say("Выберите постройку в меню"); return; }
  if (!gh.ok) { if (gh.why[0]) say(gh.why); return; }
  pcAdd({gh.type, gh.x, gh.y, gh.z, gh.yaw, gh.hinge, 0.f, 0.f}); wood -= COST[gh.type]; sfx(1); shDirty = true;
}
static void act() {
  cd = .4f; float fx = -sinf(yaw), fz = -cosf(yaw);
  if (mode) { place(); return; }
  for (int i : pdoor) { Pc& d = pcs[i]; float dx = d.x - px, dz = d.z - pz, dd = hypotf(dx, dz); if (dd < 3.2f && dx*fx + dz*fz > .4f*dd) { d.tgt = d.tgt > .5f ? 0.f : 1.f; return; } }
  NChunk* bc = 0; int bi = -1; float bd = 4.5f, cxn = floorf(px/NCS), czn = floorf(pz/NCS);
  for (int j = (int)czn - 1; j <= (int)czn + 1; j++) for (int i = (int)cxn - 1; i <= (int)cxn + 1; i++) {
    NChunk* c = ncFind(i, j); if (!c) continue;
    for (int k = 0; k < c->n; k++) {
      int ty = c->type[k]; if (c->hp[k] <= 0 || (ty >= 20 && SPC[ty - 20].kind == 1)) continue;
      float dx = c->x[k] - px, dz = c->z[k] - pz, d = sqrtf(dx*dx + dz*dz);
      if (d < bd && dx*fx + dz*fz > .6f*d) { bd = d; bc = c; bi = k; }
    }
  }
  if (!bc) { if (hgt(px + fx*3, pz + fz*3) < -.2f) { thirst = fminf(100.f, thirst + 18); say("Пьёшь воду"); } else say("Мимо"); return; }
  int ty = bc->type[bi]; bc->mod = true;
  if (--bc->hp[bi] <= 0) { bc->t[bi] = 60; bc->dead++; deadList.push_back(DeadRef{bc, bi}); bc->c[bc->cell[bi]].dirty = true; shDirty = true; }
  int kd = ty >= 20 ? SPC[ty - 20].kind : -1;
  if (kd == 0 || kd == 3) { wood += 10; say("+10 дерева"); }
  else if (kd == 2) { thirst = fminf(100.f, thirst + 22); say("+22 воды"); }
  else if (ty == 1) { stone += 8; say("+8 камня"); }
  else if (ty == 5) { metal += 6; say("+6 металла"); }
  else if (ty == 6) { sulfur += 6; say("+6 серы"); }
}
static EM_BOOL tc(int type, const EmscriptenTouchEvent* e, void*) {
  for (int i = 0; i < e->numTouches; i++) {
    const auto& t = e->touches[i]; if (!t.isChanged) continue;
    float x = t.targetX*K, y = t.targetY*K; int id = t.identifier;
    if (type == EMSCRIPTEN_EVENT_TOUCHSTART) { if (x < W*.5f && st.id < 0) st = {id, x, y, x, y}; else if (lk.id < 0) lk = {id, x, y, x, y}; }
    else if (type == EMSCRIPTEN_EVENT_TOUCHMOVE) {
      if (id == st.id) { st.x = x; st.y = y; }
      else if (id == lk.id) { yaw -= (x-lk.x)/K*.006f; pitch = fminf(fmaxf(pitch - (y-lk.y)/K*.006f, -1.4f), 1.4f); lk.x = x; lk.y = y; }
    } else { if (id == st.id) st.id = -1; if (id == lk.id) lk.id = -1; }
  }
  return EM_TRUE;
}
static void update(float dt) {
  PROF_SCOPE(1);
  { float t[8]; if (tpGet(t)) {   // [x, z, yaw, pitch] = go there | [bioMin, bioMax, hMin, yaw, 1, pitch] = go to the first spot with a good stand of plants in that biome range
      if (t[4] > .5f) { bool hit = false; for (float r = 40.f; r < WE*.9f && !hit; r += 14.f) for (float a = 0.f; a < 6.28f && !hit; a += .09f) { float x = r*cosf(a), z = r*sinf(a), h = hgt(x, z), b = biomeAt(x, z);
          if (b >= t[0] && b <= t[1] && h >= t[2] && h < (t[6] > 0.f ? t[6] : 25.f) && fabsf(hgt(x + 3.f, z) - h) < .6f && fabsf(hgt(x, z + 3.f) - h) < .6f && fabsf(hgt(x - 3.f, z) - h) < .6f && fabsf(hgt(x, z - 3.f) - h) < .6f && !blockedAt(x, z) && (t[6] > 0.f || (pn(x*.018f + 31.f, z*.018f - 17.f) > .6f && pn(x*.055f - 8.f, z*.055f + 12.f) < .6f))) { t[0] = x; t[1] = z; t[2] = t[3]; t[3] = t[5]; hit = true; } }
        if (!hit) { t[0] = px; t[1] = pz; t[2] = yaw; t[3] = pitch; } }
      px = t[0]; pz = t[1]; yaw = t[2]; pitch = t[3]; streamWorld(true); py = meshH(px, pz); vy = 0; tpInfo(px, pz, py, biomeAt(px, pz));
      { static int hist[48]; memset(hist, 0, sizeof hist); int cx = (int)floorf(px/NCS), cz = (int)floorf(pz/NCS);
        for (int j = cz - 1; j <= cz + 1; j++) for (int i = cx - 1; i <= cx + 1; i++) { NChunk* c = ncFind(i, j); if (!c) continue; for (int k = 0; k < c->n; k++) { float dx = c->x[k] - px, dz = c->z[k] - pz; if (dx*dx + dz*dz < 6400.f && c->type[k] < 48) hist[c->type[k]]++; } }
        tpHist(hist, 48); } } }
  static bool mld = false; if (!mld && gk(11)) { loadModels(); mld = true; } doorStep(dt); trLoadStep();
  inBush = false; tod += dt/240; if (tod > 1) tod -= 1; cd -= dt; msgT -= dt;
  food -= dt*.4f; thirst = fmaxf(0.f, thirst - dt*.55f);
  if (food <= 0 || thirst <= 0) { food = fmaxf(food, 0.f); hp -= dt*2; } else if (food > 50 && thirst > 50 && hp < 100) hp = fminf(100, hp + dt*2);
  if (hp <= 0) { hp = 100; food = 100; thirst = 100; wood = stone = metal = sulfur = 0; px = spX; pz = spZ; say("Ты умер. Ресурсы потеряны"); }
  float sx = gf(3), sy = gf(4);
  yaw -= gf(5)*.006f; pitch = fminf(fmaxf(pitch - gf(6)*.006f, -1.4f), 1.4f);
  float fx = -sinf(yaw), fz = -cosf(yaw), rx = cosf(yaw), rz = -sinf(yaw), sp = 5.f*(hypotf(sx, sy) > .95f ? 1.4f : 1.f);
  float ox = px, oz = pz; px += (fx*-sy + rx*sx)*sp*dt; pz += (fz*-sy + rz*sx)*sp*dt;
  nodeStep(dt);
  for (auto& b : blks) if (b.wall) push(px, pz, b.x-b.w/2, b.z-b.d/2, b.x+b.w/2, b.z+b.d/2);
  collidePcs();
  if (hgt(px, pz) < -.5f) { px = ox; pz = oz; }
  float gy = surfaceY(px, pz, py, meshH(px, pz));
  bool jmp = false; if (gk(1) && py <= gy + .01f) { vy = 7.8f; jmp = true; }
  vy -= 20*dt; py += vy*dt; if (py < gy + .25f && vy <= 0) { py = gy; vy = 0; }
  { float mag = hypotf(sx, sy), b2 = biomeAt(px, pz); bool grd = py <= gy + .05f;   // audio is event driven: the JS side is only called when something changed
    int a0 = b2 > 1.5f ? 2 : (b2 > .5f || gy < 1.6f) ? 1 : 0, a1 = (mag > .12f && grd) ? (mag > .95f ? 2 : 1) : 0, a2 = inBush ? 1 : 0, a4 = jmp ? 1 : 0;
    static int l0 = -1, l1 = -1, l2 = -1, l4 = -1; static float lt = -9.f;
    if (a0 != l0 || a1 != l1 || a2 != l2 || a4 != l4 || fabsf(tod - lt) > .002f) { l0 = a0; l1 = a1; l2 = a2; l4 = a4; lt = tod; aud(a0, a1, a2, tod, a4); } }
  mode = gk(2); updGhost();
  if (gk(0) && cd <= 0) act();
}
static float rockD(float dx, float dy, float dz, int sd, float rough) {
  float s = (float)(sd & 1023);
  float a = (pn(dx*1.6f + s, dy*1.6f) + pn(dy*1.6f + 7, dz*1.6f + s) + pn(dz*1.6f + 3, dx*1.6f + s*.5f))/3.f - .5f, b = pn(dx*4.2f + s*.3f, dz*4.2f + dy*3.f) - .5f, st = pn(dy*4.f + s, 3.f) - .5f;
  return 1.f + rough*(a*1.2f + b*.4f + st*.3f);
}
struct RB { float x, y, z, rx, ry, rz, rough; int seed, mat; };
static void rtri(const RB& o, const float* a, const float* b, const float* c, int lev) {
  if (lev > 0) {
    float ab[3] = {a[0]+b[0], a[1]+b[1], a[2]+b[2]}, bc[3] = {b[0]+c[0], b[1]+c[1], b[2]+c[2]}, ca[3] = {c[0]+a[0], c[1]+a[1], c[2]+a[2]};
    float* m3[3] = {ab, bc, ca};
    for (int k = 0; k < 3; k++) { float l = sqrtf(m3[k][0]*m3[k][0] + m3[k][1]*m3[k][1] + m3[k][2]*m3[k][2]); for (int i = 0; i < 3; i++) m3[k][i] /= l; }
    rtri(o, a, ab, ca, lev-1); rtri(o, ab, b, bc, lev-1); rtri(o, ca, bc, c, lev-1); rtri(o, ab, bc, ca, lev-1); return;
  }
  const float* v[3] = {a, b, c};
  for (int i = 0; i < 3; i++) {
    float d = rockD(v[i][0], v[i][1], v[i][2], o.seed, o.rough);
    tv(o.x + v[i][0]*o.rx*d, fmaxf(o.y + v[i][1]*o.ry*d, o.y - .48f*o.ry), o.z + v[i][2]*o.rz*d, 10.f*(o.mat + 1) + .9f, 0, 0);
  }
}
static void blob(float x, float y, float z, float rx, float ry, float rz, int seed, int mat, float rough, int lev) {   // one fused displaced icosphere
  static const float T = 1.618f, IV[12][3] = {{-1,T,0},{1,T,0},{-1,-T,0},{1,-T,0},{0,-1,T},{0,1,T},{0,-1,-T},{0,1,-T},{T,0,-1},{T,0,1},{-T,0,-1},{-T,0,1}};
  static const int IF[20][3] = {{0,11,5},{0,5,1},{0,1,7},{0,7,10},{0,10,11},{1,5,9},{5,11,4},{11,10,2},{10,7,6},{7,1,8},{3,9,4},{3,4,2},{3,2,6},{3,6,8},{3,8,9},{4,9,5},{2,4,11},{6,2,10},{8,6,7},{9,8,1}};
  float U[12][3];
  for (int i = 0; i < 12; i++) { float l = sqrtf(IV[i][0]*IV[i][0] + IV[i][1]*IV[i][1] + IV[i][2]*IV[i][2]); for (int k = 0; k < 3; k++) U[i][k] = IV[i][k]/l; }
  RB o = {x, y + .4f*ry, z, rx, ry, rz, rough, seed, mat};
  for (int f = 0; f < 20; f++) rtri(o, U[IF[f][0]], U[IF[f][1]], U[IF[f][2]], lev);
}
static void ore(float x, float y, float z, int t, int seed, int lod) {   // 0 stone, 1 metal, 2 sulfur: one fused boulder (+8 = snow-capped)
  if (biomeAt(x, z) > 1.5f) t += 8;
  blob(x, y, z, 1.9f, 1.5f, 1.7f, seed, t, .42f, lod == 0 ? 2 : 1);
}
// ===================== render caches: static batches, frustum culling, streaming =====================
struct Frus { float p[6][4]; };
static Frus fr;
static void frustumFrom(Frus& f, const float* A) {   // planes of a column-major view-projection matrix (works for the perspective and for the sun's orthographic one)
  for (int k = 0; k < 6; k++) {
    int r = k >> 1; float s = (k & 1) ? -1.f : 1.f, a[4]; for (int c = 0; c < 4; c++) a[c] = A[c*4 + 3] + s*A[c*4 + r];
    float l = sqrtf(a[0]*a[0] + a[1]*a[1] + a[2]*a[2]) + 1e-9f; for (int c = 0; c < 4; c++) f.p[k][c] = a[c]/l;
  }
}
static inline bool inFrus(const Frus& f, float x0, float y0, float z0, float x1, float y1, float z1) {
  for (int k = 0; k < 6; k++) { const float* p = f.p[k]; if (p[0]*(p[0] > 0 ? x1 : x0) + p[1]*(p[1] > 0 ? y1 : y0) + p[2]*(p[2] > 0 ? z1 : z0) + p[3] < 0) return false; }
  return true;
}
static vector<GLuint> bufPool;   // pooled vertex buffers: nothing is created or destroyed while moving through the world
static GLuint bufGet() { if (!bufPool.empty()) { GLuint b = bufPool.back(); bufPool.pop_back(); return b; } GLuint b; glGenBuffers(1, &b); return b; }
static void bufPut(GLuint b) { if (b) bufPool.push_back(b); }
static void bufUp(GLuint b, const V* d, int n) { glBindBuffer(GL_ARRAY_BUFFER, b); glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)n*sizeof(V), d, GL_STATIC_DRAW); }
static inline void attribs() { glVertexAttribPointer(0, 3, GL_FLOAT, 0, 24, (void*)0); glVertexAttribPointer(1, 3, GL_FLOAT, 0, 24, (void*)12); }
static inline void bufDraw(GLuint b, int n) { glBindBuffer(GL_ARRAY_BUFFER, b); attribs(); glDrawArrays(GL_TRIANGLES, 0, n); }

// ---- terrain blocks (static indexed meshes) ----
static GLuint tIbo = 0;
static void tcIndices() { vector<unsigned short> I; for (int j = 0; j < TCN; j++) for (int i = 0; i < TCN; i++) { unsigned short a = j*TCV + i, b = a + 1, c = a + TCV + 1, d = a + TCV; unsigned short t[6] = {a, b, c, a, c, d}; I.insert(I.end(), t, t + 6); }
  glGenBuffers(1, &tIbo); glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, tIbo); glBufferData(GL_ELEMENT_ARRAY_BUFFER, I.size()*2, I.data(), GL_STATIC_DRAW); glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0); }
static void tcDraw(TChunk* c) {
  if (!c->up) { if (!c->vbo) c->vbo = bufGet(); bufUp(c->vbo, c->v, TCV*TCV); c->up = true; }
  glBindBuffer(GL_ARRAY_BUFFER, c->vbo); attribs(); glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, tIbo); glDrawElements(GL_TRIANGLES, TCN*TCN*6, GL_UNSIGNED_SHORT, (void*)0); glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
}
static void tcWater(TChunk* c) { if (!c->wn) return; if (!c->wup) { if (!c->wvbo) c->wvbo = bufGet(); bufUp(c->wvbo, c->wv.data(), c->wn); c->wup = true; } bufDraw(c->wvbo, c->wn); }
static vector<TChunk*> tvis;
static void collectTerrain() {   // blocks that cover the old 105 unit window and touch the view frustum
  const float R = 110.f; tvis.clear();
  int i0 = (int)floorf((px - R)/80.f), i1 = (int)floorf((px + R)/80.f), j0 = (int)floorf((pz - R)/80.f), j1 = (int)floorf((pz + R)/80.f);
  for (int j = j0; j <= j1; j++) for (int i = i0; i <= i1; i++) { TChunk* c = tcGet(i, j); if (inFrus(fr, i*80.f, c->y0 - .5f, j*80.f, i*80.f + 80.f, c->y1 + .5f, j*80.f + 80.f)) tvis.push_back(c); }
}
// ---- world nodes: generation (deterministic per chunk) ----
// Which plant (or rock) grows at a spot. Every model only grows where the terrain shader paints its own ground: grass (bio < .35), sand (.65 .. 1.2),
// snow (>= 1.6); in the colour ramps between them only dry bushes and rocks. Nothing grows on the beach (low ground), and the cacti need a good margin of
// height above the sand strip. -1 = nothing here.
static int plantPick(float bio, float h, float stand, float q) {
  if (bio < .35f) {   // grass: oak, birch, bushes; conifers only up in the hills
    if (h < 2.8f) return -1;
    if (h > 18.f) return q < .34f ? P_SPRS : q < .58f ? P_SPRM : q < .70f ? P_BIRCHS : q < .82f ? P_OAK3 : P_BUSHH;
    if (stand > .72f) return q < .2f ? P_OAK1 : q < .31f ? P_OAK2 : q < .43f ? P_OAK3 : q < .53f ? P_BIRCHS : q < .64f ? P_BIRCHT1 : q < .74f ? P_BIRCHT2 : q < .82f ? P_BUSHT : q < .91f ? P_BUSHW : P_BUSHH;
    return q < .14f ? P_OAK1 : q < .22f ? P_OAK2 : q < .33f ? P_OAK3 : q < .43f ? P_BIRCHS : q < .52f ? P_BIRCHT1 : q < .6f ? P_BIRCHT2 : q < .72f ? P_BUSHT : q < .86f ? P_BUSHW : P_BUSHH;
  }
  if (bio < 1.6f) {   // sand: cacti, dry bushes, rocks
    if (h < 3.4f) return -1;
    if (bio < .65f) return q < .4f ? P_BUSHD : -1;   // edge between the grass and the sand
    if (bio < 1.2f) return q < .2f ? P_SAG1 : q < .4f ? P_SAG2 : q < .62f ? P_BARREL : q < .8f ? P_PEAR : q < .92f ? P_BUSHD : 1;
    return q < .3f ? P_BUSHD : q < .4f ? 1 : -1;     // towards the snow: no cacti any more, no trees yet
  }
  if (h < 2.8f) return -1;   // snow: spruce, a few birches, dry bushes, rocks
  return q < .2f ? P_SPRS : q < .46f ? P_SPRM : q < .68f ? P_SPRT : q < .76f ? P_BIRCHS : q < .84f ? P_BIRCHT1 : q < .93f ? P_BUSHD : 1;
}
static void ncGen(int cx, int cz) {
  NChunk* c = new NChunk(); c->cx = cx; c->cz = cz; c->dead = 0; c->mod = false;
  for (int k = 0; k < 4; k++) { Cell& e = c->c[k]; e.vbo[0] = e.vbo[1] = 0; e.n[0] = e.n[1] = 0; e.y0 = 0; e.y1 = 1; e.band = -1; e.dirty = true; e.built = false; }
  c->x.reserve(48); c->z.reserve(48); c->y.reserve(48); c->t.reserve(48); c->sc.reserve(48); c->rt.reserve(48); c->sn.reserve(48); c->type.reserve(48); c->hp.reserve(48); c->cell.reserve(48);
  // Deterministic jittered lattice: low-frequency noise makes forests, groves and clearings.
  for (int i = 0; i < 64; i++) {
    int gx = i % 8, gz = i / 8;
    float x = cx*64.f + (gx + .12f + .76f*vn(cx*29 + gx*7, cz*37 + gz))*8.f;
    float z = cz*64.f + (gz + .12f + .76f*vn(cx*41 + gx*11, cz*23 + gz*5))*8.f;
    float sl, h = meshH(x, z, &sl), r = vn(cx*13 + i*3, cz*7 + i), bio = biomeAt(x, z);
    float stand = pn(x*.018f + 31.f, z*.018f - 17.f), clearing = pn(x*.055f - 8.f, z*.055f + 12.f);
    if (hypotf(x - spX, z - spZ) < 6.f || h < .8f || h > 62.f || sl > .42f || blockedAt(x, z)) continue;
    float chance = .18f + .68f*stand; if (clearing > .72f) chance *= .16f;
    if (r > chance) continue;
    int ty = plantPick(bio, h, stand, vn(cx*53 + i*17, cz*31 + i*5)); if (ty < 0) continue;
    bool pl = ty >= 20; float q2 = vn(cx*7 + i*19, cz*61 + i*3), sc = pl ? .78f + .4f*q2 : 1.f, rot = vn(cx*3 + i*23, cz*5 + i*11)*6.2832f, snow = pl ? sst(1.35f, 1.7f, bio) : 0.f;
    float rad = pl ? fminf(1.2f, fmaxf(.5f, SPC[ty - 20].rad*sc)) : 1.7f;
    float yy = fminf(h, fminf(fminf(meshH(x + rad, z), meshH(x - rad, z)), fminf(meshH(x, z + rad), meshH(x, z - rad))));   // sink into slopes, never float
    c->x.push_back(x); c->z.push_back(z); c->y.push_back(yy); c->t.push_back(0.f); c->sc.push_back(sc); c->rt.push_back(rot); c->sn.push_back(snow); c->type.push_back(ty); c->hp.push_back(nodeHp(ty));
    c->cell.push_back((signed char)((x - cx*64.f >= 32.f ? 1 : 0) + (z - cz*64.f >= 32.f ? 2 : 0)));
  }
  c->n = (int)c->x.size(); nchm[ckey(cx, cz)] = c;
}
// ---- 32x32 static batches of nodes (and beach pebbles): baked once per detail level, redrawn only when something changes ----
static int bandFor(float d2, int cur) {   // 0 full detail | 1 medium | 2 low | 3 low without bushes; hysteresis keeps cells from flip-flopping
  static const float T[3] = {1500.f, 5000.f, 8100.f}; int b = 0; while (b < 3 && d2 >= T[b]) b++;
  if (cur >= 0 && b != cur) { float lo = cur > 0 ? T[cur - 1] : -1e9f, hi = cur < 3 ? T[cur] : 1e30f; if (d2 > lo*.88f && d2 < hi*1.12f) return cur; }
  return b;
}
static float cellD2(int cx, int cz, int k) {
  float x0 = cx*64.f + (k & 1)*32.f, z0 = cz*64.f + (k >> 1)*32.f, dx = fmaxf(fmaxf(x0 - px, 0.f), px - (x0 + 32.f)), dz = fmaxf(fmaxf(z0 - pz, 0.f), pz - (z0 + 32.f)); return dx*dx + dz*dz;
}
static void cellFree(Cell& e) { bufPut(e.vbo[0]); bufPut(e.vbo[1]); e.vbo[0] = e.vbo[1] = 0; e.n[0] = e.n[1] = 0; e.built = false; e.band = -1; e.dirty = true; }
static void cellBuild(NChunk* c, int k, int band) {
  Cell& e = c->c[k]; vb.clear(); int lod = band > 2 ? 2 : band;
  for (int i = 0; i < c->n; i++) {
    if (c->cell[i] != k || c->hp[i] <= 0) continue;
    int ty = c->type[i]; if (ty != 1 && ty != 5 && ty != 6) continue;   // plants are not baked: they are drawn instanced from their models
    float nx = c->x[i], ny = c->y[i], nz = c->z[i]; int sd0 = (int)(nx*7) ^ (int)(nz*13);
    ore(nx, ny, nz, ty == 1 ? 0 : ty - 4, sd0, lod);
  }
  if (band == 0) {   // stones on the beach
    int tx0 = (int)floorf((c->cx*64.f + (k & 1)*32.f)/2), tz0 = (int)floorf((c->cz*64.f + (k >> 1)*32.f)/2);
    for (int i = 0; i < 16; i++) for (int j = 0; j < 16; j++) {
      int gx = tx0 + i, gz = tz0 + j; if (vn(gx*5+3, gz*7+1) < .86f) continue;
      float x = (gx + vn(gx, gz+2))*2, z = (gz + vn(gz+5, gx))*2, h = meshH(x, z);
      if (h < -.3f || h > 1.4f) continue;
      float s = .12f + .3f*vn(gx+9, gz); blob(x, h - .03f, z, s, s*.55f, s*.8f, gx^gz, 3, .3f, 0);
    }
  }
  float y0 = 1e9f, y1 = -1e9f;
  for (size_t i = 0; i < vb.size(); i++) { y0 = fminf(y0, vb[i].y); y1 = fmaxf(y1, vb[i].y); }
  e.n[0] = (int)vb.size(); e.n[1] = 0; if (e.n[0]) { if (!e.vbo[0]) e.vbo[0] = bufGet(); bufUp(e.vbo[0], vb.data(), e.n[0]); }
  e.y0 = vb.empty() ? 0.f : y0; e.y1 = vb.empty() ? 1.f : y1; e.band = (signed char)band; e.dirty = false; e.built = true; vb.clear();
  if (cellD2(c->cx, c->cz, k) < 4900.f) shDirty = true;   // changed geometry near the player: refresh the shadow map
}
// ---- occlusion culling (horizon test): a distant cell that is completely hidden behind a ridge of the terrain is not drawn ----
static bool terrainHmin(float x, float z, float& h) {   // lowest corner height of the cached lattice cell under (x,z); false if that block is not loaded (nothing is ever built here)
  int gx = (int)floorf(x/5.f), gz = (int)floorf(z/5.f); auto it = tcm.find(ckey(gx >> 4, gz >> 4)); if (it == tcm.end()) return false;
  const float* H = it->second->h + (gz & 15)*TCV + (gx & 15); h = fminf(fminf(H[0], H[1]), fminf(H[TCV], H[TCV + 1])); return true;
}
static bool cellOccluded(float x0, float z0, float ytop) {   // conservative: all 9 view rays to the top of the cell must be blocked by terrain with a 1 unit margin
  float ex = px, ey = py + 1.7f, ez = pz;
  for (int k = 0; k < 9; k++) {
    float tx = x0 + (k % 3)*16.f, tz = z0 + (k/3)*16.f, ty = ytop + .5f; bool blocked = false;
    for (int s = 1; s <= 7 && !blocked; s++) { float t = s/8.f, x = ex + (tx - ex)*t, z = ez + (tz - ez)*t, h; if (terrainHmin(x, z, h) && h > ey + (ty - ey)*t + 1.f) blocked = true; }
    if (!blocked) return false;
  }
  return true;
}
static void drawCells(const Frus& f, int pass, bool shadow) {   // pass 0 opaque, 1 alpha-tested
  int cx = (int)floorf(px/NCS), cz = (int)floorf(pz/NCS);
  for (int j = cz - 3; j <= cz + 3; j++) for (int i = cx - 3; i <= cx + 3; i++) {
    NChunk* c = ncFind(i, j); if (!c) continue;
    for (int k = 0; k < 4; k++) { Cell& e = c->c[k]; if (!e.built || !e.n[pass]) continue;
      float x0 = i*64.f + (k & 1)*32.f, z0 = j*64.f + (k >> 1)*32.f; if (!shadow && cellD2(i, j, k) > 16900.f) continue;
      if (!inFrus(f, x0, e.y0 - .2f, z0, x0 + 32.f, e.y1 + .2f, z0 + 32.f)) continue;
      if (!shadow && cellD2(i, j, k) > 3600.f && cellOccluded(x0, z0, e.y1)) {
#ifdef PROF
        pC[3]++;
#endif
        continue; }
      bufDraw(e.vbo[pass], e.n[pass]); }
  }
}
// ---- platform props (static) ----
struct PlatMesh { unsigned vbo; int n; }; static vector<PlatMesh> pmesh;
static void platBuild(size_t i) {   // placeholder monument: a pole with a flag and four corner bollards
  const Plat& p = plats[i]; float c = cosf(p.yaw), s = sinf(p.yaw); vb.clear();
  box(p.x, p.h, p.z, .5f, 7.f, .5f, .36f, .37f, .4f); box(p.x + .8f, p.h + 5.6f, p.z, 1.6f, .95f, .08f, .86f, .22f, .16f);
  for (int k = 0; k < 4; k++) { float lx = (k & 1 ? 1.f : -1.f)*(p.hx - 1.2f), lz = (k & 2 ? 1.f : -1.f)*(p.hz - 1.2f); box(p.x + lx*c + lz*s, p.h, p.z - lx*s + lz*c, .6f, 1.1f, .6f, .9f, .74f, .12f); }
  pmesh[i].vbo = bufGet(); pmesh[i].n = (int)vb.size(); bufUp(pmesh[i].vbo, vb.data(), pmesh[i].n); vb.clear();
}
static void drawPlats(const Frus& f, bool shadow) {
  for (size_t i = 0; i < plats.size(); i++) { const Plat& p = plats[i]; float dx = p.x - px, dz = p.z - pz; if (!shadow && dx*dx + dz*dz > 25600.f) continue;
    if (!inFrus(f, p.x - 16.f, p.h - 1.f, p.z - 16.f, p.x + 16.f, p.h + 8.f, p.z + 16.f)) continue;
    if (!pmesh[i].n) platBuild(i); bufDraw(pmesh[i].vbo, pmesh[i].n); }
}
// ---- grass: 16x16 tile static batches, the wind is animated in the vertex shader (tips carry +2 in the red channel) ----
static const int GCN = 16;
struct GChunk { int cx, cz; unsigned vbo; int n; signed char lod; float y0, y1; };
static unordered_map<long long, GChunk*> gcm;
static void grassBuild(GChunk* g, int lod) {   // static LOD batches: layered blades close-up, economical crossed tufts farther away
  vb.clear(); const int nb = lod == 0 ? 3 : 2;
  for (int gx = g->cx*GCN; gx < g->cx*GCN + GCN; gx++) for (int gz = g->cz*GCN; gz < g->cz*GCN + GCN; gz++) {
    float density = vn(gx*3+1, gz*5+2); if (density < (lod == 0 ? .12f : .24f)) continue;
    float x = gx + .12f + .76f*vn(gx, gz), z = gz + .12f + .76f*vn(gz+7, gx);
    float sl, h = meshH(x, z, &sl); if (h < 1.5f || sl > .48f || biomeAt(x, z) > .3f || roadCov(x, z)) continue;
    float patch = vn((int)floorf(x*.22f), (int)floorf(z*.22f)), kind = vn(gx+5, gz+1);
    float baseH = .24f + .31f*vn(gx+3, gz+9), baseR = .10f + .10f*vn(gx+13, gz+2);
    float br=.12f, bg=.29f, bb=.065f, tr=.39f, tg=.58f, tb=.16f;
    if (kind < .28f) { br=.16f; bg=.31f; bb=.055f; tr=.55f; tg=.68f; tb=.19f; }
    else if (kind > .78f) { br=.19f; bg=.25f; bb=.07f; tr=.43f; tg=.49f; tb=.13f; }
    // Each blade is a tapered, slightly bent leaf; two crossed orientations give volume without texture overdraw.
    for (int q = 0; q < nb; q++) {
      float a = q*3.14159f/nb + vn(gx, gz+q)*3.14159f, dx=cosf(a), dz=sinf(a);
      float width=baseR*(lod==0?1.f:1.25f), hh=baseH*(.72f+.65f*vn(gx+q, gz+11));
      float lean=(vn(gx+q*7,gz+5)-.5f)*.32f;
      float lx=-dz*width, lz=dx*width;
      tv(x-lx,h,z-lz,br,bg,bb); tv(x+lx,h,z+lz,br,bg,bb);
      tv(x+dx*lean+lx*.42f,h+hh*.68f,z+dz*lean+lz*.42f,tr,tg,tb);
      tv(x-lx,h,z-lz,br,bg,bb); tv(x+dx*lean+lx*.42f,h+hh*.68f,z+dz*lean+lz*.42f,tr,tg,tb);
      tv(x+dx*lean*1.5f,h+hh,z+dz*lean*1.5f,tr*.88f,tg*.94f,tb);
    }
    // Small wildflower patches: clumped spatial noise, with a few color varieties and a tiny green stem.
    float flower = vn(gx+11,gz+3), flowerPatch = vn((int)floorf(x*.13f+4),(int)floorf(z*.13f-9));
    if (flowerPatch > .48f && flower > (lod==0 ? .80f : .90f)) {
      float fx=x+(vn(gx+23,gz)-.5f)*.34f, fz=z+(vn(gx,gz+23)-.5f)*.34f, fh=h+baseH*.9f;
      float c=vn(gx+31,gz+17), fr=c<.34f?1.f:c<.68f?.95f:1.f, fg=c<.34f?.78f:c<.68f?.88f:.48f, fb=c<.34f?.2f:1.f;
      tv(fx-.018f, h, fz, .12f,.28f,.08f); tv(fx+.018f, h, fz, .12f,.28f,.08f); tv(fx, fh*.55f+h*.45f, fz, .18f,.38f,.1f);
      // two perpendicular petal cards and a warm centre; no separate draw calls
      float rr=lod==0?.095f:.13f;
      tv(fx-rr,fh,fz,fr,fg,fb); tv(fx+rr,fh,fz,fr,fg,fb); tv(fx,fh+rr*.22f,fz,1.f,.84f,.28f);
      tv(fx,fh,fz-rr,fr,fg,fb); tv(fx,fh,fz+rr,fr,fg,fb); tv(fx,fh+rr*.22f,fz,1.f,.84f,.28f);
    }
    (void)patch;
  }
  float y0 = 1e9f, y1 = -1e9f; for (size_t i = 0; i < vb.size(); i++) { y0 = fminf(y0, vb[i].y); y1 = fmaxf(y1, vb[i].y); }
  g->n = (int)vb.size(); if (g->n) { if (!g->vbo) g->vbo = bufGet(); bufUp(g->vbo, vb.data(), g->n); } g->y0 = g->n ? y0 : 0.f; g->y1 = g->n ? y1 : 1.f; g->lod = (signed char)lod; vb.clear();
}
static float gcD2(int cx, int cz) { float x0 = cx*(float)GCN, z0 = cz*(float)GCN, dx = fmaxf(fmaxf(x0 - px, 0.f), px - (x0 + GCN)), dz = fmaxf(fmaxf(z0 - pz, 0.f), pz - (z0 + GCN)); return dx*dx + dz*dz; }
static void drawGrass(const Frus& f) {
  for (auto& kv : gcm) { GChunk* g = kv.second; if (!g->n || gcD2(g->cx, g->cz) > 576.f) continue;
    if (inFrus(f, g->cx*(float)GCN, g->y0 - .1f, g->cz*(float)GCN, g->cx*(float)GCN + GCN, g->y1 + .1f, g->cz*(float)GCN + GCN)) bufDraw(g->vbo, g->n); }
}
// ---- building pieces: instanced drawing (one draw call per piece type instead of one per piece) ----
static bool instOk = false; static GLuint instVbo = 0; static GLint uInst = -1, uShInst = -1; static vector<float> imat[8];
static void drawPcsInst(const Frus& f, bool shadow) {
  if (!mReady || pcs.empty()) return;
  for (int m = 0; m < 8; m++) imat[m].clear();
  for (size_t i = 0; i < pcs.size(); i++) { const Pc& p = pcs[i]; if (!mCnt[p.type]) continue;
    float dx = p.x - px, dz = p.z - pz; if (!shadow && dx*dx + dz*dz > 16900.f) continue;
    if (!inFrus(f, p.x - 3.4f, p.y - 1.f, p.z - 3.4f, p.x + 3.4f, p.y + 6.f, p.z + 3.4f)) continue;
    imat[p.type].insert(imat[p.type].end(), pcx[i].M, pcx[i].M + 16); }
  GLint ui = shadow ? uShInst : uInst, um = shadow ? uShMm : uMm;
  for (int m = 0; m < 8; m++) { int n = (int)imat[m].size()/16; if (!n) continue;
    glBindBuffer(GL_ARRAY_BUFFER, mVbo[m]); attribs(); glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, mIbo[m]);
    if (instOk) {
      glBindBuffer(GL_ARRAY_BUFFER, instVbo); glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)n*64, imat[m].data(), GL_STREAM_DRAW);
      for (int k = 0; k < 4; k++) { glEnableVertexAttribArray(2 + k); glVertexAttribPointer(2 + k, 4, GL_FLOAT, 0, 64, (void*)(size_t)(k*16)); glVertexAttribDivisorANGLE(2 + k, 1); }
      glUniform1f(ui, 1.f); glDrawElementsInstancedANGLE(GL_TRIANGLES, mCnt[m], GL_UNSIGNED_SHORT, 0, n); glUniform1f(ui, 0.f);
      for (int k = 0; k < 4; k++) { glVertexAttribDivisorANGLE(2 + k, 0); glDisableVertexAttribArray(2 + k); }
    } else for (int q = 0; q < n; q++) { glUniformMatrix4fv(um, 1, 0, &imat[m][q*16]); glDrawElements(GL_TRIANGLES, mCnt[m], GL_UNSIGNED_SHORT, 0); }
  }
  glUniformMatrix4fv(um, 1, 0, ID4); glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
}
// ---- plants: the GLB models (3 detail levels each) are uploaded once, every plant node is one instance (scale / yaw / snow in its matrix) ----
struct TrPart { GLuint vbo, ibo; int n; };   // n = number of indices (0: this part does not exist)
struct TrSp { bool ok; float bb[6], R; GLuint tex[4]; TrPart p[3][2]; };
static TrSp TS[NSP]; static GLuint trTexId[64]; static bool trTexOk[64]; static int trStage = 0, trNext = 0;
static vector<float> tinst[NSP][3];
static void restoreModelTex() {   // the plants borrow texture units 2..5 while they are drawn: give them back to the building pieces
  for (int k = 0; k < 6; k++) { glActiveTexture(GL_TEXTURE2 + k); glBindTexture(GL_TEXTURE_2D, (mReady && mTexOk[k]) ? mTex[k] : dummyTex); }
  glActiveTexture(GL_TEXTURE0);
}
static void trLoadStep() {   // runs every frame until everything is uploaded: the textures first, then one species per frame (no long hitch)
  if (trStage == 0) {
    if (!trReady()) return;
    int nt = trNTex(); if (nt > 64) nt = 64;
    if (nt > 0) glGenTextures(nt, trTexId);
    for (int g = 0; g < nt; g++) trTexOk[g] = trTexUp(trTexId[g], g) != 0;
    glActiveTexture(GL_TEXTURE0); trStage = 1; return;
  }
  if (trNext >= NSP) return;
  int s = trNext++; TrSp& t = TS[s]; t.ok = false;
  if (!trN(s, 0, 0, 1) && !trN(s, 0, 1, 1)) return;   // model missing: this species is simply not drawn
  for (int k = 0; k < 6; k++) t.bb[k] = trB(s, k);
  t.R = fmaxf(fmaxf(fabsf(t.bb[0]), fabsf(t.bb[3])), fmaxf(fabsf(t.bb[2]), fabsf(t.bb[5])));
  bool good = true;
  for (int k = 0; k < 4; k++) { int g = trSlot(s, k); if (g < 0) t.tex[k] = dummyTex; else if (g < 64 && trTexOk[g]) t.tex[k] = trTexId[g]; else { t.tex[k] = dummyTex; good = false; } }
  if (!good) return;   // a texture could not be decoded: better no tree than white squares
  for (int l = 0; l < 3; l++) for (int pt = 0; pt < 2; pt++) {
    TrPart& q = t.p[l][pt]; q.vbo = q.ibo = 0; q.n = 0;
    int nv = trN(s, l, pt, 0), ni = trN(s, l, pt, 1); if (!nv || !ni) continue;
    vector<V> v(nv); vector<unsigned short> ix(ni); trV(s, l, pt, (float*)v.data()); trI(s, l, pt, ix.data());
    glGenBuffers(1, &q.vbo); glBindBuffer(GL_ARRAY_BUFFER, q.vbo); glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)nv*sizeof(V), v.data(), GL_STATIC_DRAW);
    glGenBuffers(1, &q.ibo); glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, q.ibo); glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)ni*2, ix.data(), GL_STATIC_DRAW); q.n = ni;
  }
  glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0); glBindBuffer(GL_ARRAY_BUFFER, streamVbo);
  t.ok = true; shDirty = true;
}
static void trDraw(const Frus& f, bool shadow) {   // all plants near the player: culled, sorted into (species, detail level) lists, one instanced draw call per list and part
  if (trStage < 1) return;
  for (int s = 0; s < NSP; s++) for (int l = 0; l < 3; l++) tinst[s][l].clear();
  const float D0 = shadow ? 144.f : 324.f, D1 = shadow ? 1156.f : 3025.f, DF = shadow ? 8100.f : 12544.f;   // squared distances: full detail | medium | draw limit
  int cx = (int)floorf(px/NCS), cz = (int)floorf(pz/NCS); bool any = false;
  for (int j = cz - 2; j <= cz + 2; j++) for (int i = cx - 2; i <= cx + 2; i++) {
    NChunk* c = ncFind(i, j); if (!c) continue;
    for (int k = 0; k < c->n; k++) {
      int ty = c->type[k]; if (ty < 20 || c->hp[k] <= 0) continue;
      int s = ty - 20; const TrSp& t = TS[s]; if (!t.ok) continue;
      float x = c->x[k], z = c->z[k], dx = x - px, dz = z - pz, d2 = dx*dx + dz*dz; if (d2 > DF) continue;
      float sc = c->sc[k], y = c->y[k], R = t.R*sc;
      if (!inFrus(f, x - R, y + t.bb[1]*sc, z - R, x + R, y + t.bb[4]*sc, z + R)) continue;
      float a = c->rt[k], ca = cosf(a)*sc, sa = sinf(a)*sc, M[16] = {ca, 0, -sa, c->sn[k],  0, sc, 0, 0,  sa, 0, ca, 0,  x, y, z, 1};
      vector<float>& L = tinst[s][d2 < D0 ? 0 : d2 < D1 ? 1 : 2]; L.insert(L.end(), M, M + 16); any = true;
    }
  }
  if (!any) return;
  GLint ui = shadow ? uShInst : uInst, um = shadow ? uShMm : uMm; int tris = 0, insts = 0;
  for (int part = 0; part < 2; part++) for (int s = 0; s < NSP; s++) {   // opaque trunks first, then the alpha-tested leaves
    const TrSp& t = TS[s]; if (!t.ok || (tinst[s][0].empty() && tinst[s][1].empty() && tinst[s][2].empty())) continue;
    bool bound = false;
    for (int l = 0; l < 3; l++) { int n = (int)tinst[s][l].size()/16; const TrPart& q = t.p[l][part]; if (!n || !q.n) continue;
      tris += n*q.n/3; if (part == 0) insts += n;
      if (!bound) { for (int k = 0; k < 4; k++) { glActiveTexture(GL_TEXTURE2 + k); glBindTexture(GL_TEXTURE_2D, t.tex[k]); } glActiveTexture(GL_TEXTURE0); bound = true; }
      glBindBuffer(GL_ARRAY_BUFFER, q.vbo); attribs(); glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, q.ibo);
      if (instOk) {
        glBindBuffer(GL_ARRAY_BUFFER, instVbo); glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)n*64, tinst[s][l].data(), GL_STREAM_DRAW);
        for (int k = 0; k < 4; k++) { glEnableVertexAttribArray(2 + k); glVertexAttribPointer(2 + k, 4, GL_FLOAT, 0, 64, (void*)(size_t)(k*16)); glVertexAttribDivisorANGLE(2 + k, 1); }
        glUniform1f(ui, 1.f); glDrawElementsInstancedANGLE(GL_TRIANGLES, q.n, GL_UNSIGNED_SHORT, 0, n); glUniform1f(ui, 0.f);
        for (int k = 0; k < 4; k++) { glVertexAttribDivisorANGLE(2 + k, 0); glDisableVertexAttribArray(2 + k); }
      } else for (int m = 0; m < n; m++) { glUniformMatrix4fv(um, 1, 0, &tinst[s][l][m*16]); glDrawElements(GL_TRIANGLES, q.n, GL_UNSIGNED_SHORT, 0); }
    }
  }
  restoreModelTex(); glUniformMatrix4fv(um, 1, 0, ID4); glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0); glBindBuffer(GL_ARRAY_BUFFER, streamVbo); attribs();
  static int fcnt = 0; if (!shadow && (++fcnt & 15) == 0) trStat(tris, insts);   // for the test page: what the plants cost this frame
}
// ---- streaming: everything near the player is generated / baked with a small time budget per frame, everything far away is released ----
static void resetWorldCaches() {
  for (auto& kv : tcm) { TChunk* c = kv.second; bufPut(c->vbo); bufPut(c->wvbo); c->vbo = c->wvbo = 0; tcPool.push_back(c); } tcm.clear(); tcLast = 0;
  for (auto& kv : nchm) { NChunk* c = kv.second; for (int k = 0; k < 4; k++) cellFree(c->c[k]); delete c; } nchm.clear(); deadList.clear();
  for (auto& kv : gcm) { bufPut(kv.second->vbo); delete kv.second; } gcm.clear();
  for (auto& m : pmesh) bufPut(m.vbo); pmesh.assign(plats.size(), PlatMesh{0, 0});
  pcClear(); shDirty = true;
}
static void unloadFar() {
  for (auto it = tcm.begin(); it != tcm.end();) { TChunk* c = it->second; float dx = (c->cx + .5f)*80.f - px, dz = (c->cz + .5f)*80.f - pz;
    if (dx*dx + dz*dz > 360000.f) { bufPut(c->vbo); bufPut(c->wvbo); c->vbo = c->wvbo = 0; tcPool.push_back(c); if (tcLast == c) tcLast = 0; it = tcm.erase(it); } else ++it; }
  for (auto it = nchm.begin(); it != nchm.end();) { NChunk* c = it->second; float dx = (c->cx + .5f)*64.f - px, dz = (c->cz + .5f)*64.f - pz;
    if (!c->mod && !c->dead && dx*dx + dz*dz > 360000.f) { for (int k = 0; k < 4; k++) cellFree(c->c[k]); delete c; it = nchm.erase(it); }   // generation is deterministic: it comes back identical
    else { if (dx*dx + dz*dz > 40000.f) for (int k = 0; k < 4; k++) if (c->c[k].built) cellFree(c->c[k]); ++it; } }
  for (auto it = gcm.begin(); it != gcm.end();) { GChunk* g = it->second; if (gcD2(g->cx, g->cz) > 6400.f) { bufPut(g->vbo); delete g; it = gcm.erase(it); } else ++it; }
}
static void streamWorld(bool force) {
  PROF_SCOPE(12);
  double t0 = emscripten_get_now(); const double BUD = force ? 1e9 : 3.0; auto over = [&]() { return emscripten_get_now() - t0 > BUD; };
  int cx = (int)floorf(px/NCS), cz = (int)floorf(pz/NCS);
  static vector<pair<float, int> > ord;
  if (ord.empty()) for (int j = -3; j <= 3; j++) for (int i = -3; i <= 3; i++) ord.push_back(make_pair((float)(i*i + j*j), (j + 3)*7 + i + 3));
  if (!force || ord.size() == 49) sort(ord.begin(), ord.end());
  for (size_t q = 0; q < ord.size(); q++) {   // 1. node chunks, nearest first
    int i = cx + ord[q].second % 7 - 3, j = cz + ord[q].second / 7 - 3; if (ncFind(i, j)) continue; ncGen(i, j); if (over()) break; }
  if (!over()) {   // 2. terrain blocks ahead of the player (physics and node placement need them anyway)
    int bx = (int)floorf(px/80.f), bz = (int)floorf(pz/80.f);
    for (int r = 0; r <= 3 && !over(); r++) for (int j = bz - r; j <= bz + r && !over(); j++) for (int i = bx - r; i <= bx + r; i++) { if (r && j != bz - r && j != bz + r && i != bx - r && i != bx + r) continue; if (!tcm.count(ckey(i, j))) { tcGet(i, j); if (over()) break; } }
  }
  if (!over()) {   // 3. 32x32 batches: missing, dirty (harvested / respawned) or at a new detail level
    static vector<pair<float, int> > todo; todo.clear();
    for (int j = cz - 3; j <= cz + 3; j++) for (int i = cx - 3; i <= cx + 3; i++) { NChunk* c = ncFind(i, j); if (!c) continue;
      for (int k = 0; k < 4; k++) { float d2 = cellD2(i, j, k); if (d2 > 19600.f) continue; Cell& e = c->c[k]; int b = bandFor(d2, e.band);
        if (!e.built || e.dirty || b != e.band) todo.push_back(make_pair(d2, (((j - cz + 3)*7 + (i - cx + 3)) << 2) | k)); } }
    sort(todo.begin(), todo.end());
    for (size_t q = 0; q < todo.size(); q++) { int id = todo[q].second, k = id & 3, o = id >> 2, i = cx + o % 7 - 3, j = cz + o/7 - 3; NChunk* c = ncFind(i, j);
      cellBuild(c, k, bandFor(todo[q].first, c->c[k].band)); if (over() && !force) break; }
  }
  if (!over()) {   // 4. grass blocks around the player
    int gx = (int)floorf(px/GCN), gz = (int)floorf(pz/GCN);
    for (int j = gz - 2; j <= gz + 2; j++) for (int i = gx - 2; i <= gx + 2; i++) { float d2 = gcD2(i, j); if (d2 > 900.f) continue; int want = d2 < 144.f ? 0 : 1;
      auto it = gcm.find(ckey(i, j)); GChunk* g; if (it == gcm.end()) { g = new GChunk(); g->cx = i; g->cz = j; g->vbo = 0; g->n = 0; g->lod = -1; gcm[ckey(i, j)] = g; } else g = it->second;
      if (g->lod != want && (g->lod < 0 || (want == 0 ? d2 < 121.f : d2 > 169.f))) { grassBuild(g, want); if (over() && !force) break; } }
  }
  static int fc = 0; if (++fc % 120 == 0) unloadFar();
}
#ifdef PROF
static int profNodes() { int n = 0; for (auto& kv : nchm) n += kv.second->n; return n; }
static int profChunks() { return (int)nchm.size(); }
#endif
static void lightMat(float* LM, float lx, float ly, float lz) {   // orthographic sun view around the player, snapped to the shadow-map texel grid (no shimmering when the map is refreshed)
  ly = fmaxf(ly, .25f); float ll = sqrtf(lx*lx + ly*ly + lz*lz); lx /= ll; ly /= ll; lz /= ll;
  float ex = px + lx*100, ey = py + ly*100, ez = pz + lz*100, fx = -lx, fy = -ly, fz = -lz, ux = 0, uy = 1, uz = 0;
  if (ly > .95f) { uy = 0; uz = 1; }
  float rx = fy*uz - fz*uy, ry = fz*ux - fx*uz, rz = fx*uy - fy*ux, rl = sqrtf(rx*rx + ry*ry + rz*rz); rx /= rl; ry /= rl; rz /= rl;
  float vx = ry*fz - rz*fy, vy = rz*fx - rx*fz, vz = rx*fy - ry*fx;
  const float S = 34.f, nn = 20.f, ff = 180.f;
  { float ts = 2*S/2048.f, a = ex*rx + ey*ry + ez*rz, b = ex*vx + ey*vy + ez*vz, da = floorf(a/ts + .5f)*ts - a, db = floorf(b/ts + .5f)*ts - b; ex += rx*da + vx*db; ey += ry*da + vy*db; ez += rz*da + vz*db; }
  float V4[16] = {rx, vx, -fx, 0,  ry, vy, -fy, 0,  rz, vz, -fz, 0,  -(rx*ex + ry*ey + rz*ez), -(vx*ex + vy*ey + vz*ez), fx*ex + fy*ey + fz*ez, 1};
  float Pl[16] = {1/S,0,0,0, 0,1/S,0,0, 0,0,-2/(ff-nn),0, 0,0,-(ff+nn)/(ff-nn),1};
  mul(LM, Pl, V4);
}
static float shLM[16], shL[3] = {0, 1, 0}, shCx = 1e9f, shCy = 0, shCz = 1e9f;
static void shadowPass(const float* LM) {   // only what touches the sun's box is drawn, from the same static batches as the main pass
  PROF_SCOPE(3);
  Frus lf; frustumFrom(lf, LM);
  glBindFramebuffer(GL_FRAMEBUFFER, shFbo); glViewport(0, 0, 2048, 2048);
  glClearColor(1, 1, 1, 1); glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT); glEnable(GL_DEPTH_TEST);
  glUseProgram(shProg); glUniformMatrix4fv(uShLm, 1, 0, LM); glUniformMatrix4fv(uShMm, 1, 0, ID4); glUniform1f(uShInst, 0.f);
  drawCells(lf, 0, true); trDraw(lf, true); drawPlats(lf, true); drawPcsInst(lf, true);
  glBindBuffer(GL_ARRAY_BUFFER, streamVbo); attribs();
  glBindFramebuffer(GL_FRAMEBUFFER, 0); glViewport(0, 0, W, H); glUseProgram(prog);
}
static void shadowUpdate(const float* L0) {   // the shadow map is cached: redrawn when the sun moved, the player left the middle of the box or a caster changed
  float dl = fabsf(L0[0] - shL[0]) + fabsf(L0[1] - shL[1]) + fabsf(L0[2] - shL[2]), dx = px - shCx, dy = py - shCy, dz = pz - shCz;
  if (!shDirty && dl < .012f && dx*dx + dy*dy + dz*dz < 6.25f) return;
  shDirty = false; shCx = px; shCy = py; shCz = pz; shL[0] = L0[0]; shL[1] = L0[1]; shL[2] = L0[2];
  lightMat(shLM, L0[0], L0[1], L0[2]); shadowPass(shLM);
}
static void setU(const float* A, const float* sky, float lit, float tx) { glUniformMatrix4fv(uM, 1, 0, A); glUniform3fv(uFog, 1, sky); glUniform1f(uLit, lit); glUniform1f(uTx, tx); txOn = tx; }
static void drawWorld(const float* A, const float* sky, float lt, float lo) {
  PROF_SCOPE(15);
  glUniform1f(uInst, 0.f); glUniformMatrix4fv(uMm, 1, 0, ID4);
  collectTerrain();
  setU(A, sky, lo, 0.f); drawCells(fr, 0, false); drawPlats(fr, false);            // 1. opaque rocks: terrain pixels behind them never reach the heavy terrain shader (early z)
  if (mReady) { setU(A, sky, lt, 0.f); drawPcsInst(fr, false); }                    // 2. building pieces, instanced
  setU(A, sky, lt, 1.f); for (size_t i = 0; i < tvis.size(); i++) tcDraw(tvis[i]);  // 3. terrain
  setU(A, sky, lt, 8.f); drawGrass(fr);                                             // 4. grass (wind in the vertex shader)
  setU(A, sky, lo, 0.f); trDraw(fr, false);                                          // 5. plants (GLB models, instanced, alpha-tested leaves)
  setU(A, sky, lt, 2.f); glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA); for (size_t i = 0; i < tvis.size(); i++) tcWater(tvis[i]); glDisable(GL_BLEND);   // 6. water
  setU(A, sky, lt, 0.f);
  if (mReady && gh.on) { Pc g = {gh.type, gh.x, gh.y, gh.z, gh.yaw, gh.hinge, 0.f, 0.f}; float M[16]; pcMat(M, g); glUniform1f(uGt, gh.ok ? 1.f : 2.f); glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA); drawModel(g.type, M, false); glDisable(GL_BLEND); glUniform1f(uGt, 0.f); glUniformMatrix4fv(uMm, 1, 0, ID4); }
  glBindBuffer(GL_ARRAY_BUFFER, streamVbo); attribs(); glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0); txOn = 0;
}
static void quadUV(float x, float y, float z, float ux, float uy, float uz, float vx, float vy, float vz, float e) {
  tv(x-ux-vx, y-uy-vy, z-uz-vz, 0, 0, e); tv(x+ux-vx, y+uy-vy, z+uz-vz, 1, 0, e); tv(x+ux+vx, y+uy+vy, z+uz+vz, 1, 1, e);
  tv(x-ux-vx, y-uy-vy, z-uz-vz, 0, 0, e); tv(x+ux+vx, y+uy+vy, z+uz+vz, 1, 1, e); tv(x-ux+vx, y+uy+vy, z-uz+vz, 0, 1, e);
}
static bool rdirty = true;
static void roadTex() {   // keep the 256x256 unit road mask centred on the player (re-rasterised every 32 units) and bind it for the terrain shader
  float cx = floorf(px/32.f + .5f)*32.f - RWS*.5f, cz = floorf(pz/32.f + .5f)*32.f - RWS*.5f;
  glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, rTex);
  if (rdirty || cx != rox || cz != roz) { rasterWindow(px, pz); glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, RWN, RWN, 0, GL_RGBA, GL_UNSIGNED_BYTE, rbuf); rdirty = false; }
  glUniform1i(uRm, 0); glUniform3f(uRmo, rox, roz, RWS);
}
static void render(float dt) {
  PROF_SCOPE(0);
  float k = fminf(fmaxf(sinf(tod*6.2832f)*2 + .5f, .06f), 1.f), dyf = fminf(fmaxf((k - .06f)/.94f, 0.f), 1.f), wm = fmaxf(0.f, 1.f - fabsf(sinf(tod*6.2832f))*6.f), sky[3] = {.07f + .59f*dyf + .3f*wm, .1f + .7f*dyf + .12f*wm, .2f + .75f*dyf - .05f*wm}, top[3] = {.01f + .19f*dyf, .02f + .4f*dyf, .07f + .78f*dyf};
  glViewport(0, 0, W, H); glClearColor(sky[0]*.7f, sky[1]*.82f, sky[2]*.98f, 1); glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT); glEnable(GL_DEPTH_TEST); roadTex();
  float a = (float)W/H, f = 1/tanf(.55f), n = .1f, fa = 400.f;
  float P[16] = {f/a,0,0,0, 0,f,0,0, 0,0,(fa+n)/(n-fa),-1, 0,0,2*fa*n/(n-fa),0};
  float c = cosf(-yaw), s = sinf(-yaw), c2 = cosf(-pitch), s2 = sinf(-pitch);
  float Ry[16] = {c,0,-s,0, 0,1,0,0, s,0,c,0, 0,0,0,1}, Rx[16] = {1,0,0,0, 0,c2,s2,0, 0,-s2,c2,0, 0,0,0,1};
  float T[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, -px,-(py+1.7f),-pz,1}, A[16], B[16];
  mul(A, Rx, Ry); mul(B, A, T); mul(A, P, B);
  static float tt = 0; tt += dt; float lt = .25f + .75f*k;
  {
    float I0[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    float yh = fminf(fmaxf(-f*tanf(pitch), -2.5f), 2.5f), Y4[4] = {-3.f, yh, fminf(yh + 1.3f, 3.f), 3.f}; const float* C4[4] = {sky, sky, top, top};
    for (int q = 0; q < 3; q++) {
      auto Q = [&](float x, int w) { vb.push_back({x, Y4[w], 0, C4[w][0], C4[w][1], C4[w][2]}); };
      Q(-1, q); Q(1, q); Q(1, q+1); Q(-1, q); Q(1, q+1); Q(-1, q+1);
    }
    glDisable(GL_DEPTH_TEST); txOn = 3; flush(I0, sky, 1);
    float th = tod*6.2832f, sn = sinf(th), cs = cosf(th), sd[3] = {.62f*cs, sn, -.78f*cs};
    glUniform3f(uSd, sd[0], sd[1], sd[2]); glUniform3f(uEy, px, py + 1.7f, pz); glUniform1f(uTm, tt);
    glEnable(GL_BLEND); glBlendFunc(GL_ONE, GL_ONE);
    float night = fminf(fmaxf(.5f - sn*3.f, 0.f), 1.f);
    if (night > .02f) {
      for (int i = 0; i < 170; i++) {
        float a = vn(i, 11)*6.2832f, e = .06f + 1.45f*vn(11, i), qx = cosf(a)*cosf(e), qy = sinf(e), qz = sinf(a)*cosf(e), s0 = .5f + .9f*vn(i, 12), br = night*(.35f + .65f*vn(i, 13));
        float t1x = -qz, t1z = qx, tl = sqrtf(t1x*t1x + t1z*t1z) + 1e-4f; t1x /= tl; t1z /= tl;
        float t2x = qy*t1z, t2y = qz*t1x - qx*t1z, t2z = -qy*t1x, X = px + qx*300, Y = py + 1.7f + qy*300, Z = pz + qz*300;
        tv(X + t1x*s0*1.6f, Y, Z + t1z*s0*1.6f, br, br, br*1.1f);
        tv(X - t1x*s0*.8f + t2x*s0*1.4f, Y + t2y*s0*1.4f, Z - t1z*s0*.8f + t2z*s0*1.4f, br, br, br*1.1f);
        tv(X - t1x*s0*.8f - t2x*s0*1.4f, Y - t2y*s0*1.4f, Z - t1z*s0*.8f - t2z*s0*1.4f, br, br, br*1.1f);
      }
      txOn = 6; flush(A, sky, 1);
    }
    auto body = [&](const float* md, float R0, float tint) {
      quadUV(px + md[0]*100, py + 1.7f + md[1]*100, pz + md[2]*100, .78f*R0, 0, .62f*R0, -.62f*md[1]*R0, -(.78f*md[2] - .62f*md[0])*R0, .78f*md[1]*R0, tint);
    };
    float md[3] = {-sd[0], -sd[1], -sd[2]};
    if (sn > -.06f) body(sd, 75.f, 0.f);
    if (md[1] > -.06f) body(md, 40.f, 1.f);
    txOn = 4; flush(A, sky, 1); glDisable(GL_BLEND);
    {   // cloud layer: full-screen quad, view ray per corner, clouds shaded per pixel
      float th_ = tanf(.55f), fx = -sinf(yaw)*cosf(pitch), fy = sinf(pitch), fz = -cosf(yaw)*cosf(pitch), rx = cosf(yaw), rz = -sinf(yaw), ux = sinf(yaw)*sinf(pitch), uy = cosf(pitch), uz = cosf(yaw)*sinf(pitch);
      const float cxs[4] = {-1, 1, 1, -1}, cys[4] = {-1, -1, 1, 1};
      auto V4 = [&](int i) { float sx = cxs[i]*th_*a, sy = cys[i]*th_; tv(cxs[i], cys[i], 0, fx + rx*sx + ux*sy, fy + uy*sy, fz + rz*sx + uz*sy); };
      V4(0); V4(1); V4(2); V4(0); V4(2); V4(3);
      txOn = 7; glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA); flush(I0, sky, lt); glDisable(GL_BLEND); txOn = 3;
    }
    glEnable(GL_DEPTH_TEST); txOn = 1;
  }
  float th2 = tod*6.2832f, L0[3] = {.62f*cosf(th2), sinf(th2), -.78f*cosf(th2)};
  bool shOn = shOk && L0[1] > .08f;
  frustumFrom(fr, A);
  if (shOn) {
    shadowUpdate(L0);
    glUniformMatrix4fv(uLm, 1, 0, shLM); glUniform1i(uSm, 1); glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, shTex); glActiveTexture(GL_TEXTURE0);
  } else shDirty = true;
  glUniform1f(uSs, shOn ? 1.f : 0.f);
  drawWorld(A, sky, lt, .25f + .75f*k);
  glDisable(GL_DEPTH_TEST);
  float I[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1}, u = fminf(W, H)/400.f;
  if (st.id >= 0) {
    float sx = st.x-st.bx, sy = st.y-st.by, l = hypotf(sx, sy), m = 60*u; if (l > m) { sx *= m/l; sy *= m/l; }
    disc(st.bx, st.by, m, .2f, .2f, .2f); disc(st.bx+sx, st.by+sy, 28*u, .8f, .8f, .8f);
  }
  txOn = 3; flush(I, sky, 1);
  hudT -= dt;
  if (hudT <= 0) {
    hudT = .1f; char b[220];
    snprintf(b, sizeof b, "%d|%d|%d|%d|%d|%d|%d|%d|%d|%d|%s", (int)hp, (int)food, (int)thirst, wood, stone, metal, sulfur, (int)px, (int)pz, (int)(yaw*100), msgT > 0 ? msg : "");
    hud(b);
  }
}
static bool wInit = false;
static void newWorld() {   // (re)generate everything from the menu settings
#ifdef PROF
  double tn0 = emscripten_get_now();
#endif
  Cfg c; c.seed = wcf(0, 12345); c.size = (float)wcf(1, 1.4); c.plat = (int)wcf(2, 8); c.loops = (int)wcf(3, 1); c.trails = (int)wcf(4, 8);
  genWorld(c); resetWorldCaches();
  px = spX; pz = spZ; py = meshH(px, pz); vy = 0; yaw = spYaw; pitch = 0; blks.clear();
  static unsigned char img[512*512*4]; fillMap(img, 512); mapImg(img, 512, WE);
  rdirty = true; rasterWindow(px, pz);   // the grass bake needs the road mask around the player
  streamWorld(true);
  static bool first = true; if (!first) { char b[64]; snprintf(b, sizeof b, "Новый мир · сид %u", (unsigned)c.seed); say(b); msgT = 4; } first = false;
#ifdef PROF
  { char b[80]; snprintf(b, sizeof b, "PROF-NEW %.0f ms (world generation + first streaming)", emscripten_get_now() - tn0); EM_ASM({ console.log(UTF8ToString($0)); }, b); }
#endif
  wready(c.seed, WS.nPlat, WS.roadLen, WS.trailLen);
}
static void loop() {
  double now = emscripten_get_now(); float dt = fminf((float)((now-last)/1000), .05f); last = now;
  double cw, ch; emscripten_get_element_css_size("#canvas", &cw, &ch);
  K = fminf((float)emscripten_get_device_pixel_ratio(), 1.5f);
  int nw = (int)(cw*K), nh = (int)(ch*K);
  if (nw != W || nh != H) { W = nw; H = nh; emscripten_set_canvas_element_size("#canvas", W, H); }
  if (W < 2 || H < 2) return;
  if (!wInit || gk(12)) { newWorld(); wInit = true; }
  streamWorld(false); update(dt); render(dt);
#ifdef PROF
  profFrame();
#endif
}
int main() {
  EmscriptenWebGLContextAttributes a; emscripten_webgl_init_context_attributes(&a); a.majorVersion = 1; a.antialias = 0;
  auto gctx = emscripten_webgl_create_context("#canvas", &a); emscripten_webgl_make_context_current(gctx); emscripten_webgl_enable_extension(gctx, "OES_standard_derivatives"); instOk = emscripten_webgl_enable_extension(gctx, "ANGLE_instanced_arrays");
#ifdef NO_INST
  instOk = false;
#endif
  GLuint p = glCreateProgram(); glAttachShader(p, sh(GL_VERTEX_SHADER, VS)); glAttachShader(p, sh(GL_FRAGMENT_SHADER, FS));
  glBindAttribLocation(p, 0, "p"); glBindAttribLocation(p, 1, "c"); glBindAttribLocation(p, 2, "i0"); glBindAttribLocation(p, 3, "i1"); glBindAttribLocation(p, 4, "i2"); glBindAttribLocation(p, 5, "i3"); glLinkProgram(p); glUseProgram(p); prog = p;
  uInst = glGetUniformLocation(p, "inst"); uM = glGetUniformLocation(p, "m"); uFog = glGetUniformLocation(p, "fog"); uLit = glGetUniformLocation(p, "lit"); uTx = glGetUniformLocation(p, "tx"); uSd = glGetUniformLocation(p, "sd"); uEy = glGetUniformLocation(p, "ey"); uTm = glGetUniformLocation(p, "tm"); uLm = glGetUniformLocation(p, "lm"); uSm = glGetUniformLocation(p, "sm"); uSs = glGetUniformLocation(p, "ss"); uMm = glGetUniformLocation(p, "mm"); uGt = glGetUniformLocation(p, "gt"); uRm = glGetUniformLocation(p, "rm"); uRmo = glGetUniformLocation(p, "rmo"); glUniform1i(uRm, 0); glUniformMatrix4fv(uMm, 1, 0, ID4); { const char* tn[6] = {"t0", "t1", "t2", "t3", "t4", "t5"}; for (int k = 0; k < 6; k++) glUniform1i(glGetUniformLocation(p, tn[k]), 2 + k); }
  { GLuint dt; glGenTextures(1, &dt); dummyTex = dt; glBindTexture(GL_TEXTURE_2D, dt); unsigned char w4[4] = {255, 255, 255, 255}; glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, w4); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST); for (int k = 2; k <= 7; k++) { glActiveTexture(GL_TEXTURE0 + k); glBindTexture(GL_TEXTURE_2D, dt); } glActiveTexture(GL_TEXTURE0); }
  { glGenTextures(1, &rTex); glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, rTex); glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, RWN, RWN, 0, GL_RGBA, GL_UNSIGNED_BYTE, 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE); }   // road / trail / platform mask
  GLuint bf; glGenBuffers(1, &bf); glBindBuffer(GL_ARRAY_BUFFER, bf); streamVbo = bf; tcIndices(); glGenBuffers(1, &instVbo); glBindBuffer(GL_ARRAY_BUFFER, bf);
  glEnableVertexAttribArray(0); glEnableVertexAttribArray(1);
  glVertexAttribPointer(0, 3, GL_FLOAT, 0, 24, (void*)0); glVertexAttribPointer(1, 3, GL_FLOAT, 0, 24, (void*)12);
  {
    const char* svs = "attribute vec3 p;attribute vec3 c;attribute vec4 i0;attribute vec4 i1;attribute vec4 i2;attribute vec4 i3;uniform mat4 lm;uniform mat4 mm;uniform float inst;varying float z;varying vec3 vc;void main(){mat4 M=inst>.5?mat4(i0,i1,i2,i3):mm;vec4 w=M*vec4(p,1.);w.w=1.;gl_Position=lm*w;z=gl_Position.z*.5+.5;vc=c;}";
    const char* sfs = "precision highp float;varying float z;varying vec3 vc;uniform sampler2D t0;uniform sampler2D t1;uniform sampler2D t2;uniform sampler2D t3;"
      "void main(){if(vc.b>29.5){float id=vc.b-29.5;vec2 tc=vc.rg;float a=id<1.?texture2D(t0,tc).a:(id<2.?texture2D(t1,tc).a:(id<3.?texture2D(t2,tc).a:texture2D(t3,tc).a));if(a<.5)discard;}vec4 e=fract(z*vec4(1.,255.,65025.,16581375.));e-=e.yzww*vec4(1./255.,1./255.,1./255.,0.);gl_FragColor=e;}";
    shProg = glCreateProgram(); glAttachShader(shProg, sh(GL_VERTEX_SHADER, svs)); glAttachShader(shProg, sh(GL_FRAGMENT_SHADER, sfs));
    glBindAttribLocation(shProg, 0, "p"); glBindAttribLocation(shProg, 1, "c"); glBindAttribLocation(shProg, 2, "i0"); glBindAttribLocation(shProg, 3, "i1"); glBindAttribLocation(shProg, 4, "i2"); glBindAttribLocation(shProg, 5, "i3"); glLinkProgram(shProg); uShInst = glGetUniformLocation(shProg, "inst"); uShLm = glGetUniformLocation(shProg, "lm"); uShMm = glGetUniformLocation(shProg, "mm"); glUseProgram(shProg); glUniformMatrix4fv(uShMm, 1, 0, ID4); { const char* tn[4] = {"t0", "t1", "t2", "t3"}; for (int k = 0; k < 4; k++) glUniform1i(glGetUniformLocation(shProg, tn[k]), 2 + k); } glUseProgram(p);
    glGenTextures(1, &shTex); glBindTexture(GL_TEXTURE_2D, shTex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 2048, 2048, 0, GL_RGBA, GL_UNSIGNED_BYTE, 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    GLuint rb; glGenRenderbuffers(1, &rb); glBindRenderbuffer(GL_RENDERBUFFER, rb); glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT16, 2048, 2048);
    glGenFramebuffers(1, &shFbo); glBindFramebuffer(GL_FRAMEBUFFER, shFbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, shTex, 0); glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, rb);
    shOk = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE; glBindFramebuffer(GL_FRAMEBUFFER, 0);
  }
  say("Лево: ходьба · Право: камера"); msgT = 6;
  py = meshH(0, 0);
  emscripten_set_main_loop(loop, 0, 1);
}
