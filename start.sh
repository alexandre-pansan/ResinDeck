#!/bin/sh
# Opens the halot-send web interface. Creates the virtualenv on first run.
cd "$(dirname "$0")" || exit 1

if [ ! -x .venv/bin/python ]; then
    echo "Setting up the environment (first run only)..."
    if command -v uv >/dev/null 2>&1; then
        uv venv .venv && uv pip install --python .venv/bin/python -r requirements.txt || exit 1
    else
        python3 -m venv .venv && .venv/bin/pip install -r requirements.txt || exit 1
    fi
fi

exec .venv/bin/python -I halot_send.py ui "$@"
