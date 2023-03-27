# Copyright 2023 Oğuz İsmail Uysal <oguzismailuysal@gmail.com>
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
	printf "$input" | eval "$environment $binary $arguments"
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

binary=./nat
export LC_ALL=C
unset COLUMNS

input=
arguments='-w -2'
environment=
expected_output=
expected_status=2
run_test an invalid argument 2>/dev/null

input='x\nxx\n'
arguments='-w 8'
environment=
expected_output='x  xx   \n'
expected_status=0
run_test an output wider than list

input='x\nx\n'
arguments='-p 2 -w 1'
environment=
expected_output=$input
expected_status=0
run_test a padding wider than output

input='x\n'
arguments='-w 0'
environment=
expected_output='\n'
expected_status=1
run_test an output of zero width

input=xy
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
run_test a NUL as delimiter

input='x\n'
arguments=
environment=COLUMNS=2
expected_output='x \n'
expected_status=0
run_test '$COLUMNS' 2>/dev/null

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

input=x
arguments='-w 1'
environment=
expected_output='x\n'
expected_status=0
run_test a missing delimiter

input='xx\n'
arguments='-w 1'
environment=
expected_output='x\n'
expected_status=1
run_test an item wider than output

input='x\nxx\n'
arguments='-w 4'
environment=
expected_output='x   \nxx  \n'
expected_status=0
run_test a list wider than output

input='\0x\nx\n'
arguments='-w 4'
environment=
expected_output='\0x  x\n'
expected_status=0
run_test an item containing a NUL

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
run_test a vacant column

input='x\nx\nx\nx\n'
arguments='-c 3 -a'
environment=
expected_output='x  x  x\nx      \n'
expected_status=0
run_test -a -c

input='xx\n'
arguments='-w 1 -c 1'
environment=
expected_output=$input
expected_status=0
run_test -c overriding -w

# vim: fdm=marker
