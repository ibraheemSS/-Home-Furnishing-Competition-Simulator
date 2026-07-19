#!/usr/bin/env bash
set -euo pipefail

# Build the simulator if needed.
if [ ! -x bin/furnish ]; then
    make
fi

# Build the visualizer if needed. This requires freeglut/OpenGL development packages.
if [ ! -x bin/visualizer ]; then
    make visualizer
fi

# Run only the simulator. The master process creates the FIFO and launches
# bin/visualizer as a child process automatically.
./bin/furnish config/graphics.conf
