#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Chris <goabonga@pm.me>
#
# Creates or updates the repository labels defined in .github/labels.json.
# Idempotent. The `resign` label triggers the resign workflow; Dependabot
# only applies labels that already exist.
#
# Usage: scripts/apply-labels.sh [owner/repo]

set -euo pipefail

repo=${1:-$(gh repo view --json nameWithOwner --jq .nameWithOwner)}
file="$(dirname "$0")/../.github/labels.json"

jq -c '.[]' "$file" | while read -r label; do
  name=$(jq -r .name <<<"$label")
  gh label create "$name" --repo "$repo" \
    --color "$(jq -r .color <<<"$label")" \
    --description "$(jq -r .description <<<"$label")" \
    --force
done
