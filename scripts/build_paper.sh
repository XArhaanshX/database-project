#!/usr/bin/env bash
# Builds paper/main.pdf. Uses tectonic if available, otherwise latexmk.
# Run scripts/plot_results.py first if the results have changed.
set -euo pipefail
cd "$(dirname "$0")/../paper"

if command -v tectonic >/dev/null; then
  tectonic -X compile main.tex
elif command -v latexmk >/dev/null; then
  latexmk -pdf -interaction=nonstopmode main.tex
  latexmk -c
else
  echo "need tectonic or latexmk" >&2
  exit 1
fi
