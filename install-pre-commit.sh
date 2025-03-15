#!/bin/sh

# Change to the root directory of the Git repository
REPO_ROOT=$(git rev-parse --show-toplevel 2>/dev/null)
if [ -z "$REPO_ROOT" ]; then
    echo "Not a Git repository."
    exit 1
fi
cd "$REPO_ROOT"

# Check if .git/hooks/tools exists
if [ ! -d ".git/hooks/tools" ]; then
    mkdir -p ".git/hooks/tools"
    echo "Created .git/hooks/tools directory."
fi

# Copy the pre-commit script to .git/hooks/tools
cp "hooks/pre-commit" ".git/hooks/tools/pre-commit.sh"

# Ensure the script is executable
chmod +x ".git/hooks/tools/pre-commit.sh"

# Remove existing pre-commit link if it exists
if [ -e ".git/hooks/pre-commit" ]; then
    rm ".git/hooks/pre-commit"
fi

# Create the symbolic link
ln -s "tools/pre-commit.sh" ".git/hooks/pre-commit"

echo "Pre-commit hook installed."
exit 0
