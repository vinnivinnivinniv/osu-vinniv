#!/bin/bash
set -e
cd "$(dirname "$0")"
echo "Building tests..."
g++ -std=c++17 -O0 -g -Wall -Wextra -Wno-unused-parameter \
    test_parser.cpp -o test_parser
echo "Running tests..."
./test_parser
