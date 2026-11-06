#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Chris <goabonga@pm.me>
#
# Creates or updates (by name) the repository rulesets defined in
# .github/rulesets/*.json. Idempotent. Needs a plan that supports rulesets on
# private repositories, or a public repository.
#
# Usage: scripts/apply-rulesets.sh [owner/repo]

set -euo pipefail

repo=${1:-$(gh repo view --json nameWithOwner --jq .nameWithOwner)}
dir="$(dirname "$0")/../.github/rulesets"

for file in "$dir"/*.json; do
  name=$(jq -r .name "$file")
  id=$(gh api "repos/${repo}/rulesets" --jq ".[] | select(.name == \"${name}\") | .id")
  if [ -n "$id" ]; then
    gh api -X PUT "repos/${repo}/rulesets/${id}" --input "$file" --silent
    echo "updated ruleset ${name} (${id})"
  else
    gh api -X POST "repos/${repo}/rulesets" --input "$file" --silent
    echo "created ruleset ${name}"
  fi
done
