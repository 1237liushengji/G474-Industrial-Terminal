#!/bin/sh
# A-01 wrapper (docs/05): single configuration point check.
# Requires python on PATH (the project already depends on Python 3 for Tools/).
cd "$(dirname "$0")" || exit 2
exec python check_config_points.py "$@"
