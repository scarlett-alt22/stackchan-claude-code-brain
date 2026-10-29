#!/bin/bash
# Launcher for stt_whisper_server.py under launchd.
# Keep this directory under ~/Library/Application Support/, NOT ~/Documents/:
# from ~/Documents, launchd-started jobs could not exec the venv python
# (bash 126 / launchd 78 EX_CONFIG) because of macOS privacy controls (TCC),
# while the same command worked fine in a terminal.
cd "$HOME/Library/Application Support/StackChanSTT" || exit 1
exec ./.venv/bin/python ./stt_whisper_server.py
