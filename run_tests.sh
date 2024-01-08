# Copyright 2023, 2024 Oğuz İsmail Uysal <oguzismailuysal@gmail.com>
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program. If not, see <https://www.gnu.org/licenses/>.

# {{{
escape_for_printf() {
	eval "tmp=\$$1"
	
	case $tmp in
	-* )
		tmp="\055${tmp#-}"
	esac

	case $tmp in
	*%* )
		tmp=$(printf '%sx\n' "$tmp" | sed 's/%/%%/g')
		tmp=${tmp%x}
	esac

	eval "$1=\$tmp"
}

build_expected_result() {
	escape_for_printf expected_output
	printf "${expected_output}x%d\n" "$expected_status"
}

run_scenario() {
	escape_for_printf input
	printf "$input" | eval "$environment $program $arguments"
	printf 'x%s\n' $?
}

run_test() {
	printf 'testing if %s is handled correctly... ' "$*"

	expected_result=$(build_expected_result | od)
	result=$(run_scenario | od)

	if test "$result" = "$expected_result"; then
		echo yes
	else
		echo no
		exit 1
	fi
}
# }}}

program=./nat
export LC_ALL=C
unset COLUMNS

input=
arguments='-c -1'
environment=
expected_output=
expected_status=2
run_test an invalid argument 2>/dev/null

input='x\n'
arguments='-w -78'
environment=
expected_output='x \n'
expected_status=0
run_test a relative width 2>/dev/null

input='x\nxx\n'
arguments='-w 8'
environment=
expected_output='x  xx   \n'
expected_status=0
run_test more than enough room

input='x\nx\n'
arguments='-p 2 -w 1'
environment=
expected_output=$input
expected_status=0
run_test no room for padding

input='\n\n'
arguments='-p 2 -w 1'
environment=
expected_output=' \n \n'
expected_status=0
run_test 'no room for padding #2'

input='x\n'
arguments='-w 0'
environment=
expected_output='\n'
expected_status=1
run_test no room at all

input='xy'
arguments='-d y -w 1'
environment=
expected_output='x\n'
expected_status=0
run_test a custom delimiter

input='x\0x\0'
arguments='-d "" -w 1'
environment=
expected_output='x\nx\n'
expected_status=0
run_test NUL as delimiter

input='x\n'
arguments=
environment='COLUMNS=2'
expected_output='x \n'
expected_status=0
run_test COLUMNS 2>/dev/null

input=
arguments=
environment=
expected_output=
expected_status=0
run_test an empty list

input='\n'
arguments='-w 1'
environment=
expected_output=' \n'
expected_status=0
run_test an empty item

input='\n\n'
arguments='-w 2'
environment=
expected_output='  \n'
expected_status=0
run_test a list of empty items

input='x'
arguments='-w 1'
environment=
expected_output='x\n'
expected_status=0
run_test a missing delimiter

input='xyx\n'
arguments='-d y -w 1'
environment=
expected_output='x\nx\n'
expected_status=0
run_test a trailing newline

input='xx\n'
arguments='-w 1'
environment=
expected_output='x\n'
expected_status=1
run_test not enough room

input='x\nxx\n'
arguments='-w 4'
environment=
expected_output='x   \nxx  \n'
expected_status=0
run_test room for one column only

input='\0x\nx\n'
arguments='-w 4'
environment=
expected_output='\0x  x\n'
expected_status=0
run_test an item containing NULs

input='xx\nx\nx\nx\nx\nx\nxx\nx\nxx\n'
arguments='-w 9'
environment=
expected_output='xx  x  xx\nx   x  x \nx   x  xx\n'
expected_status=0
run_test 'edge case #1'

input='x\n\n'
arguments='-p 0 -w 1'
environment=
expected_output='x\n'
expected_status=0
run_test 'edge case #2'

input='x\ny\nx\n'
arguments='-w 4 -a'
environment=
expected_output='x  y\nx   \n'
expected_status=0
run_test -a

