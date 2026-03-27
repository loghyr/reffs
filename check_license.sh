#!/bin/bash

# SPDX-FileCopyrightText: 2025 Tom Haynes <loghyr@gmail.com>
# SPDX-License-Identifier: AGPL-3.0-or-later

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

# Get a list of all files, excluding .gitignore, LICENSES directory, .x files, LICENSE, COPYING files, and the entire lib/scripts/reffs/pynfs directory
files=$(git ls-files | grep -vE '\.gitignore$|LICENSES/|\.x$|^LICENSE$|(^|/)COPYING$|lib/scripts/reffs/pynfs/|deploy/benchmark/results/')

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

# ---------------------------------------------------------------
# License compatibility check.
#
# reffs is AGPL-3.0-or-later.  AGPL-3.0 is compatible with:
#   - AGPL-3.0-or-later, AGPL-3.0-only
#   - GPL-3.0-or-later, GPL-3.0-only
#   - LGPL-3.0-or-later, LGPL-3.0-only
#   - LGPL-2.1-or-later (upgradeable to LGPL-3.0)
#   - GPL-2.0-or-later (upgradeable to GPL-3.0)
#   - MIT, BSD-2-Clause, BSD-3-Clause, ISC, Apache-2.0
#
# NOT compatible:
#   - GPL-2.0-only (cannot upgrade to GPL-3.0)
#   - Any proprietary or incompatible copyleft
# ---------------------------------------------------------------

INCOMPATIBLE_LICENSES="GPL-2.0-only"

compat_errors=0
while IFS= read -r file; do
  license=$(grep -oP 'SPDX-License-Identifier:\s*\K\S+' "$file" 2>/dev/null || true)
  for bad in $INCOMPATIBLE_LICENSES; do
    if [[ "$license" == "$bad" ]]; then
      echo "ERROR: Incompatible license $bad in: $file (reffs is AGPL-3.0-or-later)"
      compat_errors=$((compat_errors + 1))
    fi
  done
done <<< "$files"

# Exit with an error code if any errors were found
total_errors=$((error_count + compat_errors))
if [[ $total_errors -gt 0 ]]; then
  [[ $error_count -gt 0 ]] && echo "$error_count files are missing SPDX headers."
  [[ $compat_errors -gt 0 ]] && echo "$compat_errors files have incompatible licenses."
  exit 1
else
  echo "All checked files have SPDX headers and compatible licenses."
  exit 0
fi
