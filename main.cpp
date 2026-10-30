// Mini-Rust: survival sandbox. C++ -> WebAssembly (Emscripten), touch controls for iPhone.
#include <emscripten.h>
#include <emscripten/html5.h>
#include <GLES2/gl2.h>
#include <cmath>
#include <cstdio>
#include <vector>
#include <algorithm>
using namespace std;

struct V { float x, y, z, r, g, b; };
struct Node { float x, z, t; int type, hp; float y; };      // 0 tree, 1 rock, 2 berry bush
struct Blk { float x, z, w, d; bool wall; float y; };       // building piece
struct Fin { int id; float x, y, bx, by; };        // finger

static vector<V> vb; static vector<Node> nodes; static vector<Blk> blks;
static GLint uM, uFog, uLit, uTx, uSd, uEy, uTm, uLm, uSm, uSs, uShLm; static GLuint prog, shProg, shFbo, shTex; static bool shOk = false; static vector<V> sv; static float txOn = 1; static int W = 0, H = 0; static float K = 1;
static float px = 0, py = 0, pz = 0, vy = 0, yaw = 0, pitch = 0, hp = 100, food = 100;
static float tod = .1f, cd = 0, hudT = 0, msgT = 0; static int wood = 60, stone = 0, metal = 0, sulfur = 0, mode = 0; static float thirst = 100;
static char msg[96] = ""; static Fin st = {-1}, lk = {-1};
static double last = 0; static unsigned rs = 12345;

EM_JS(int, gk, (int i), { var v = window.G[i] | 0; if (i == 1) window.G[1] = 0; return v; });
EM_JS(float, gf, (int i), { var v = window.G[i] || 0; if (i > 4) window.G[i] = 0; return v; });
EM_JS(void, hud, (const char* s), { if (window.hudUpdate) window.hudUpdate(UTF8ToString(s)); });

static const char* VS = "attribute vec3 p;attribute vec3 c;uniform mat4 m;uniform mat4 lm;uniform float tx;uniform float tm;varying vec3 vc;varying float d;varying vec3 wp;varying vec4 ls;varying float wo;"
  "void main(){vec3 q=p;float o=0.;if(tx>1.5&&tx<2.5){o=.16*sin(tm*.7)+.1*sin(dot(p.xz,vec2(.06,.045))-tm*1.15);q.y+=o;}gl_Position=m*vec4(q,1.);vc=c;d=gl_Position.w;wp=q;ls=lm*vec4(q,1.);wo=o;}";
