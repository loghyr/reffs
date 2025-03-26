#!/bin/bash

# SPDX-FileCopyrightText: 2025 Tom Haynes <loghyr@gmail.com>
# SPDX-License-Identifier: GPL-2.0+

CHECK_EM=$(git status -short | grep -E ".*\.(c|cpp|h|hpp)\b" | awk '{ print $2; }')

for FILE in ${CHECK_EM}
do
	clang-format -i --Werror ${FILE}
	ret=$?

	if [[ $ret -ne 0 ]]; then
		echo -e "Unable to format ${FILE}."
		echo -e "EXTERMINATE!"
		exit $ret
	fi
done

exit 0
