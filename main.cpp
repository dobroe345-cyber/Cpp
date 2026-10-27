// Mini-Rust: survival sandbox. C++ -> WebAssembly (Emscripten), touch controls for iPhone.
#include <emscripten.h>
#include <emscripten/html5.h>
#include <GLES2/gl2.h>
#include <cmath>
#include <cstdio>
#include <vector>
using namespace std;

struct V { float x, y, z, r, g, b; };
struct Node { float x, z, t; int type, hp; float y; };      // 0 tree, 1 rock, 2 berry bush
struct Blk { float x, z, w, d; bool wall; float y; };       // building piece
struct Fin { int id; float x, y, bx, by; };        // finger

static vector<V> vb; static vector<Node> nodes; static vector<Blk> blks;
static GLint uM, uFog, uLit, uTx; static float txOn = 1; static int W = 0, H = 0; static float K = 1;
static float px = 0, py = 0, pz = 0, vy = 0, yaw = 0, pitch = 0, hp = 100, food = 100;
static float tod = .1f, cd = 0, hudT = 0, msgT = 0; static int wood = 60, stone = 0, mode = 0;
static char msg[48] = ""; static Fin st = {-1}, lk = {-1};
static double last = 0; static unsigned rs = 12345;

EM_JS(int, gk, (int i), { var v = window.G[i] | 0; if (i == 1) window.G[1] = 0; return v; });
EM_JS(float, gf, (int i), { var v = window.G[i] || 0; if (i > 4) window.G[i] = 0; return v; });
EM_JS(void, hud, (const char* s), { document.getElementById('hud').textContent = UTF8ToString(s); });

static const char* VS = "attribute vec3 p;attribute vec3 c;uniform mat4 m;varying vec3 vc;varying float d;varying vec2 wp;"
  "void main(){gl_Position=m*vec4(p,1.);vc=c;d=gl_Position.w;wp=p.xz;}";
static const char* FS = "precision highp float;varying vec3 vc;varying float d;varying vec2 wp;uniform vec3 fog;uniform float lit;uniform float tx;"
  "float h(vec2 p){return fract(sin(dot(p,vec2(127.1,311.7)))*43758.5453);}"
  "float n(vec2 p){vec2 i=floor(p),f=fract(p);f=f*f*(3.-2.*f);return mix(mix(h(i),h(i+vec2(1.,0.)),f.x),mix(h(i+vec2(0.,1.)),h(i+vec2(1.,1.)),f.x),f.y);}"
  "void main(){vec3 c=vc*lit;if(tx>.5)c*=.72+.5*(n(wp*.8)*.5+n(wp*3.)*.3+n(wp*11.)*.2);gl_FragColor=vec4(mix(c,fog,clamp(d/110.,0.,1.)),1.);}";

