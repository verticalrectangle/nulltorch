#!/usr/bin/env bash
# Build the submission to ./convert (invoked by harness/orchestrate.py).
set -e
cd "$(dirname "$0")"
g++ -std=c++20 -O2 convert.cpp -o convert
