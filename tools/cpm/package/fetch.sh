#!/bin/sh -e

# SPDX-FileCopyrightText: Copyright 2026 crueter
# SPDX-License-Identifier: LGPL-3.0-or-later

# shellcheck disable=SC1091
. "$SCRIPTS/util/fetch.sh"

usage() {
	cat <<EOF
Usage: cpmutil.sh package fetch [a|--all] [PACKAGE]...

Fetch the specified package or packages from their defined download locations.
If the package is already cached, it will not be re-fetched.

EOF

	exit 0
}

while :; do
	case "$1" in
	-h | --help) usage ;;
	-a | --all) ALL=1 ;;
	"$0") break ;;
	"") break ;;
	*) packages="$packages $1" ;;
	esac

	shift
done

[ "$ALL" != 1 ] || packages="${LIBS:-$packages}"
[ -n "$packages" ] || usage

for PACKAGE in $packages; do
	unset JSON
	export PACKAGE

	# shellcheck disable=SC1091
	. "$SCRIPTS"/vars.sh

	fetch_package
done
