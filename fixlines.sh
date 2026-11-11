#!/bin/sh
# fixlines.sh -- strip CRLF (\r) line endings from text files, in place.
#
# This Windows checkout historically produced CRLF files (core.autocrlf=true);
# FreeBSD /bin/sh and make(1) reject CRLF ("invalid option", "missing
# separator").  .gitattributes (committed) makes every git checkout LF, but
# any non-git transfer (raw folder copy, old archive) can still poison a
# tree.  Run this once if a build suddenly complains about line endings:
#
#     sh fixlines.sh
#
# Firmware blobs and .git internals are never touched.

set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
cd "$HERE" || exit 1

echo "==> stripping CRLF line endings from text files"
find . -type f \( -name '*.sh' -o -name '*.c' -o -name '*.h' \
    -o -name '*.md' -o -name 'Makefile' -o -name '*.pl' \
    -o -name '*.py' -o -name '*.svg' \) \
    -not -path './.git/*' -not -path './firmware/*' \
    -exec perl -pi -e 's/\r$//' {} +
echo "==> done.  Verify with:  grep -c \$'\\r' build.sh   (want 0)"
