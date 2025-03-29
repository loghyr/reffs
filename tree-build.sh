#!/bin/bash

# SPDX-FileCopyrightText: 2025 Tom Haynes <loghyr@gmail.com>
# SPDX-License-Identifier: GPL-2.0+

# Check if a build directory is provided as an argument
if [ -n "$1" ]; then
    build_dir="$1"
else
    # Create a unique temporary build directory in the parent directory
    build_dir="$(mktemp -d "$(dirname "$(pwd)")/.reffs.build.XXXXXX")"
    cleanup_build_dir=1  # Flag to indicate we should clean up the directory
    echo "Using temporary build directory: $build_dir"
fi

# Create build directory if it doesn't exist
mkdir -p "$build_dir"

# Get current working directory
repo_root=$(pwd)  # Use repo_root for clarity

# Configure the build
cd "$build_dir"
"$repo_root/configure"  # Use repo_root for clarity

# Check the configure result
if [ $? -ne 0 ]; then
    echo "Configure failed. Aborting commit."
    if [ "$cleanup_build_dir" -eq 1 ]; then
        rm -rf "$build_dir"
    fi
    exit 1
fi

# Build the project
make

# Check the build result
if [ $? -ne 0 ]; then
    echo "Build failed. Aborting commit."
    if [ "$cleanup_build_dir" -eq 1 ]; then
        rm -rf "$build_dir"
    fi
    exit 1
fi

# Run tests (if available)
make check

# Check the test result
if [ $? -ne 0 ]; then
    echo "Pre-commit tests failed. Aborting commit."
    if [ "$cleanup_build_dir" -eq 1 ]; then
        rm -rf "$build_dir"
    fi
    exit 1
fi

# Clean up the build directory if it's temporary
if [ "$cleanup_build_dir" -eq 1 ]; then
    rm -rf "$build_dir"
fi

# Allow the commit
exit 0
