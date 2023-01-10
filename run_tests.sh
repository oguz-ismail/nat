run_scenario() {
	printf "$input" | eval "$environment ./nat $arguments"
	status=$?
	printf x
	return $status
}

run_test() {
	printf 'testing if %s is handled correctly... ' "$*"

	output=$(run_scenario)
	status=$?
	expected_output=$(printf "$expected_output"x)

	if test "$output" = "$expected_output" && test $status -eq $expected_status; then
		echo yes
	else
		echo no
		exit 1
	fi
}

input=
arguments='-w -2'
environment=
expected_output=
expected_status=1
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
expected_output=$input
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
unset COLUMNS
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
expected_output=$input
expected_status=1
run_test an item wider than output

input='x\nxx\n'
arguments='-w 4'
environment=
expected_output='x   \nxx  \n'
expected_status=0
run_test a list wider than output

input='xx\nx\nx\nx\nx\nx\nxx\nx\nxx\n'
arguments='-w 9'
environment=
expected_output='xx  x  xx\nx   x  x \nx   x  xx\n'
expected_status=0
run_test 'edge case #1'
