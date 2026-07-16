#!/usr/bin/env bash
# Compile your converter to ./convert  (invocation: ./convert <file.pth> <out>)
set -e
cd "$(dirname "$0")"
g++ -std=c++20 -O2 convert.cpp -o convert
