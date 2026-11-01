#!/bin/bash
# Sample script matching Slide 30 of Terminator-slides.pdf
# Demonstrates native script interpretation, positional arguments ($1),
# nested if/else control flow, and [ -f ... ] test evaluations.

if [ -f "$1" ]; then
    echo "File exists, compiling $1..."
    gcc "$1" -o myprog
    echo "Compilation successful! Output binary: myprog"
else
    echo "Source file missing: $1"
    echo "Usage: ./check.sh <source_file.c>"
fi
