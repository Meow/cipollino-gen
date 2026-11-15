#!/bin/sh
# Container entry point: refuse to search if the keys would die with the container.
#
# A key can take days to find. Written to the container's own filesystem it is gone the
# moment a `docker run --rm` container exits, so a search only starts when the output
# directory is on a mount from outside (a bind mount or a named volume).
set -eu

dir=.
search=1
prev=
for arg in "$@"; do
    case "$prev" in
        -d | --dir) dir=$arg ;;
    esac
    case "$arg" in
        --bench | --selftest | --tune | -h | --help) search=0 ;;
    esac
    prev=$arg
done
[ "$#" -gt 0 ] || search=0

if [ "$search" = 1 ] && [ "${CIPOLLINO_EPHEMERAL:-0}" != 1 ]; then
    probe=$dir
    while [ ! -e "$probe" ]; do probe=$(dirname "$probe"); done
    if [ "$(stat -c %d "$probe")" = "$(stat -c %d /)" ]; then
        echo "error: $(realpath -m "$dir") is inside the container, so keys written there would be lost when it exits." >&2
        echo "       Mount a host directory, e.g.  docker run --gpus all -v \"\$PWD/keys:/keys\" ..." >&2
        echo "       (set CIPOLLINO_EPHEMERAL=1 to run anyway)" >&2
        exit 2
    fi
fi

exec cipollino "$@"
