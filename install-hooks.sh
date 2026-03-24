#!/bin/sh

# SPDX-FileCopyrightText: 2025 Tom Haynes <loghyr@gmail.com>
# SPDX-License-Identifier: AGPL-3.0-or-later

# Install git hooks from the hooks/ directory.

REPO_ROOT=$(git rev-parse --show-toplevel 2>/dev/null)
if [ -z "$REPO_ROOT" ]; then
    echo "Not a Git repository."
    exit 1
fi
cd "$REPO_ROOT"

# Ensure hooks tools directory exists
if [ ! -d ".git/hooks/tools" ]; then
    mkdir -p ".git/hooks/tools"
fi

# Install pre-push hook
cp "hooks/pre-push" ".git/hooks/tools/pre-push.sh"
chmod +x ".git/hooks/tools/pre-push.sh"

# Remove existing pre-push link/file if it exists
if [ -e ".git/hooks/pre-push" ]; then
    rm ".git/hooks/pre-push"
fi

ln -s "tools/pre-push.sh" ".git/hooks/pre-push"
echo "Pre-push hook installed."

# Remove stale pre-commit hook if it was the old version
if [ -L ".git/hooks/pre-commit" ] && \
   [ "$(readlink .git/hooks/pre-commit)" = "tools/pre-commit.sh" ]; then
    rm ".git/hooks/pre-commit"
    rm -f ".git/hooks/tools/pre-commit.sh"
    echo "Removed old pre-commit hook (replaced by pre-push)."
fi

exit 0
