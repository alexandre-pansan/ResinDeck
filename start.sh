#!/bin/sh
# Opens the ResinDeck web interface. Creates the virtualenv on first run and installs the
# dependencies again whenever requirements.txt changes.
cd "$(dirname "$0")" || exit 1

STAMP=.venv/.requirements-installed
if [ ! -x .venv/bin/python ] || [ requirements.txt -nt "$STAMP" ]; then
    echo "Installing dependencies..."
    if command -v uv >/dev/null 2>&1; then
        { [ -x .venv/bin/python ] || uv venv .venv; } &&
            uv pip install --python .venv/bin/python -r requirements.txt || exit 1
    else
        { [ -x .venv/bin/python ] || python3 -m venv .venv; } &&
            .venv/bin/pip install -r requirements.txt || exit 1
    fi
    touch "$STAMP"
fi

exec .venv/bin/python -I resindeck.py ui "$@"
