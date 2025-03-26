#!/bin/bash

# SPDX-FileCopyrightText: 2025 Tom Haynes <loghyr@gmail.com>
# SPDX-License-Identifier: GPL-2.0+

# Function to check if a file has SPDX headers
check_spdx_headers() {
  file="$1"
  copyright_found=0
  license_found=0

  # Check for SPDX-FileCopyrightText
  if grep -q "SPDX-FileCopyrightText:" "$file"; then
    copyright_found=1
  fi

  # Check for SPDX-License-Identifier
  if grep -q "SPDX-License-Identifier:" "$file"; then
    license_found=1
  fi

  if [[ $copyright_found -eq 0 || $license_found -eq 0 ]]; then
    echo "ERROR: Missing SPDX headers in: $file"
    return 1
  else
    return 0
  fi
}

# Get a list of all files, excluding .gitignore, LICENSES directory, .x files, COPYING, and xdrgen.py
files=$(git ls-files | grep -vE '\.gitignore$|LICENSES/|\.x$|COPYING$|lib/scripts/reffs/xdrgen\.py$')

# Check each file
error_count=0
while IFS= read -r file; do
  # Exclude binary files
  if ! file $(which file) -b --mime-type "$file" | grep -q "text/"; then
    continue
  fi

  if ! check_spdx_headers "$file"; then
    error_count=$((error_count + 1))
  fi
done <<< "$files"

# Exit with an error code if any errors were found
if [[ $error_count -gt 0 ]]; then
  echo "$error_count files are missing SPDX headers."
  exit 1
else
  echo "All checked files have SPDX headers."
  exit 0
fi
