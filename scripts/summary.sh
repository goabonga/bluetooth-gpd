#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Chris <goabonga@pm.me>
#
# Appends a formatted job summary to $GITHUB_STEP_SUMMARY: a title, an alert
# matching the job status, an optional note and the tail of an output file in
# a collapsible block (open when the job failed).
#
# Usage: summary.sh <title> <status> [--note TEXT] [--file PATH] [--tail N]

set -euo pipefail

title=${1:?title}
status=${2:?status}
shift 2

note=""
file=""
tail_lines=30
while [ $# -gt 0 ]; do
  case "$1" in
    --note) note=$2 ;;
    --file) file=$2 ;;
    --tail) tail_lines=$2 ;;
    *) echo "unknown option: $1" >&2; exit 2 ;;
  esac
  shift 2
done

case "$status" in
  success) alert="NOTE"; verdict="Passed" ;;
  failure) alert="CAUTION"; verdict="Failed" ;;
  cancelled) alert="WARNING"; verdict="Cancelled" ;;
  skipped) alert="NOTE"; verdict="Skipped" ;;
  *) alert="NOTE"; verdict="$status" ;;
esac

{
  echo "## ${title}"
  echo
  echo "> [!${alert}]"
  echo "> **${verdict}.**${note:+ ${note}}"
  if [ -n "$file" ] && [ -s "$file" ]; then
    open=""
    if [ "$status" = "failure" ]; then open=" open"; fi
    echo
    echo "<details${open}>"
    echo "<summary>Output (last ${tail_lines} lines)</summary>"
    echo
    echo '```'
    tail -n "$tail_lines" "$file"
    echo '```'
    echo
    echo "</details>"
  fi
} >> "${GITHUB_STEP_SUMMARY:-/dev/stdout}"
