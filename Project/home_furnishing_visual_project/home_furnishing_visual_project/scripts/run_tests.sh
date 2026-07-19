#!/usr/bin/env bash
set -euo pipefail

make clean
make
./bin/furnish config/fast.conf
./bin/furnish config/serial_file_example.conf
make openmp
./bin/furnish config/fast.conf
make clean
make

echo "All smoke tests passed."
