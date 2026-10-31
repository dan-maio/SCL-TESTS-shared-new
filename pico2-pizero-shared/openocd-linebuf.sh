#!/usr/bin/env bash
# openocd-linebuf.sh — the xPack OpenOCD with line-buffered output, for live consoles.
OCD_DIR="$(ls -d "$HOME"/.local/xPacks/@xpack-dev-tools/openocd/*/.content | sort -V | tail -1)"
# Its ports stay off, so that this can run beside another board's OpenOCD.
exec stdbuf -oL -eL "$OCD_DIR/bin/openocd" -s "$OCD_DIR/openocd/scripts" -c "tcl port disabled" -c "telnet port disabled" "$@"
