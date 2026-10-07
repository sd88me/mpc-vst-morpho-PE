#!/bin/sh
# The source of truth for mpc_analog.h is this directory (Morpho-PE builds with -Ianalog); other ports keep a synced copy.
# sync.sh DIR          copy mpc_analog.h into DIR
# sync.sh --check DIR  exit 1 if DIR/mpc_analog.h differs
here=$(cd "$(dirname "$0")" && pwd)
if [ "$1" = "--check" ]; then cmp -s "$here/mpc_analog.h" "$2/mpc_analog.h" || { echo "$2/mpc_analog.h differs from $here/mpc_analog.h"; exit 1; }
else cp "$here/mpc_analog.h" "$2/mpc_analog.h"; fi
