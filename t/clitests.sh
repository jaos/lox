#!/bin/bash
#
# Copyright (C) 2022-2023 Jason Woodward <woodwardj at jaos dot org>
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; either version 2 of the License, or
# any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU Library General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software
# Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
set -fexuo pipefail

if [ -z "${1:-}" ]; then
    echo "$0 <binary>"
    exit 1
fi

TEST_TMPDIR=$(mktemp -d)
trap "rm -rf ${TEST_TMPDIR};" err exit

tater=$1
echo 'let a = 1;' | ${tater}
${tater} /tmp/nosuchfileordir || true # expected failure
${tater} /tmp/nosuchfileordir noarg || true # expected failure
echo 'let a = 1;' > "${TEST_TMPDIR}/t.tot"
${tater} "${TEST_TMPDIR}/t.tot"
${tater} --version
${tater} --help
${tater} --debug --gc-trace --gc-stress --invalid-option-will-fail || true
