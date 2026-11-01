#!/bin/bash
# Sample test script for native shell scripting engine
# Demonstrates native script interpretation, positional arguments ($1),
# nested if/else control flow, and [ -f ... ] test evaluations.

if [ -f "$1" ]; then
    echo "File exists: $1"
    echo "Verification successful for argument: $1"
else
    echo "Source file missing: $1"
    echo "Usage: ./check.sh <source_file.c>"
fi