input='x\nxx\n'
arguments='-c 1'
environment=
expected_output='x \nxx\n'
expected_status=0
run_test -c

input='x\n'
arguments='-c 2'
environment=
expected_output='x  \n'
expected_status=0
run_test more columns than items

input='x\nx\nx\nx\n'
arguments='-c 3'
environment=
expected_output='x  x  \nx  x  \n'
expected_status=0
run_test an unused column

input='x\nx\nx\nx\n'
arguments='-c 3 -a'
environment=
expected_output='x  x  x\nx      \n'
expected_status=0
run_test -c with -a

input='xx\n'
arguments='-w 1 -c 1'
environment=
expected_output=$input
expected_status=0
run_test -c overriding -w

input='x\tx\nxx\tx\n'
arguments='-t'
environment=
expected_output='x   x\nxx  x\n'
expected_status=0
run_test -t

input='x\tx\n\tx\n'
arguments='-t'
environment=
expected_output='x  x\n   x\n'
expected_status=0
run_test an empty cell

input='x\nx\tx\n'
arguments='-t'
environment=
expected_output='x   \nx  x\n'
expected_status=0
run_test an extra field

input='x\tx\nx\n'
arguments='-t'
environment=
expected_output='x  x\nx   \n'
expected_status=0
run_test a missing field

input='\t'
arguments='-t'
environment=
expected_output='  \n'
expected_status=0
run_test 'corner case #1'

input='x\tx\tx\n'
arguments='-t -c 2'
environment=
expected_output='x  x\tx\n'
expected_status=0
run_test -t with -c

input='x x\tx\nx\n'
arguments='-s -w 10'
environment=
expected_output='x  x  x  x\n'
expected_status=0
run_test -s

input=' \n'
arguments='-s'
environment=
expected_output=
expected_status=0
run_test a blank input

input=' x\n'
arguments='-s -w 2'
environment=
expected_output='x \n'
expected_status=0
run_test leading whitespace

input='x\nx \n'
arguments='-s -w 4'
environment=
expected_output='x  x\n'
expected_status=0
run_test trailing whitespace

input='x x\n x\tx\n'
arguments='-t -s'
environment=
expected_output='x  x\nx  x\n'
expected_status=0
run_test -t with -s

input=' '
arguments='-t -s'
environment=
expected_output='\n'
expected_status=0
run_test 'corner case #2'

input='x\nxx\n'
arguments='-w 2 -r 1'
environment=
expected_output=' x\nxx\n'
expected_status=0
run_test -r

input='x\nxx\nx\nxx\n'
arguments='-w 6 -r -1'
environment=
expected_output='x    x\nxx  xx\n'
expected_status=0
run_test a negative column number

input='x\nxx\nx\nxx\nx\nxx\n'
arguments='-w 10 -r 1~2'
environment=
expected_output=' x  x    x\nxx  xx  xx\n'
expected_status=0
run_test a sequence

input='x\nxx\nx\nxx\nx\nxx\n'
arguments='-w 10 -r -1~2'
environment=
expected_output=' x  x    x\nxx  xx  xx\n'
expected_status=0
run_test a backward sequence

input='x\nxx\nx\nxx\nx\nxx\n'
arguments='-w 10 -r -2~1'
environment=
expected_output=' x   x  x \nxx  xx  xx\n'
expected_status=0
run_test selecting the first half

input='x\nxx\nx\nxx\nx\nxx\nx\nxx\n'
arguments='-w 14 -r 1~3,2~2'
environment=
expected_output=' x   x  x    x\nxx  xx  xx  xx\n'
expected_status=0
run_test sequences overlapping

input='\33[mx\n'
arguments='-R -w 4'
environment=
expected_output='\33[mx   \n'
expected_status=0
run_test -R

input='x x  x'
arguments='-S -w 3'
environment=
expected_output='x x\nx  \n'
expected_status=0
run_test -S

input='x '
arguments='-S -w 1'
environment=
expected_output='x\n'
expected_status=0
run_test trailing space with -S

# vim: fdm=marker
