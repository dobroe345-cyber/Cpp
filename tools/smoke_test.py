#!/usr/bin/env python3
"""Smoke test for Mini-Rust: opens the built game in headless Chrome and checks that it really starts.

    python3 tools/smoke_test.py <folder with index.html, game.js, game.wasm, models/ ...> [screenshot.png]

Passes only if the world was generated (the "Сид ..." line of the world menu is filled in), the HUD shows no
error text, and the page raised no JavaScript errors / shader-error alerts. Exit code 0 = OK, 1 = broken.
"""
import functools, http.server, os, socketserver, sys, threading, time

from playwright.sync_api import sync_playwright

root = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else '.')
shot = sys.argv[2] if len(sys.argv) > 2 else 'smoke.png'
LIMIT = 90   # seconds (software GL on a CI machine is slow, a real device needs about one second)
BAD_HUD = ('Ошибка', 'Сбой', 'Игра не запустилась', 'Файл game.js')


class Handler(http.server.SimpleHTTPRequestHandler):
    extensions_map = {**http.server.SimpleHTTPRequestHandler.extensions_map, '.wasm': 'application/wasm', '.js': 'text/javascript'}

    def log_message(self, *a):
        pass


server = socketserver.ThreadingTCPServer(('127.0.0.1', 0), functools.partial(Handler, directory=root))
server.daemon_threads = True
threading.Thread(target=server.serve_forever, daemon=True).start()
url = 'http://127.0.0.1:%d/index.html' % server.server_address[1]

errors, dialogs, console_errors = [], [], []
state = {}
tree_dir = os.path.join(root, 'models', 'tree_assets')
want_trees = os.path.isdir(tree_dir) and any(f.endswith('.glb') for f in os.listdir(tree_dir))   # plant models shipped -> they must load too
trees = {}
with sync_playwright() as p:
    chrome = '/usr/bin/google-chrome'
    browser = p.chromium.launch(
        **({'executable_path': chrome} if os.path.exists(chrome) else {'channel': 'chrome'}),
        headless=True,
        args=['--no-sandbox', '--use-gl=angle', '--use-angle=swiftshader', '--enable-unsafe-swiftshader', '--ignore-gpu-blocklist'])
    page = browser.new_context(viewport={'width': 900, 'height': 500}, has_touch=True).new_page()
    page.on('pageerror', lambda e: errors.append(str(e)[:300]))
    page.on('dialog', lambda d: (dialogs.append(d.message[:300]), d.dismiss()))
    page.on('console', lambda m: console_errors.append(m.text[:200]) if m.type == 'error' else None)
    page.goto(url)
    t0 = time.time()
    while time.time() - t0 < LIMIT:
        time.sleep(0.5)
        try:
            state = page.evaluate("({hud: document.getElementById('hud').textContent, world: document.getElementById('winfo').textContent})")
        except Exception as e:   # page is navigating / crashed
            state = {'hud': 'evaluate failed: %s' % e, 'world': ''}
        if state['world'].startswith('Сид') or errors or dialogs:
            break
    if want_trees and not errors and not dialogs:   # the plant models are parsed after the building models: wait for them, then give the GPU upload a few frames
        while time.time() - t0 < LIMIT:
            try:
                trees = page.evaluate("({rdy: window.TRDY | 0, n: Object.keys(window.TR || {}).length})")
            except Exception as e:
                trees = {'rdy': 0, 'n': 0}
            if trees['rdy'] or errors or dialogs:
                break
            time.sleep(0.5)
        time.sleep(8)
    time.sleep(1.5)   # let a few frames render before the screenshot
    page.screenshot(path=shot)
    browser.close()

problems = []
if not state.get('world', '').startswith('Сид'):
    problems.append('the world was not generated within %d s (game stuck on loading)' % LIMIT)
if any(state.get('hud', '').startswith(b) for b in BAD_HUD):
    problems.append('HUD shows an error: %r' % state['hud'])
if want_trees and (not trees.get('rdy') or trees.get('n', 0) < 1):
    problems.append('the plant models (models/tree_assets) were not loaded: %r' % trees)
problems += ['JavaScript error: %s' % e for e in errors]
problems += ['alert shown (shader error?): %s' % d for d in dialogs]

print('state:', state, 'plants:', trees)
if console_errors:
    print('console errors (informational, e.g. a missing .glb model):', sorted(set(console_errors))[:5])
if problems:
    print('SMOKE TEST FAILED')
    for pr in problems:
        print(' -', pr)
        print('::error::%s' % pr)
    sys.exit(1)
print('SMOKE TEST OK:', state['world'])
