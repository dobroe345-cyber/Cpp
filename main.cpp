// Mini-Rust: survival sandbox. C++ -> WebAssembly (Emscripten), touch controls for iPhone.
#include <emscripten.h>
#include <emscripten/html5.h>
#include <GLES2/gl2.h>
#include <cmath>
#include <cstdio>
#include <vector>
using namespace std;

struct V { float x, y, z, r, g, b; };
struct Node { float x, z, t; int type, hp; };      // 0 tree, 1 rock, 2 berry bush
struct Blk { float x, z, w, d; bool wall; };       // building piece
struct Fin { int id; float x, y, bx, by; };        // finger

static vector<V> vb; static vector<Node> nodes; static vector<Blk> blks;
static GLint uM, uFog, uLit; static int W = 0, H = 0; static float K = 1;
static float px = 0, py = 0, pz = 0, vy = 0, yaw = 0, pitch = 0, hp = 100, food = 100;
static float tod = .1f, cd = 0, hudT = 0, msgT = 0; static int wood = 60, stone = 0, mode = 0;
static char msg[48] = ""; static Fin st = {-1}, lk = {-1};
static double last = 0; static unsigned rs = 12345;

EM_JS(int, gk, (int i), { var v = window.G[i] | 0; if (i == 1) window.G[1] = 0; return v; });
EM_JS(void, hud, (const char* s), { document.getElementById('hud').textContent = UTF8ToString(s); });

static const char* VS = "attribute vec3 p;attribute vec3 c;uniform mat4 m;varying vec3 vc;varying float d;"
  "void main(){gl_Position=m*vec4(p,1.);vc=c;d=gl_Position.w;}";
static const char* FS = "precision mediump float;varying vec3 vc;varying float d;uniform vec3 fog;uniform float lit;"
  "void main(){gl_FragColor=vec4(mix(vc*lit,fog,clamp(d/110.,0.,1.)),1.);}";

