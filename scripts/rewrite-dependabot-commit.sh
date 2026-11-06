#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Chris <goabonga@pm.me>
#
# Re-authors and re-signs a Dependabot commit as a Conventional Commit. The
# project only has GitHub Actions dependencies (not shipped, no release), so
# every Dependabot commit becomes `ci: <subject>`.
#
# Commands:
#   rewrite-dependabot-commit.sh          amend HEAD (run by `git rebase --exec`)
#   rewrite-dependabot-commit.sh title R  print the pull request title for the
#                                         commit range R (needs $PR_TITLE)

set -euo pipefail

leading_type='^[a-z]+(\([^)]+\))?!?:[[:space:]]*'

if [ "${1:-}" = "title" ]; then
  range=${2:?commit range}
  mapfile -t subjects < <(git log --reverse --pretty=%s "$range")
  if [ "${#subjects[@]}" -eq 1 ]; then
    echo "${subjects[0]}"
  else
    echo "ci: $(sed -E "s/${leading_type}//" <<<"${PR_TITLE:?PR_TITLE}")"
  fi
  exit 0
fi

subject=$(git log -1 --pretty=%s HEAD | sed -E "s/${leading_type}//")
body=$(git log -1 --pretty=%b HEAD | grep -viE '^co-authored-by:' || true)
message="ci: ${subject}"
if [ -n "$(tr -d '[:space:]' <<<"$body")" ]; then
  message="${message}"$'\n\n'"${body}"
fi
git commit --amend --reset-author -m "$message" --quiet