static GLuint sh(GLenum t, const char* s) { GLuint o = glCreateShader(t); glShaderSource(o, 1, &s, 0); glCompileShader(o); return o; }
static float vn(int x, int z) { unsigned h = x*374761393u + z*668265263u; h = (h ^ (h >> 13))*1274126177u; return ((h ^ (h >> 16)) & 0xffff)/65535.f; }
static float pn(float x, float z) {
  int ix = (int)floorf(x), iz = (int)floorf(z); float fx = x-ix, fz = z-iz; fx = fx*fx*(3-2*fx); fz = fz*fz*(3-2*fz);
  return (vn(ix,iz)*(1-fx) + vn(ix+1,iz)*fx)*(1-fz) + (vn(ix,iz+1)*(1-fx) + vn(ix+1,iz+1)*fx)*fz;
}
static float hgt(float x, float z) {   // island: hills inland, beach, sea
  float e = pn(x*.015f, z*.015f)*.6f + pn(x*.04f+9, z*.04f)*.3f + pn(x*.13f, z*.13f+4)*.1f, m = 1 - hypotf(x, z)/100.f;
  return (e - .3f)*16*fminf(fmaxf(m*3, 0.f), 1.f) + m*6 - 1;
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
    if (n.hp <= 0) continue;
    float dx = n.x-px, dz = n.z-pz, d = sqrtf(dx*dx + dz*dz);
    if (d < bd && dx*fx + dz*fz > .6f*d) { bd = d; best = &n; }
  }
  if (!best) { say("Мимо"); return; }
  if (--best->hp <= 0) best->t = 60;
  if (best->type == 0 || best->type == 3) { wood += 10; say("+10 дерева"); }
  else if (best->type == 1) { stone += 8; say("+8 камня"); }
  else { food = fminf(100, food + 20); say("+20 еды"); }
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
  food -= dt*.4f;
  if (food <= 0) { food = 0; hp -= dt*2; } else if (food > 50 && hp < 100) hp = fminf(100, hp + dt*2);
  if (hp <= 0) { hp = 100; food = 100; wood = stone = 0; px = pz = 0; say("Ты умер. Ресурсы потеряны"); }
  float sx = gf(3), sy = gf(4);
  yaw -= gf(5)*.006f; pitch = fminf(fmaxf(pitch - gf(6)*.006f, -1.4f), 1.4f);
  float fx = -sinf(yaw), fz = -cosf(yaw), rx = cosf(yaw), rz = -sinf(yaw), sp = 5.f*(hypotf(sx, sy) > .95f ? 1.4f : 1.f);
  float ox = px, oz = pz; px += (fx*-sy + rx*sx)*sp*dt; pz += (fz*-sy + rz*sx)*sp*dt;
  for (auto& n : nodes) {
    if (n.hp > 0) { float r = n.type == 0 ? .9f : n.type == 1 ? 1.2f : .5f; push(px, pz, n.x-r, n.z-r, n.x+r, n.z+r); }
    else if ((n.t -= dt) <= 0) n.hp = n.type == 0 ? 5 : n.type == 1 ? 6 : n.type == 3 ? 4 : 1;
  }
  for (auto& b : blks) if (b.wall) push(px, pz, b.x-b.w/2, b.z-b.d/2, b.x+b.w/2, b.z+b.d/2);
  if (hgt(px, pz) < -.5f) { px = ox; pz = oz; }
  float gy = hgt(px, pz); for (auto& b : blks) if (!b.wall && fabsf(px-b.x) < 2 && fabsf(pz-b.z) < 2) gy = fmaxf(gy, b.y + .3f);
  if (gk(1) && py <= gy + .01f) vy = 7;
  vy -= 20*dt; py += vy*dt; if (py < gy + .25f && vy <= 0) { py = gy; vy = 0; }
  mode = gk(2);
  if (gk(0) && cd <= 0) act();
}
static void terrain() {
  const int N = 21; const float C = 5.f; int gx0 = (int)floorf(px/C), gz0 = (int)floorf(pz/C);
  static float Y[2*N+1][2*N+1], Q[2*N+1][2*N+1][3];
  for (int i = 0; i <= 2*N; i++) for (int j = 0; j <= 2*N; j++) {
    float x = (gx0-N+i)*C, z = (gz0-N+j)*C, h = hgt(x, z);
    float dx = hgt(x-2,z) - hgt(x+2,z), dz = hgt(x,z-2) - hgt(x,z+2), l = sqrtf(dx*dx + 16 + dz*dz), ny = 4/l;
    float lit = .5f + .5f*fmaxf(0.f, (dx*.4f + 3.f - dz*.5f)/l), v = pn(x*.25f, z*.25f), r, g, b;
    if (h < -.2f) { float t = fminf(1.f, (-.2f-h)/4); r = .25f-.2f*t; g = .62f-.4f*t; b = .62f-.2f*t; lit = 1; h = -.2f; }
    else if (h < 1.3f) { r = .78f; g = .7f; b = .48f; }
    else if (ny < .8f) { r = .46f; g = .43f; b = .4f; }
    else { r = .2f + .1f*v; g = .45f + .12f*v; b = .15f; }
    Y[i][j] = h; Q[i][j][0] = r*lit; Q[i][j][1] = g*lit; Q[i][j][2] = b*lit;
  }
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
static void grass(float tt) {   // swaying tufts around the player
  int cx = (int)floorf(px/2), cz = (int)floorf(pz/2);
  for (int i = -14; i <= 14; i++) for (int j = -14; j <= 14; j++) {
    int gx = cx + i, gz = cz + j; if (vn(gx*3+1, gz*5+2) < .35f) continue;
    float x = gx*2 + vn(gx, gz)*2, z = gz*2 + vn(gz+7, gx)*2, dx = x-px, dz = z-pz;
    if (dx*dx + dz*dz > 784) continue;
    float h = hgt(x, z); if (h < 1.6f) continue;
    float s = .6f + .6f*vn(gx+3, gz+9), w = sinf(tt*2 + x*.7f + z*.5f)*.15f*s;
    for (int q = 0; q < 3; q++) {
      float a = q*1.047f + vn(gx, gz+q)*.5f, bx = cosf(a)*.15f, bz = sinf(a)*.15f;
      vb.push_back({x-bx, h, z-bz, .1f, .28f, .07f}); vb.push_back({x+bx, h, z+bz, .1f, .28f, .07f});
      vb.push_back({x + w + bx*.4f, h + .75f*s, z + w*.5f + bz*.4f, .4f, .66f, .2f});
    }
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
static void rock(float x, float y, float z, float s, float r, float g, float b) {
  float P[6][3] = {{x+s,y+.4f*s,z},{x,y+.4f*s,z+s*.9f},{x-s,y+.4f*s,z},{x,y+.4f*s,z-s*.9f},{x,y+s*1.1f,z},{x,y-.3f*s,z}};
  for (int i = 0; i < 6; i++) { float j = (vn((int)(x*13) + i*31, (int)(z*13) + i*17) - .5f)*.7f*s; P[i][0] += j; P[i][2] -= j; if (i < 5) P[i][1] += j*.5f; }
  static const int F[8][3] = {{0,1,4},{1,2,4},{2,3,4},{3,0,4},{1,0,5},{2,1,5},{3,2,5},{0,3,5}};
  for (int f = 0; f < 8; f++) { float l = f > 3 ? .45f : .6f + .1f*f;
    for (int k = 0; k < 3; k++) vb.push_back({P[F[f][k]][0], P[F[f][k]][1], P[F[f][k]][2], r*l, g*l, b*l}); }
}
static void render(float dt) {
  float k = fminf(fmaxf(sinf(tod*6.2832f)*2 + .5f, .06f), 1.f), sky[3] = {.66f*k, .8f*k, .95f*k}, top[3] = {.2f*k, .42f*k, .85f*k};
  glViewport(0, 0, W, H); glClearColor(sky[0]*.7f, sky[1]*.82f, sky[2]*.98f, 1); glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT); glEnable(GL_DEPTH_TEST);
  float a = (float)W/H, f = 1/tanf(.55f), n = .1f, fa = 120.f;
  float P[16] = {f/a,0,0,0, 0,f,0,0, 0,0,(fa+n)/(n-fa),-1, 0,0,2*fa*n/(n-fa),0};
  float c = cosf(-yaw), s = sinf(-yaw), c2 = cosf(-pitch), s2 = sinf(-pitch);
  float Ry[16] = {c,0,-s,0, 0,1,0,0, s,0,c,0, 0,0,0,1}, Rx[16] = {1,0,0,0, 0,c2,s2,0, 0,-s2,c2,0, 0,0,0,1};
  float T[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, -px,-(py+1.7f),-pz,1}, A[16], B[16];
  mul(A, Rx, Ry); mul(B, A, T); mul(A, P, B);
  static float tt = 0; tt += dt;
  {
    float I0[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    float yh = fminf(fmaxf(-f*tanf(pitch), -2.5f), 2.5f), Y4[4] = {-3.f, yh, fminf(yh + 1.3f, 3.f), 3.f}; const float* C4[4] = {sky, sky, top, top};
    for (int q = 0; q < 3; q++) {
      auto Q = [&](float x, int w) { vb.push_back({x, Y4[w], 0, C4[w][0], C4[w][1], C4[w][2]}); };
      Q(-1, q); Q(1, q); Q(1, q+1); Q(-1, q); Q(1, q+1); Q(-1, q+1);
    }
    glDisable(GL_DEPTH_TEST); txOn = 0; flush(I0, sky, 1);
    float th = tod*6.2832f, sn = sinf(th), cs = cosf(th), sd[3] = {.62f*cs, sn, -.78f*cs};
    if (sn > -.06f) {
      float sx0 = px + sd[0]*100, sy0 = py + 1.7f + sn*100, sz0 = pz + sd[2]*100;
      const float R3[3] = {22, 13, 7.5f}, M3[3] = {.22f, .55f, 1.f};
      for (int q = 0; q < 3; q++) {
        float col[3] = {sky[0] + (1.f-sky[0])*M3[q], sky[1] + (.97f-sky[1])*M3[q], sky[2] + (.8f-sky[2])*M3[q]};
        auto S = [&](float a2) { float c3 = cosf(a2), s3 = sinf(a2); vb.push_back({sx0 + R3[q]*(c3*.78f - s3*.62f*sn), sy0 + R3[q]*s3*.993f*cs, sz0 + R3[q]*(c3*.62f + s3*.78f*sn), col[0], col[1], col[2]}); };
        for (int i = 0; i < 16; i++) { vb.push_back({sx0, sy0, sz0, col[0], col[1], col[2]}); S(i*.3927f); S((i+1)*.3927f); }
        flush(A, col, 1);
      }
    }
    float kk = .25f + .75f*k, cc[3] = {.93f*kk, .95f*kk, kk};
    for (int i = 0; i < 20; i++) {
      float x = fmodf((vn(i, 1) - .5f)*240 + tt*2.5f - px + 1000.f, 240.f) - 120.f, z = fmodf((vn(i, 2) - .5f)*240 - pz + 1000.f, 240.f) - 120.f;
      if (x*x + z*z > 6400) continue;
      for (int j = 0; j < 4; j++) box(px + x + (j-1.5f)*7, 42 + vn(i, 3)*22 + (j&1)*1.5f, pz + z + ((j*5)%3-1)*3.f, 12 + vn(i, j)*8, 3.5f, 8 + vn(j, i)*5, cc[0], cc[1], cc[2]);
    }
    flush(A, cc, 1);
    glEnable(GL_DEPTH_TEST); txOn = 1;
  }
  terrain(); grass(tt);
  for (auto& o : nodes) {
    if (o.hp <= 0) continue; float dx = o.x-px, dz = o.z-pz; if (dx*dx + dz*dz > 8100) continue;
    if (o.type == 0) {
      float sc = .8f + .6f*vn((int)(o.x*9), (int)(o.z*9)); int nc = dx*dx + dz*dz < 2500 ? 5 : 3;
      seg(o.x, o.y-.3f, o.z, o.x, o.y+6.f*sc, o.z, .55f*sc, .22f*sc, .36f, .24f, .13f);
      for (int q = 0; q < nc; q++) { float f = nc == 5 ? q : q*1.7f; cone(o.x, o.y + (1.9f + f*1.2f)*sc, o.z, (2.3f - .4f*f)*sc*(.88f + .24f*vn(q, (int)o.x)), 2.5f*sc, .1f + .02f*q, .34f + .045f*q, .13f); }
    } else if (o.type == 1) {
      rock(o.x, o.y, o.z, 1.5f, .5f, .48f, .45f); rock(o.x + 1.1f, o.y, o.z + .6f, .9f, .56f, .54f, .5f); rock(o.x - .5f, o.y, o.z - 1.f, 1.f, .46f, .45f, .42f);
    } else if (o.type == 2) {
      rock(o.x, o.y, o.z, .75f, .1f, .34f, .1f); rock(o.x + .5f, o.y, o.z + .2f, .55f, .14f, .42f, .12f); rock(o.x - .4f, o.y, o.z + .3f, .5f, .12f, .38f, .1f);
      box(o.x + .15f, o.y + .75f, o.z, .16f, .16f, .16f, .85f, .1f, .15f); box(o.x - .3f, o.y + .6f, o.z + .35f, .16f, .16f, .16f, .85f, .1f, .15f);
    } else palm(o.x, o.y, o.z);
  }
  for (auto& b : blks) { if (b.wall) box(b.x,b.y-1.2f,b.z,b.w,4.2f,b.d,.6f,.42f,.22f); else box(b.x,b.y-1.2f,b.z,b.w,1.5f,b.d,.5f,.35f,.18f); }
  if (mode) { Blk g = ghost(); bool ok = wood >= (mode == 1 ? 30 : 20); float gr = ok ? .3f : .9f, gg = ok ? .9f : .25f, gb = ok ? .3f : .2f; if (g.wall) box(g.x,g.y-1.2f,g.z,g.w,4.2f,g.d,gr,gg,gb); else box(g.x,g.y-1.2f,g.z,g.w,1.52f,g.d,gr,gg,gb); }
  flush(A, sky, .25f + .75f*k);
  glDisable(GL_DEPTH_TEST);
  float I[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1}, u = fminf(W, H)/400.f;
  if (st.id >= 0) {
    float sx = st.x-st.bx, sy = st.y-st.by, l = hypotf(sx, sy), m = 60*u; if (l > m) { sx *= m/l; sy *= m/l; }
    disc(st.bx, st.by, m, .2f, .2f, .2f); disc(st.bx+sx, st.by+sy, 28*u, .8f, .8f, .8f);
  }
  txOn = 0; flush(I, sky, 1); txOn = 1;
  hudT -= dt;
  if (hudT <= 0) {
    hudT = .1f; char b[160];
    snprintf(b, sizeof b, "❤ %d  🍖 %d  🪵 %d  🪨 %d  %s  %s", (int)hp, (int)food, wood, stone,
      mode == 1 ? "🔨 пол (30🪵)" : mode == 2 ? "🔨 стена (20🪵)" : "", msgT > 0 ? msg : "");
    hud(b);
  }
}
static void loop() {
  double now = emscripten_get_now(); float dt = fminf((float)((now-last)/1000), .05f); last = now;
  double cw, ch; emscripten_get_element_css_size("#canvas", &cw, &ch);
  K = fminf((float)emscripten_get_device_pixel_ratio(), 2.f);
  int nw = (int)(cw*K), nh = (int)(ch*K);
  if (nw != W || nh != H) { W = nw; H = nh; emscripten_set_canvas_element_size("#canvas", W, H); }
  if (W < 2 || H < 2) return;
  update(dt); render(dt);
}
int main() {
  EmscriptenWebGLContextAttributes a; emscripten_webgl_init_context_attributes(&a); a.majorVersion = 1; a.antialias = 0;
  emscripten_webgl_make_context_current(emscripten_webgl_create_context("#canvas", &a));
  GLuint p = glCreateProgram(); glAttachShader(p, sh(GL_VERTEX_SHADER, VS)); glAttachShader(p, sh(GL_FRAGMENT_SHADER, FS));
  glBindAttribLocation(p, 0, "p"); glBindAttribLocation(p, 1, "c"); glLinkProgram(p); glUseProgram(p);
  uM = glGetUniformLocation(p, "m"); uFog = glGetUniformLocation(p, "fog"); uLit = glGetUniformLocation(p, "lit"); uTx = glGetUniformLocation(p, "tx");
  GLuint bf; glGenBuffers(1, &bf); glBindBuffer(GL_ARRAY_BUFFER, bf);
  glEnableVertexAttribArray(0); glEnableVertexAttribArray(1);
  glVertexAttribPointer(0, 3, GL_FLOAT, 0, 24, (void*)0); glVertexAttribPointer(1, 3, GL_FLOAT, 0, 24, (void*)12);
  for (int i = 0; i < 1200 && nodes.size() < 320; i++) {
    float x = (rnd()-.5f)*200, z = (rnd()-.5f)*200, h = hgt(x, z), r = rnd();
    if (hypotf(x, z) < 6 || h < .7f) continue;
    int ty = h < 2.f ? (r < .5f ? 3 : 2) : r < .55f ? 0 : r < .75f ? 1 : 2;
    nodes.push_back({x, z, 0, ty, ty == 0 ? 5 : ty == 1 ? 6 : ty == 3 ? 4 : 1, h});
  }
  say("Лево: ходьба · Право: камера"); msgT = 6;
  py = hgt(0, 0);
  emscripten_set_main_loop(loop, 0, 1);
}