static GLuint sh(GLenum t, const char* s) { GLuint o = glCreateShader(t); glShaderSource(o, 1, &s, 0); glCompileShader(o); return o; }
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
  glUniformMatrix4fv(uM, 1, 0, m); glUniform3fv(uFog, 1, fog); glUniform1f(uLit, lit);
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
  if (best->type == 0) { wood += 10; say("+10 дерева"); }
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
  float sx = 0, sy = 0;
  if (st.id >= 0) { float m = 60*fminf(W, H)/400.f; sx = (st.x-st.bx)/m; sy = (st.y-st.by)/m; float l = hypotf(sx, sy); if (l > 1) { sx /= l; sy /= l; } }
  float fx = -sinf(yaw), fz = -cosf(yaw), rx = cosf(yaw), rz = -sinf(yaw), sp = 5.f*(hypotf(sx, sy) > .95f ? 1.4f : 1.f);
  px += (fx*-sy + rx*sx)*sp*dt; pz += (fz*-sy + rz*sx)*sp*dt;
  for (auto& n : nodes) {
    if (n.hp > 0) { float r = n.type == 0 ? .9f : n.type == 1 ? 1.2f : .5f; push(px, pz, n.x-r, n.z-r, n.x+r, n.z+r); }
    else if ((n.t -= dt) <= 0) n.hp = n.type == 0 ? 5 : n.type == 1 ? 6 : 1;
  }
  for (auto& b : blks) if (b.wall) push(px, pz, b.x-b.w/2, b.z-b.d/2, b.x+b.w/2, b.z+b.d/2);
  float gy = 0; for (auto& b : blks) if (!b.wall && fabsf(px-b.x) < 2 && fabsf(pz-b.z) < 2) gy = .3f;
  if (gk(1) && py <= gy + .01f) vy = 7;
  vy -= 20*dt; py += vy*dt; if (py < gy) { py = gy; vy = 0; }
  mode = gk(2);
  if (gk(0) && cd <= 0) act();
}
static void cone(float x, float y, float z, float rad, float h, float r, float g, float b) {
  for (int i = 0; i < 8; i++) {
    float a0 = i*.7854f, a1 = a0 + .7854f, l = .5f + .5f*fmaxf(0.f, cosf(a0 + .39f - 2.2f));
    vb.push_back({x+rad*cosf(a0), y, z+rad*sinf(a0), r*l, g*l, b*l});
    vb.push_back({x+rad*cosf(a1), y, z+rad*sinf(a1), r*l, g*l, b*l});
    vb.push_back({x, y+h, z, r*l*1.15f, g*l*1.15f, b*l*1.15f});
  }
}
static void rock(float x, float z, float s, float r, float g, float b) {
  float P[6][3] = {{x+s,.4f*s,z},{x,.4f*s,z+s*.9f},{x-s,.4f*s,z},{x,.4f*s,z-s*.9f},{x,s*1.1f,z},{x,0,z}};
  static const int F[8][3] = {{0,1,4},{1,2,4},{2,3,4},{3,0,4},{1,0,5},{2,1,5},{3,2,5},{0,3,5}};
  for (int f = 0; f < 8; f++) { float l = f > 3 ? .45f : .6f + .1f*f;
    for (int k = 0; k < 3; k++) vb.push_back({P[F[f][k]][0], P[F[f][k]][1], P[F[f][k]][2], r*l, g*l, b*l}); }
}
static void render(float dt) {
  float k = fminf(fmaxf(sinf(tod*6.2832f)*2 + .5f, .06f), 1.f), sky[3] = {.45f*k, .7f*k, .95f*k};
  glViewport(0, 0, W, H); glClearColor(sky[0]*.7f, sky[1]*.82f, sky[2]*.98f, 1); glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT); glEnable(GL_DEPTH_TEST);
  float a = (float)W/H, f = 1/tanf(.55f), n = .1f, fa = 120.f;
  float P[16] = {f/a,0,0,0, 0,f,0,0, 0,0,(fa+n)/(n-fa),-1, 0,0,2*fa*n/(n-fa),0};
  float c = cosf(-yaw), s = sinf(-yaw), c2 = cosf(-pitch), s2 = sinf(-pitch);
  float Ry[16] = {c,0,-s,0, 0,1,0,0, s,0,c,0, 0,0,0,1}, Rx[16] = {1,0,0,0, 0,c2,s2,0, 0,-s2,c2,0, 0,0,0,1};
  float T[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, -px,-(py+1.7f),-pz,1}, A[16], B[16];
  mul(A, Rx, Ry); mul(B, A, T); mul(A, P, B);
  int cx = (int)floorf(px/8), cz = (int)floorf(pz/8);
  for (int i = -8; i <= 8; i++) for (int j = -8; j <= 8; j++) {
    int gx = cx+i, gz = cz+j; float v = ((gx*7 + gz*13) & 3)*.03f;
    box(gx*8+4, -.05f, gz*8+4, 8, .05f, 8, .22f+v, .5f+v, .2f+v*.5f);
  }
  for (auto& o : nodes) {
    if (o.hp <= 0) continue; float dx = o.x-px, dz = o.z-pz; if (dx*dx + dz*dz > 8100) continue;
    if (o.type == 0) { box(o.x,0,o.z,.5f,1.4f,.5f,.4f,.25f,.1f); cone(o.x,1.f,o.z,1.7f,1.9f,.1f,.36f,.16f); cone(o.x,2.2f,o.z,1.3f,1.7f,.12f,.44f,.18f); cone(o.x,3.3f,o.z,.85f,1.5f,.16f,.52f,.2f); }
    else if (o.type == 1) { rock(o.x,o.z,1.3f,.5f,.5f,.54f); rock(o.x+.9f,o.z+.5f,.7f,.58f,.58f,.62f); }
    else { rock(o.x,o.z,.6f,.1f,.4f,.12f); box(o.x+.2f,.45f,o.z,.2f,.2f,.2f,.85f,.1f,.15f); box(o.x-.2f,.35f,o.z+.15f,.2f,.2f,.2f,.85f,.1f,.15f); }
  }
  for (auto& b : blks) { if (b.wall) box(b.x,0,b.z,b.w,3,b.d,.6f,.42f,.22f); else box(b.x,0,b.z,b.w,.3f,b.d,.5f,.35f,.18f); }
  if (mode) { Blk g = ghost(); bool ok = wood >= (mode == 1 ? 30 : 20); float gr = ok ? .3f : .9f, gg = ok ? .9f : .25f, gb = ok ? .3f : .2f; if (g.wall) box(g.x,0,g.z,g.w,3,g.d,gr,gg,gb); else box(g.x,.01f,g.z,g.w,.32f,g.d,gr,gg,gb); }
  flush(A, sky, .25f + .75f*k);
  glDisable(GL_DEPTH_TEST);
  float I[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1}, u = fminf(W, H)/400.f;
  if (st.id >= 0) {
    float sx = st.x-st.bx, sy = st.y-st.by, l = hypotf(sx, sy), m = 60*u; if (l > m) { sx *= m/l; sy *= m/l; }
    disc(st.bx, st.by, m, .2f, .2f, .2f); disc(st.bx+sx, st.by+sy, 28*u, .8f, .8f, .8f);
  }
  flush(I, sky, 1);
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
  uM = glGetUniformLocation(p, "m"); uFog = glGetUniformLocation(p, "fog"); uLit = glGetUniformLocation(p, "lit");
  GLuint bf; glGenBuffers(1, &bf); glBindBuffer(GL_ARRAY_BUFFER, bf);
  glEnableVertexAttribArray(0); glEnableVertexAttribArray(1);
  glVertexAttribPointer(0, 3, GL_FLOAT, 0, 24, (void*)0); glVertexAttribPointer(1, 3, GL_FLOAT, 0, 24, (void*)12);
  for (int i = 0; i < 260; i++) {
    float x = (rnd()-.5f)*190, z = (rnd()-.5f)*190; if (x*x + z*z < 40) continue;
    int ty = i % 13 < 7 ? 0 : i % 13 < 10 ? 1 : 2; nodes.push_back({x, z, 0, ty, ty == 0 ? 5 : ty == 1 ? 6 : 1});
  }
  emscripten_set_touchstart_callback("#canvas", 0, 1, tc); emscripten_set_touchmove_callback("#canvas", 0, 1, tc);
  emscripten_set_touchend_callback("#canvas", 0, 1, tc); emscripten_set_touchcancel_callback("#canvas", 0, 1, tc);
  say("Лево: ходьба · Право: камера"); msgT = 6;
  emscripten_set_main_loop(loop, 0, 1);
}