static const char* FS = "#extension GL_OES_standard_derivatives : enable\n"
  "precision highp float;varying vec3 vc;varying float d;varying vec3 wp;varying vec4 ls;varying float wo;uniform vec3 fog;uniform vec3 sd;uniform vec3 ey;uniform float lit;uniform float tx;uniform float tm;uniform sampler2D sm;uniform float ss;"
  "float h(vec2 p){vec3 q=fract(vec3(p.xyx)*.1031);q+=dot(q,q.yzx+33.33);return fract((q.x+q.y)*q.z);}"
  "float n(vec2 p){vec2 i=floor(p),f=fract(p);f=f*f*f*(f*(f*6.-15.)+10.);return mix(mix(h(i),h(i+vec2(1.,0.)),f.x),mix(h(i+vec2(0.,1.)),h(i+vec2(1.,1.)),f.x),f.y);}"
  "float fb(vec2 p){float a=.5,s=0.;for(int i=0;i<5;i++){s+=a*n(p);p=mat2(.8,.6,-.6,.8)*p*2.03+vec2(17.1,9.2);a*=.5;}return s;}"
  "float wv(vec2 p){p=mat2(.8,.6,-.6,.8)*p;float a=n(p*.35+vec2(tm*.25,tm*.15))*.6;p=mat2(.8,.6,-.6,.8)*p*2.1;return a+n(p-vec2(tm*.45,tm*.3))*.28+n(p*2.3+tm*.6)*.12;}"
  "float lf(vec2 q){vec2 i=floor(q),f=fract(q)-.5;float a=h(i)*6.283;f+=(vec2(h(i+7.),h(i+13.))-.5)*.3;vec2 r=vec2(cos(a)*f.x+sin(a)*f.y,-sin(a)*f.x+cos(a)*f.y);return 1.-step(1.,length(r/vec2(.52,.24)));}float lfm(vec2 u){return max(lf(u*5.),lf(u*6.+3.1))*step(length(u-.5),.5);}"
  "float up(vec4 c){return dot(c,vec4(1.,1./255.,1./65025.,1./16581375.));}"
  "float shw(float bs){vec3 q=ls.xyz/ls.w*.5+.5;if(q.x<0.||q.x>1.||q.y<0.||q.y>1.||q.z>1.)return 1.;float s=(q.z-bs>up(texture2D(sm,q.xy)))?0.:1.;for(int i=0;i<8;i++){float a=float(i)*.785;vec2 o=vec2(cos(a),sin(a))*.0011;s+=(q.z-bs>up(texture2D(sm,q.xy+o)))?0.:1.;}float e=max(abs(q.x-.5),abs(q.y-.5))*2.;return mix(1.,s/9.,1.-smoothstep(.8,1.,e));}"
  "void main(){vec3 c=vc;float a=1.;"
  "if(tx<.5){vec3 al=vc;vec3 N=normalize(cross(dFdx(wp),dFdy(wp)));if(dot(N,ey-wp)<0.)N=-N;bool fo=vc.b>1.5,rk=vc.r>5.&&!fo;"
  "if(fo){if(lfm(vc.xy)<.5)discard;float k=vc.b-2.,pl=floor(k+.01),t=(k-pl)/.99;vec3 a0,a1;if(pl<.5){a0=vec3(.07,.2,.05);a1=vec3(.45,.62,.14);}else if(pl<1.5){a0=vec3(.04,.14,.08);a1=vec3(.2,.42,.2);}else if(pl<2.5){a0=vec3(.1,.2,.05);a1=vec3(.38,.52,.14);}else if(pl<3.5){a0=vec3(.03,.11,.08);a1=vec3(.14,.3,.2);}else if(pl<4.5){a0=vec3(.08,.18,.07);a1=vec3(.26,.4,.16);}else{a0=vec3(.3,.26,.1);a1=vec3(.62,.55,.26);}al=mix(a0,a1,t)*(.85+.3*h(floor(vc.xy*5.)));if(pl>2.5&&pl<4.5)al=mix(al,vec3(.93,.96,1.),smoothstep(.15,.65,N.y)*.9);}"
  "if(rk){float m10=floor(vc.r/10.),mt=m10-1.,sn=0.;if(mt>7.5){mt-=8.;sn=1.;}vec3 bs=mt<.5?vec3(.62,.62,.64):(mt<1.5?vec3(.3,.2,.16):(mt<2.5?vec3(.2,.22,.17):(mt<3.5?vec3(.42,.4,.38):(mt<4.5?vec3(.2,.42,.16):(mt<5.5?vec3(.05,.15,.05):(mt<6.5?vec3(.08,.18,.07):vec3(.3,.26,.12)))))));"
  "float st=n(vec2(wp.y*5.+n(wp.xz*1.1)*3.,(wp.x+wp.z)*.35)),gr=n(wp.xz*16.+wp.y*13.),vz=abs(n(wp.xz*2.4+wp.y*2.1)-.5),vein=1.-smoothstep(0.,.05,vz);al=bs*(.7+.45*st)*(.82+.3*gr);"
  "if(mt<.5){al=mix(al,vec3(.35,.36,.4),vein*.7);al=mix(al,vec3(.9,.9,.92),smoothstep(.72,.8,n(wp.xz*9.+wp.y*8.))*.5);al=mix(al,vec3(.2,.36,.12),smoothstep(.62,.9,N.y)*smoothstep(.5,.7,n(wp.xz*5.))*.4*(1.-sn));}else if(mt<1.5){al=mix(al,vec3(.12,.07,.06),vein*.7);al=mix(al,vec3(.8,.4,.14),smoothstep(.66,.72,n(wp.xz*10.+wp.y*9.))*.85);}else if(mt<2.5){al=mix(al,vec3(.88,.82,.16),vein*.95);al=mix(al,vec3(.8,.78,.2),smoothstep(.7,.76,n(wp.xz*8.+wp.y*7.))*.6);}else if(mt>3.5&&mt<4.5){al*=.8+.2*sin((wp.x+wp.z)*16.)+.2*h(floor(wp.xz*11.+wp.y*7.));}"
  "if(mt>5.5&&mt<6.5)sn=1.;if(sn>.5)al=mix(al,vec3(.93,.96,1.),smoothstep(.3,.7,N.y)*.92);}"
  "vec3 L=normalize(sd);float dl=dot(N,L),dif=fo?(max(dl,0.)+.25*max(-dl,0.)):max(dl,0.),hm=.5+.5*N.y,si=clamp(L.y*2.5+.2,0.,1.),nt=1.-clamp(L.y*4.+.6,0.,1.);"
  "float g=n(wp.xz*5.+wp.y*3.)*.55+n(wp.xz*19.+wp.y*11.)*.45;c=al*(fo?.8+.3*g:(rk?1.:.72+.56*g));float sh=1.;if(ss>.5)sh=shw(.0012+.004*(1.-dif));c*=(.22+.3*hm)*mix(vec3(.9,.95,1.1),vec3(.5,.65,1.25),nt)*mix(lit,.62,nt)+si*dif*sh*vec3(.95,.88,.72)+vec3(.45,.55,.95)*.5*max(-dl,0.)*nt;}"
  "else if(tx<1.5){vec2 p=wp.xz;float hh=wp.y,sl=1.-vc.g,bio=vc.b,li=vc.r,big=n(p*.07),mid=n(p*.6),fine=n(p*3.)*.5+n(p*13.)*.5;"
  "vec3 gr=mix(vec3(.2,.4,.12),vec3(.34,.5,.16),mid);gr=mix(gr,gr*vec3(1.35,1.1,.55),smoothstep(.45,.8,big)*.8);vec3 sa=mix(vec3(.74,.66,.46),vec3(.83,.75,.55),fine),ds=mix(vec3(.82,.6,.34),vec3(.92,.72,.44),mid),sw=mix(vec3(.86,.9,.97),vec3(.98,.99,1.),fine),rkc=mix(vec3(.46,.44,.4),vec3(.6,.57,.52),fine);"
  "float sf=1.-smoothstep(.8,2.3,hh+(n(p*.35)-.5)*1.6+(fine-.5)*.6),dz=smoothstep(.2,.8,bio)*(1.-smoothstep(1.2,1.8,bio)),sz=smoothstep(1.2,1.8,bio);"
  "vec3 al=mix(gr,sa,sf);al=mix(al,ds*(.9+.2*sin(p.x*.5+p.y*.3+n(p*.15)*6.)),dz);al=mix(al,sw,sz*(1.-.5*sf));al*=mix(.62,1.,smoothstep(-.3,.5,hh));al=mix(al,rkc*mix(1.,1.15,sz),smoothstep(.1,.2,sl+(fine-.5)*.05));"
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
static float vn(int x, int z) { unsigned h = x*374761393u + z*668265263u; h = (h ^ (h >> 13))*1274126177u; return ((h ^ (h >> 16)) & 0xffff)/65535.f; }
static float pn(float x, float z) {
  int ix = (int)floorf(x), iz = (int)floorf(z); float fx = x-ix, fz = z-iz; fx = fx*fx*(3-2*fx); fz = fz*fz*(3-2*fz);
  return (vn(ix,iz)*(1-fx) + vn(ix+1,iz)*fx)*(1-fz) + (vn(ix,iz+1)*(1-fx) + vn(ix+1,iz+1)*fx)*fz;
}
static float biomeAt(float x, float z) {   // 0 temperate, 1 desert (ahead of spawn), 2 winter (behind)
  float r = hypotf(x, z), bn = atan2f(z, x)/6.2832f + .5f + (pn(x*.006f, z*.006f) - .5f)*.18f, w = fminf(1.f, fmaxf(0.f, (r - 50.f)/90.f));
  float d = fminf(1.f, fmaxf(0.f, (bn - .1f)/.08f))*(1.f - fminf(1.f, fmaxf(0.f, (bn - .34f)/.08f)));
  float sn = fminf(1.f, fmaxf(0.f, (bn - .58f)/.08f))*(1.f - fminf(1.f, fmaxf(0.f, (bn - .82f)/.08f)));
  return (d + 2.f*sn)*w;
}
static float hgt(float x, float z) {   // island radius ~330: irregular coast, hills, mountains, sea cliffs, terraces
  float ang = atan2f(z, x), cu = cosf(ang), su = sinf(ang), r = hypotf(x, z);
  float rr = 330.f*(.72f + .45f*pn(cu*1.7f + 5, su*1.7f + 5) + .1f*(pn(cu*7.f, su*7.f) - .5f)), dc = rr - r;
  float e = pn(x*.015f, z*.015f)*.6f + pn(x*.04f+9, z*.04f)*.3f + pn(x*.13f, z*.13f+4)*.1f;
  float h = (e - .3f)*16*fminf(fmaxf(dc/33.f, 0.f), 1.f) + fminf(fmaxf(dc, -60.f), 120.f)*.06f - 1.f;
  h += fmaxf(0.f, pn(x*.005f + 50, z*.005f) - .55f)*70.f*fminf(fmaxf((dc - 60.f)/120.f, 0.f), 1.f)*fminf(fmaxf((r - 40.f)/80.f, 0.f), 1.f);
  float cl = fminf(1.f, fmaxf(0.f, (pn(cu*2.3f + 11, su*2.3f + 11) - .48f)*7.f));
  if (cl > 0.f) { float top = 9.f + 12.f*pn(x*.012f, z*.012f + 7); h = fmaxf(h, h + (top - h)*cl*fminf(1.f, fmaxf(0.f, dc/16.f))); }
  if (h > 2.f) { float t = h/2.6f, fl = floorf(t), w = fminf(1.f, fmaxf(0.f, (pn(x*.03f+3, z*.03f) - .5f)*6.f)); h += ((fl + fminf(1.f, fmaxf(0.f, (t - fl - .6f)*2.5f)))*2.6f - h)*w; }
  if (h < -.2f) h = -.2f + (h + .2f)*3.f;
  return h;
}
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
static void flush(const float* m, const float* fog, float lit) {
  glUniformMatrix4fv(uM, 1, 0, m); glUniform3fv(uFog, 1, fog); glUniform1f(uLit, lit); glUniform1f(uTx, txOn);
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
  b.y = hgt(b.x, b.z);
  return b;
}
static void act() {
  cd = .4f; float fx = -sinf(yaw), fz = -cosf(yaw);
  if (mode) {
    Blk b = ghost(); int cost = mode == 1 ? 30 : 20;
    if (wood < cost) { say("Нужно больше дерева"); return; }
    for (auto& o : blks) if (o.x == b.x && o.z == b.z && o.wall == b.wall) return;
    wood -= cost; blks.push_back(b); return;
  }
  Node* best = 0; float bd = 4.5f;
  for (auto& n : nodes) {
    if (n.hp <= 0 || n.type == 2 || n.type == 8 || n.type == 10) continue;
    float dx = n.x-px, dz = n.z-pz, d = sqrtf(dx*dx + dz*dz);
    if (d < bd && dx*fx + dz*fz > .6f*d) { bd = d; best = &n; }
  }
  if (!best) { if (hgt(px + fx*3, pz + fz*3) < -.2f) { thirst = fminf(100.f, thirst + 18); say("Пьёшь воду"); } else say("Мимо"); return; }
  if (--best->hp <= 0) best->t = 60;
  if (best->type == 0 || best->type == 3 || best->type == 4 || best->type == 7) { wood += 10; say("+10 дерева"); }
  else if (best->type == 1) { stone += 8; say("+8 камня"); }
  else if (best->type == 5) { metal += 6; say("+6 металла"); }
  else if (best->type == 6) { sulfur += 6; say("+6 серы"); }
  else { thirst = fminf(100.f, thirst + 22); say("+22 воды"); }
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
  tod += dt/240; if (tod > 1) tod -= 1; cd -= dt; msgT -= dt;
  food -= dt*.4f; thirst = fmaxf(0.f, thirst - dt*.55f);
  if (food <= 0 || thirst <= 0) { food = fmaxf(food, 0.f); hp -= dt*2; } else if (food > 50 && thirst > 50 && hp < 100) hp = fminf(100, hp + dt*2);
  if (hp <= 0) { hp = 100; food = 100; thirst = 100; wood = stone = metal = sulfur = 0; px = pz = 0; say("Ты умер. Ресурсы потеряны"); }
  float sx = gf(3), sy = gf(4);
  yaw -= gf(5)*.006f; pitch = fminf(fmaxf(pitch - gf(6)*.006f, -1.4f), 1.4f);
  float fx = -sinf(yaw), fz = -cosf(yaw), rx = cosf(yaw), rz = -sinf(yaw), sp = 5.f*(hypotf(sx, sy) > .95f ? 1.4f : 1.f);
  float ox = px, oz = pz; px += (fx*-sy + rx*sx)*sp*dt; pz += (fz*-sy + rz*sx)*sp*dt;
  for (auto& n : nodes) {
    if (n.hp > 0) { float r = n.type == 0 || n.type == 4 || n.type == 7 ? .9f : n.type == 1 || n.type == 5 || n.type == 6 ? 1.3f : n.type == 9 ? .6f : .5f; if (n.type != 2 && n.type != 8 && n.type != 10) push(px, pz, n.x-r, n.z-r, n.x+r, n.z+r); }
    else if ((n.t -= dt) <= 0) n.hp = n.type == 0 || n.type == 7 ? 5 : n.type == 1 || n.type == 5 || n.type == 6 ? 7 : n.type == 3 ? 4 : n.type == 4 ? 5 : n.type == 9 ? 3 : 1;
  }
  for (auto& b : blks) if (b.wall) push(px, pz, b.x-b.w/2, b.z-b.d/2, b.x+b.w/2, b.z+b.d/2);
  if (hgt(px, pz) < -.5f) { px = ox; pz = oz; }
  float gy = hgt(px, pz); for (auto& b : blks) if (!b.wall && fabsf(px-b.x) < 2 && fabsf(pz-b.z) < 2) gy = fmaxf(gy, b.y + .3f);
  if (gk(1) && py <= gy + .01f) vy = 7;
  vy -= 20*dt; py += vy*dt; if (py < gy + .25f && vy <= 0) { py = gy; vy = 0; }
  mode = gk(2);
  if (gk(0) && cd <= 0) act();
}
static float TH[43][43], TX0, TZ0;
static void terrain() {
  const int N = 21; const float C = 5.f; int gx0 = (int)floorf(px/C), gz0 = (int)floorf(pz/C);
  static float Y[2*N+1][2*N+1], Q[2*N+1][2*N+1][3];
  for (int i = 0; i <= 2*N; i++) for (int j = 0; j <= 2*N; j++) {
    float x = (gx0-N+i)*C, z = (gz0-N+j)*C, h = hgt(x, z);
    float dx = hgt(x-2,z) - hgt(x+2,z), dz = hgt(x,z-2) - hgt(x,z+2), l = sqrtf(dx*dx + 16 + dz*dz), ny = 4/l;
    float lit = .5f + .5f*fmaxf(0.f, (dx*.4f + 3.f - dz*.5f)/l);
    Y[i][j] = TH[i][j] = h; Q[i][j][0] = lit; Q[i][j][1] = ny; Q[i][j][2] = biomeAt(x, z);
  }
  TX0 = (gx0-N)*C; TZ0 = (gz0-N)*C;
  auto P = [&](int i, int j) { vb.push_back({(gx0-N+i)*C, Y[i][j], (gz0-N+j)*C, Q[i][j][0], Q[i][j][1], Q[i][j][2]}); };
  for (int i = 0; i < 2*N; i++) for (int j = 0; j < 2*N; j++) { P(i,j); P(i+1,j); P(i+1,j+1); P(i,j); P(i+1,j+1); P(i,j+1); }
}
static void seg(float x0, float y0, float z0, float x1, float y1, float z1, float r0, float r1, float cr, float cg, float cb) {
  for (int i = 0; i < 6; i++) {
    float a0 = i*1.0472f, a1 = a0 + 1.0472f, l = .7f + .3f*cosf(a0 - 2.2f);
    float p0x = x0 + r0*cosf(a0), p0z = z0 + r0*sinf(a0), p1x = x0 + r0*cosf(a1), p1z = z0 + r0*sinf(a1);
    float q0x = x1 + r1*cosf(a0), q0z = z1 + r1*sinf(a0), q1x = x1 + r1*cosf(a1), q1z = z1 + r1*sinf(a1);
    vb.push_back({p0x, y0, p0z, cr*l, cg*l, cb*l}); vb.push_back({p1x, y0, p1z, cr*l, cg*l, cb*l}); vb.push_back({q1x, y1, q1z, cr*l, cg*l, cb*l});
    vb.push_back({p0x, y0, p0z, cr*l, cg*l, cb*l}); vb.push_back({q1x, y1, q1z, cr*l, cg*l, cb*l}); vb.push_back({q0x, y1, q0z, cr*l, cg*l, cb*l});
  }
}
static void palm(float x, float y, float z) {
  float an = vn((int)(x*5), (int)(z*5))*6.28f, bx = cosf(an)*.1f, bz = sinf(an)*.1f, Tx = x, Ty = y, Tz = z;
  for (int k = 0; k < 5; k++) {
    float nx = x + bx*(k+1)*(k+1), nz = z + bz*(k+1)*(k+1), ny = y + (k+1)*1.35f;
    seg(Tx, Ty - (k ? 0.f : .3f), Tz, nx, ny, nz, .3f - .03f*k, .3f - .03f*(k+1), .5f, .38f, .24f); Tx = nx; Ty = ny; Tz = nz;
  }
  for (int q = 0; q < 7; q++) {
    float b = q*.8976f + vn(q, (int)x)*.4f, ex = cosf(b), ez = sinf(b), sx = -ez, sz = ex;
    for (int i = 0; i < 3; i++) {
      float t0 = i/3.f, t1 = (i+1)/3.f, w0 = .6f*(1 - t0*.85f), w1 = .6f*(1 - t1*.85f);
      float X0 = Tx + ex*3.6f*t0, Y0 = Ty + 1.2f*sinf(t0*2.2f) - 2.8f*t0*t0, Z0 = Tz + ez*3.6f*t0;
      float X1 = Tx + ex*3.6f*t1, Y1 = Ty + 1.2f*sinf(t1*2.2f) - 2.8f*t1*t1, Z1 = Tz + ez*3.6f*t1;
      vb.push_back({X0 - sx*w0, Y0, Z0 - sz*w0, .12f, .36f, .1f}); vb.push_back({X0 + sx*w0, Y0, Z0 + sz*w0, .12f, .36f, .1f}); vb.push_back({X1 + sx*w1, Y1, Z1 + sz*w1, .3f, .6f, .16f});
      vb.push_back({X0 - sx*w0, Y0, Z0 - sz*w0, .12f, .36f, .1f}); vb.push_back({X1 + sx*w1, Y1, Z1 + sz*w1, .3f, .6f, .16f}); vb.push_back({X1 - sx*w1, Y1, Z1 - sz*w1, .3f, .6f, .16f});
    }
  }
}
static void grass(float tt) {   // short dense tufts
  int cx = (int)floorf(px), cz = (int)floorf(pz);
  for (int i = -20; i <= 20; i++) for (int j = -20; j <= 20; j++) {
    int gx = cx + i, gz = cz + j; if (vn(gx*3+1, gz*5+2) < .2f) continue;
    float x = gx + vn(gx, gz), z = gz + vn(gz+7, gx), dx = x-px, dz = z-pz, d2 = dx*dx + dz*dz;
    if (d2 > 400) continue;
    float h = hgt(x, z); if (h < 1.5f || biomeAt(x, z) > .3f) continue;
    float kind = vn(gx+5, gz+1), s = .22f + .3f*vn(gx+3, gz+9), w = sinf(tt*2 + x*.7f + z*.5f)*.08f*s;
    float cr = .12f, cg = .3f, cb = .07f, tr = .45f, tg = .66f, tb = .2f;
    if (kind < .4f) { cr = .22f; cg = .32f; cb = .09f; tr = .72f; tg = .72f; tb = .27f; }
    else if (kind > .88f) { cr = .25f; cg = .24f; cb = .1f; tr = .62f; tg = .55f; tb = .25f; }
    int nb = d2 < 100 ? 4 : 2;
    for (int q = 0; q < nb; q++) {
      float a = q*6.2832f/nb + vn(gx, gz+q)*1.2f, bx = cosf(a)*.04f, bz = sinf(a)*.04f, hh = s*(.7f + .6f*vn(gx+q, gz));
      tv(x-bx, h, z-bz, cr, cg, cb); tv(x+bx, h, z+bz, cr, cg, cb);
      tv(x + w + cosf(a)*.12f*s, h + hh, z + w*.5f + sinf(a)*.12f*s, tr, tg, tb);
    }
    float fl = vn(gx+11, gz+3);
    if (fl > .93f && d2 < 150) { float fr = fl > .97f ? .95f : .75f, fg = fl > .965f ? .85f : .6f, fb = fl > .955f ? .3f : .95f; float hy = h + s*1.1f; tv(x-.05f, hy, z, fr, fg, fb); tv(x+.05f, hy, z, fr, fg, fb); tv(x, hy + .1f, z, fr, fg, fb); }
  }
}
static void cone(float x, float y, float z, float rad, float h, float r, float g, float b) {
  for (int i = 0; i < 8; i++) {
    float a0 = i*.7854f, a1 = a0 + .7854f, l = .5f + .5f*fmaxf(0.f, cosf(a0 + .39f - 2.2f));
    vb.push_back({x+rad*cosf(a0), y, z+rad*sinf(a0), r*l, g*l, b*l});
    vb.push_back({x+rad*cosf(a1), y, z+rad*sinf(a1), r*l, g*l, b*l});
    vb.push_back({x, y+h, z, r*l*1.15f, g*l*1.15f, b*l*1.15f});
  }
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
static void cardN(float x, float y, float z, float nx, float ny, float nz, float w, float roll, int pal, float t) {   // leaf card lying on a surface with normal n
  float ux = nz, uz = -nx, ul = sqrtf(ux*ux + uz*uz); if (ul < 1e-3f) { ux = 1; uz = 0; ul = 1; } ux /= ul; uz /= ul;
  float vx = -ny*ux*0 + ny*uz, vy = nz*ux - nx*uz, vz = -ny*ux;
  float cr = cosf(roll), sr = sinf(roll), Ux = ux*cr + vx*sr, Uy = vy*sr, Uz = uz*cr + vz*sr, Vx = -ux*sr + vx*cr, Vy = vy*cr, Vz = -uz*sr + vz*cr, hw = w*.5f, B = 2.f + pal + t*.99f;
  tv(x-(Ux+Vx)*hw, y-(Uy+Vy)*hw, z-(Uz+Vz)*hw, 0, 0, B); tv(x+(Ux-Vx)*hw, y+(Uy-Vy)*hw, z+(Uz-Vz)*hw, 1, 0, B); tv(x+(Ux+Vx)*hw, y+(Uy+Vy)*hw, z+(Uz+Vz)*hw, 1, 1, B);
  tv(x-(Ux+Vx)*hw, y-(Uy+Vy)*hw, z-(Uz+Vz)*hw, 0, 0, B); tv(x+(Ux+Vx)*hw, y+(Uy+Vy)*hw, z+(Uz+Vz)*hw, 1, 1, B); tv(x+(Vx-Ux)*hw, y+(Vy-Uy)*hw, z+(Vz-Uz)*hw, 0, 1, B);
}
static void shell(float cx, float cy, float cz, float rx, float ry, float rz, int n, int seed, int pal, float cw) {   // leaves growing on an ellipsoid dome
  for (int i = 0; i < n; i++) {
    float a = vn(seed + i*3, i)*6.2832f, e = vn(i, seed + i*7)*1.45f - .05f, ca = cosf(e), dx = cosf(a)*ca, dy = sinf(e), dz = sinf(a)*ca, k = .8f + .2f*vn(seed*5 + i, i*3);
    float nx = dx/rx, ny = dy/ry, nz = dz/rz, nl = sqrtf(nx*nx + ny*ny + nz*nz);
    cardN(cx + dx*rx*k, cy + dy*ry*k, cz + dz*rz*k, nx/nl, ny/nl, nz/nl, cw*(.8f + .5f*vn(i, seed+5)), vn(i*3, seed)*6.28f, pal, fminf(.99f, .25f + .5f*dy + .3f*vn(i, seed+3)));
  }
}
static void card(float x, float y, float z, float w, float a, float tilt, int pal, float t) {   // leaf card (alpha-tested in shader)
  float ca = cosf(a), sa = sinf(a), st = sinf(tilt), ct = cosf(tilt), hw = w*.5f, B = 2.f + pal + t*.99f;
  float ux = ca*hw, uz = sa*hw, vx = -sa*st*hw, vy = ct*hw, vz = ca*st*hw;
  tv(x-ux-vx, y-vy, z-uz-vz, 0, 0, B); tv(x+ux-vx, y-vy, z+uz-vz, 1, 0, B); tv(x+ux+vx, y+vy, z+uz+vz, 1, 1, B);
  tv(x-ux-vx, y-vy, z-uz-vz, 0, 0, B); tv(x+ux+vx, y+vy, z+uz+vz, 1, 1, B); tv(x-ux+vx, y+vy, z-uz+vz, 0, 1, B);
}
static void crown(float cx, float cy, float cz, float R, int n, int seed, int pal, float cs) {
  for (int i = 0; i < n; i++) {
    float a = vn(seed + i*3, i)*6.2832f, e = (vn(i, seed + i*7) - .25f)*1.9f, rr = R*(.4f + .6f*vn(seed*5 + i, i*3));
    card(cx + cosf(a)*cosf(e)*rr, cy + sinf(e)*rr*.75f, cz + sinf(a)*cosf(e)*rr, R*.9f*cs, vn(i*2, seed)*6.2832f, .3f + 2.4f*vn(seed, i*5), pal, fminf(.99f, .3f + .3f*sinf(e) + .4f*vn(i, seed+3)));
  }
}
static void pine(float x, float y, float z, float sc, int lod, int pal) {   // lod 0 near .. 2 far (fewer, bigger cards keep the silhouette)
  seg(x, y-.3f, z, x, y+9.f*sc, z, .55f*sc, .1f*sc, .3f, .2f, .11f);
  int nt = lod == 0 ? 8 : lod == 1 ? 6 : 4, nb = lod == 0 ? 6 : lod == 1 ? 5 : 3; float cw = lod == 0 ? 1.25f : lod == 1 ? 1.7f : 2.6f;
  for (int t = 0; t < nt; t++) {
    float f = t/(float)(nt-1), ty = y + (1.4f + f*7.2f)*sc, len = (3.2f - 2.5f*f)*sc;
    for (int q = 0; q < nb; q++) {
      float a = q*6.2832f/nb + t*.9f + vn(t*7+q, (int)(x*3))*.5f;
      card(x + cosf(a)*len*.5f, ty - .35f*len*(1.f - .5f*f), z + sinf(a)*len*.5f, len*cw, a - 1.5708f, 2.f + .35f*vn(q, t), pal, .2f + .6f*f + .15f*vn(q, t+3));
    }
  }
  card(x, y + 9.f*sc, z, 1.6f*sc, 0, 0, pal, .8f);
}
static void oak(float x, float y, float z, float sc, int seed, int lod) {
  seg(x, y-.3f, z, x, y+1.2f*sc, z, .55f*sc, .38f*sc, .4f, .34f, .27f); seg(x, y+1.2f*sc, z, x, y+3.6f*sc, z, .38f*sc, .24f*sc, .42f, .36f, .28f);
  int n = lod == 0 ? 22 : lod == 1 ? 11 : 5; float cs = lod == 0 ? 1.f : lod == 1 ? 1.45f : 2.1f;
  for (int q = 0; q < 4; q++) {
    float a = q*1.57f + vn(seed, q)*.9f, bx = x + cosf(a)*2.2f*sc, bz = z + sinf(a)*2.2f*sc, by = y + (4.6f + 1.2f*vn(q, seed))*sc;
    if (lod < 2) seg(x, y+3.2f*sc, z, bx, by, bz, .2f*sc, .07f*sc, .4f, .33f, .26f);
    crown(bx, by + .8f*sc, bz, 2.3f*sc, n, seed + q*5, 0, cs);
  }
  crown(x, y + 5.8f*sc, z, 2.4f*sc, n, seed + 41, 0, cs);
}
static void bush(float x, float y, float z, int seed, int lod, int pal) {   // pal 2 green, 4 snow, 5 dry desert; core blob + surface-aligned leaves
  float S = 1.f + .35f*vn(seed, 5), cw = lod == 0 ? 1.f : lod == 1 ? 1.5f : 2.1f; int n = lod == 0 ? 48 : lod == 1 ? 24 : 12, cm = pal == 2 ? 5 : pal == 4 ? 6 : 7, cl = lod == 2 ? 0 : 1;
  if (pal == 5) n = n*2/3;
  if (seed & 1) { blob(x, y, z, 1.5f*S, .8f*S, 1.3f*S, seed, cm, .15f, cl); shell(x, y + .1f*S, z, 2.1f*S, 1.15f*S, 1.8f*S, n, seed, pal, cw*.8f*S); }
  else { blob(x, y, z, .9f*S, 1.5f*S, .9f*S, seed, cm, .15f, cl); shell(x, y + .1f*S, z, 1.3f*S, 2.1f*S, 1.3f*S, n, seed, pal, cw*.75f*S); }
}
static void cactus(float x, float y, float z, int seed, int lod) {   // saguaro / barrel / paddle cactus
  int k = ((seed % 3) + 3) % 3; float s = 1.f + .4f*vn(seed, 3);
  if (k == 0) {
    seg(x, y-.2f, z, x, y + 4.4f*s, z, .42f*s, .32f*s, .2f, .42f, .15f);
    for (int q = 0; q < 2; q++) { float a = q*3.14f + vn(seed, q)*.6f, ax = x + cosf(a)*1.1f*s, az = z + sinf(a)*1.1f*s, y0 = y + (1.6f + q*.8f)*s;
      seg(x + cosf(a)*.2f*s, y0, z + sinf(a)*.2f*s, ax, y0 + .9f*s, az, .26f*s, .22f*s, .2f, .42f, .15f); seg(ax, y0 + .8f*s, az, ax, y0 + 2.1f*s, az, .22f*s, .18f*s, .2f, .42f, .15f); }
  } else if (k == 1) blob(x, y, z, .85f*s, .75f*s, .85f*s, seed, 4, .1f, lod == 0 ? 2 : 1);
  else for (int q = 0; q < 4; q++) { float a = q*1.57f + vn(seed, q), qx = x + cosf(a)*.5f*s, qz = z + sinf(a)*.5f*s; blob(qx, y + .3f*q*s, qz, (q & 1 ? .22f : .8f)*s, .62f*s, (q & 1 ? .8f : .22f)*s, seed + q, 4, .15f, 1); }
}
static void water() {
  auto W3 = [&](int i, int j) { vb.push_back({TX0 + i*5.f, -.2f, TZ0 + j*5.f, -.2f - TH[i][j], 0, 0}); };
  for (int i = 0; i < 42; i++) for (int j = 0; j < 42; j++) {
    if (fminf(fminf(TH[i][j], TH[i+1][j]), fminf(TH[i][j+1], TH[i+1][j+1])) > .5f) continue;
    W3(i,j); W3(i+1,j); W3(i+1,j+1); W3(i,j); W3(i+1,j+1); W3(i,j+1);
  }
}
static void ore(float x, float y, float z, int t, int seed, int lod) {   // 0 stone, 1 metal, 2 sulfur: one fused boulder (+8 = snow-capped)
  if (biomeAt(x, z) > 1.5f) t += 8;
  blob(x, y, z, 1.9f, 1.5f, 1.7f, seed, t, .42f, lod == 0 ? 2 : 1);
}
static void lightMat(float* LM, float lx, float ly, float lz) {   // orthographic sun view around the player
  ly = fmaxf(ly, .25f); float ll = sqrtf(lx*lx + ly*ly + lz*lz); lx /= ll; ly /= ll; lz /= ll;
  float ex = px + lx*100, ey = py + ly*100, ez = pz + lz*100, fx = -lx, fy = -ly, fz = -lz, ux = 0, uy = 1, uz = 0;
  if (ly > .95f) { uy = 0; uz = 1; }
  float rx = fy*uz - fz*uy, ry = fz*ux - fx*uz, rz = fx*uy - fy*ux, rl = sqrtf(rx*rx + ry*ry + rz*rz); rx /= rl; ry /= rl; rz /= rl;
  float vx = ry*fz - rz*fy, vy = rz*fx - rx*fz, vz = rx*fy - ry*fx;
  float V4[16] = {rx, vx, -fx, 0,  ry, vy, -fy, 0,  rz, vz, -fz, 0,  -(rx*ex + ry*ey + rz*ez), -(vx*ex + vy*ey + vz*ez), fx*ex + fy*ey + fz*ez, 1};
  const float S = 34.f, nn = 20.f, ff = 180.f;
  float Pl[16] = {1/S,0,0,0, 0,1/S,0,0, 0,0,-2/(ff-nn),0, 0,0,-(ff+nn)/(ff-nn),1};
  mul(LM, Pl, V4);
}
static void shadowDraw(const float* LM) {
  glBindFramebuffer(GL_FRAMEBUFFER, shFbo); glViewport(0, 0, 2048, 2048);
  glClearColor(1, 1, 1, 1); glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT); glEnable(GL_DEPTH_TEST);
  glUseProgram(shProg); glUniformMatrix4fv(uShLm, 1, 0, LM);
  glBufferData(GL_ARRAY_BUFFER, sv.size()*sizeof(V), sv.data(), GL_STREAM_DRAW); glDrawArrays(GL_TRIANGLES, 0, (GLsizei)sv.size());
  glBindFramebuffer(GL_FRAMEBUFFER, 0); glViewport(0, 0, W, H); glUseProgram(prog);
}
static void quadUV(float x, float y, float z, float ux, float uy, float uz, float vx, float vy, float vz, float e) {
  tv(x-ux-vx, y-uy-vy, z-uz-vz, 0, 0, e); tv(x+ux-vx, y+uy-vy, z+uz-vz, 1, 0, e); tv(x+ux+vx, y+uy+vy, z+uz+vz, 1, 1, e);
  tv(x-ux-vx, y-uy-vy, z-uz-vz, 0, 0, e); tv(x+ux+vx, y+uy+vy, z+uz+vz, 1, 1, e); tv(x-ux+vx, y+uy+vy, z-uz+vz, 0, 1, e);
}
static void pebbles() {   // stones on the beach
  int cx = (int)floorf(px/2), cz = (int)floorf(pz/2);
  for (int i = -16; i <= 16; i++) for (int j = -16; j <= 16; j++) {
    int gx = cx + i, gz = cz + j; if (vn(gx*5+3, gz*7+1) < .86f) continue;
    float x = (gx + vn(gx, gz+2))*2, z = (gz + vn(gz+5, gx))*2, h = hgt(x, z);
    if (h < -.3f || h > 1.4f || (x-px)*(x-px) + (z-pz)*(z-pz) > 1000) continue;
    float s = .12f + .3f*vn(gx+9, gz), g = .22f + .25f*vn(gz, gx+4);
    blob(x, h - .03f, z, s, s*.55f, s*.8f, gx^gz, 3, .3f, 0);
  }
}
static void scene() {
  pebbles();
  for (auto& o : nodes) {
    if (o.hp <= 0) continue; float dx = o.x-px, dz = o.z-pz; if (dx*dx + dz*dz > 16900) continue;
    float d2 = dx*dx + dz*dz; int lod = d2 < 1500 ? 0 : d2 < 5000 ? 1 : 2, sd0 = (int)(o.x*7) ^ (int)(o.z*13), ty = o.type;
    if ((ty == 2 || ty == 8 || ty == 10) && d2 > 8100) continue;
    if (ty == 0) pine(o.x, o.y, o.z, .9f + .6f*vn(sd0, 1), lod, 1);
    else if (ty == 7) pine(o.x, o.y, o.z, .9f + .6f*vn(sd0, 1), lod, 3);
    else if (ty == 1 || ty == 5 || ty == 6) ore(o.x, o.y, o.z, ty == 1 ? 0 : ty - 4, sd0, lod);
    else if (ty == 2) bush(o.x, o.y, o.z, sd0, lod, 2);
    else if (ty == 8) bush(o.x, o.y, o.z, sd0, lod, 4);
    else if (ty == 10) bush(o.x, o.y, o.z, sd0, lod, 5);
    else if (ty == 4) oak(o.x, o.y, o.z, .9f + .5f*vn(sd0, 2), sd0, lod);
    else if (ty == 9) cactus(o.x, o.y, o.z, sd0, lod);
    else palm(o.x, o.y, o.z);
  }
  for (auto& b : blks) { if (b.wall) box(b.x,b.y-1.2f,b.z,b.w,4.2f,b.d,.6f,.42f,.22f); else box(b.x,b.y-1.2f,b.z,b.w,1.5f,b.d,.5f,.35f,.18f); }
}
static void render(float dt) {
  float k = fminf(fmaxf(sinf(tod*6.2832f)*2 + .5f, .06f), 1.f), dyf = fminf(fmaxf((k - .06f)/.94f, 0.f), 1.f), wm = fmaxf(0.f, 1.f - fabsf(sinf(tod*6.2832f))*6.f), sky[3] = {.07f + .59f*dyf + .3f*wm, .1f + .7f*dyf + .12f*wm, .2f + .75f*dyf - .05f*wm}, top[3] = {.01f + .19f*dyf, .02f + .4f*dyf, .07f + .78f*dyf};
  glViewport(0, 0, W, H); glClearColor(sky[0]*.7f, sky[1]*.82f, sky[2]*.98f, 1); glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT); glEnable(GL_DEPTH_TEST);
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
  vb.clear(); scene(); sv.swap(vb);
  float th2 = tod*6.2832f, L0[3] = {.62f*cosf(th2), sinf(th2), -.78f*cosf(th2)}, LM[16];
  bool shOn = shOk && L0[1] > .08f;
  if (shOn) {
    lightMat(LM, L0[0], L0[1], L0[2]); shadowDraw(LM);
    glUniformMatrix4fv(uLm, 1, 0, LM); glUniform1i(uSm, 1); glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, shTex); glActiveTexture(GL_TEXTURE0);
  }
  glUniform1f(uSs, shOn ? 1.f : 0.f);
  terrain(); flush(A, sky, lt);
  txOn = 8; grass(tt); flush(A, sky, lt);
  txOn = 2; water(); glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA); flush(A, sky, lt); glDisable(GL_BLEND);
  txOn = 0;
  vb.swap(sv);
  if (mode) { Blk g = ghost(); bool ok = wood >= (mode == 1 ? 30 : 20); float gr = ok ? .3f : .9f, gg = ok ? .9f : .25f, gb = ok ? .3f : .2f; if (g.wall) box(g.x,g.y-1.2f,g.z,g.w,4.2f,g.d,gr,gg,gb); else box(g.x,g.y-1.2f,g.z,g.w,1.52f,g.d,gr,gg,gb); }
  flush(A, sky, .25f + .75f*k);
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
    snprintf(b, sizeof b, "%d|%d|%d|%d|%d|%d|%d|%s", (int)hp, (int)food, (int)thirst, wood, stone, metal, sulfur, msgT > 0 ? msg : "");
    hud(b);
  }
}
static void genChunk(int cx, int cz) {
  for (int i = 0; i < 60; i++) {
    float x = (cx + vn(cx*17 + i, cz*31 + 5))*64.f, z = (cz + vn(cx*23 + 7, cz*19 + i))*64.f, h = hgt(x, z), r = vn(cx*13 + i*3, cz*7 + i), bio = biomeAt(x, z);
    if (hypotf(x, z) < 6.f || h < .8f) continue;
    int bi = bio < .5f ? 0 : bio < 1.5f ? 1 : 2, ty;
    if (bi == 0) ty = h < 1.8f ? (r < .6f ? 2 : -1) : r < .25f ? 0 : r < .6f ? 4 : r < .69f ? 1 : r < .75f ? 5 : r < .8f ? 6 : 2;
    else if (bi == 1) ty = r < .38f ? 9 : r < .5f ? 3 : r < .68f ? 10 : r < .78f ? 1 : r < .86f ? 5 : r < .92f ? 6 : -1;
    else ty = r < .5f ? 7 : r < .68f ? 8 : r < .78f ? 1 : r < .85f ? 5 : r < .9f ? 6 : -1;
    if (ty < 0) continue;
    nodes.push_back({x, z, 0, ty, ty == 0 || ty == 7 ? 5 : ty == 1 || ty == 5 || ty == 6 ? 7 : ty == 3 ? 4 : ty == 4 ? 5 : ty == 9 ? 3 : 1, h});
  }
}
static void ensureChunks() {
  static int lx = -99999, lz = -99999; static vector<long long> done;
  int cx = (int)floorf(px/64), cz = (int)floorf(pz/64); if (cx == lx && cz == lz) return; lx = cx; lz = cz;
  for (int i = -3; i <= 3; i++) for (int j = -3; j <= 3; j++) {
    long long key = (long long)(cx + i)*100003LL + (cz + j); if (find(done.begin(), done.end(), key) != done.end()) continue;
    done.push_back(key); genChunk(cx + i, cz + j);
  }
}
static void loop() {
  double now = emscripten_get_now(); float dt = fminf((float)((now-last)/1000), .05f); last = now;
  double cw, ch; emscripten_get_element_css_size("#canvas", &cw, &ch);
  K = fminf((float)emscripten_get_device_pixel_ratio(), 1.5f);
  int nw = (int)(cw*K), nh = (int)(ch*K);
  if (nw != W || nh != H) { W = nw; H = nh; emscripten_set_canvas_element_size("#canvas", W, H); }
  if (W < 2 || H < 2) return;
  ensureChunks(); update(dt); render(dt);
}
int main() {
  EmscriptenWebGLContextAttributes a; emscripten_webgl_init_context_attributes(&a); a.majorVersion = 1; a.antialias = 0;
  auto gctx = emscripten_webgl_create_context("#canvas", &a); emscripten_webgl_make_context_current(gctx); emscripten_webgl_enable_extension(gctx, "OES_standard_derivatives");
  GLuint p = glCreateProgram(); glAttachShader(p, sh(GL_VERTEX_SHADER, VS)); glAttachShader(p, sh(GL_FRAGMENT_SHADER, FS));
  glBindAttribLocation(p, 0, "p"); glBindAttribLocation(p, 1, "c"); glLinkProgram(p); glUseProgram(p); prog = p;
  uM = glGetUniformLocation(p, "m"); uFog = glGetUniformLocation(p, "fog"); uLit = glGetUniformLocation(p, "lit"); uTx = glGetUniformLocation(p, "tx"); uSd = glGetUniformLocation(p, "sd"); uEy = glGetUniformLocation(p, "ey"); uTm = glGetUniformLocation(p, "tm"); uLm = glGetUniformLocation(p, "lm"); uSm = glGetUniformLocation(p, "sm"); uSs = glGetUniformLocation(p, "ss");
  GLuint bf; glGenBuffers(1, &bf); glBindBuffer(GL_ARRAY_BUFFER, bf);
  glEnableVertexAttribArray(0); glEnableVertexAttribArray(1);
  glVertexAttribPointer(0, 3, GL_FLOAT, 0, 24, (void*)0); glVertexAttribPointer(1, 3, GL_FLOAT, 0, 24, (void*)12);
  {
    const char* svs = "attribute vec3 p;attribute vec3 c;uniform mat4 lm;varying float z;varying vec3 vc;void main(){gl_Position=lm*vec4(p,1.);z=gl_Position.z*.5+.5;vc=c;}";
    const char* sfs = "precision highp float;varying float z;varying vec3 vc;"
      "float h(vec2 p){vec3 q=fract(vec3(p.xyx)*.1031);q+=dot(q,q.yzx+33.33);return fract((q.x+q.y)*q.z);}"
      "float lf(vec2 q){vec2 i=floor(q),f=fract(q)-.5;float a=h(i)*6.283;f+=(vec2(h(i+7.),h(i+13.))-.5)*.3;vec2 r=vec2(cos(a)*f.x+sin(a)*f.y,-sin(a)*f.x+cos(a)*f.y);return 1.-step(1.,length(r/vec2(.52,.24)));}float lfm(vec2 u){return max(lf(u*5.),lf(u*6.+3.1))*step(length(u-.5),.5);}"
      "void main(){if(vc.b>1.5&&lfm(vc.xy)<.5)discard;vec4 e=fract(z*vec4(1.,255.,65025.,16581375.));e-=e.yzww*vec4(1./255.,1./255.,1./255.,0.);gl_FragColor=e;}";
    shProg = glCreateProgram(); glAttachShader(shProg, sh(GL_VERTEX_SHADER, svs)); glAttachShader(shProg, sh(GL_FRAGMENT_SHADER, sfs));
    glBindAttribLocation(shProg, 0, "p"); glBindAttribLocation(shProg, 1, "c"); glLinkProgram(shProg); uShLm = glGetUniformLocation(shProg, "lm");
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
  py = hgt(0, 0);
  emscripten_set_main_loop(loop, 0, 1);
}
