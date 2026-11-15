#!/bin/bash
# Release build of Mini-Rust (needs the Emscripten SDK on PATH, tested with 3.1.58).
#   ./build.sh            -> game.js + game.wasm next to main.cpp
#   ./build.sh profile    -> same plus -DPROF: one "PROF {...}" line every 15 frames in the browser console
set -e
FLAGS="-O3 -flto -std=c++17 -DNDEBUG -fno-rtti \
  -sALLOW_MEMORY_GROWTH=1 -sINITIAL_MEMORY=67108864 -sMALLOC=emmalloc \
  -sFILESYSTEM=0 -sENVIRONMENT=web -sGL_TRACK_ERRORS=0 -sASSERTIONS=0"
[ "$1" = "profile" ] && FLAGS="$FLAGS -DPROF"
em++ main.cpp $FLAGS -o game.js
ls -l game.js game.wasm
