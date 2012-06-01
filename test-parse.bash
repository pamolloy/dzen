#!/bin/bash
#
#	test-parse.bash - A test script for dzen command line parsing
#

TIME="timeout 2"
APP="./dzen2"

check_status ()
{
if [ $? == 124 ]
then
	echo "## Timeout reached"
else
	echo "## Exit with status $?"
fi
}

echo "# Set lines: -l (required)"
echo "# No argument"
$TIME $APP -l
check_status
echo "# With argument"
$TIME $APP -l 10
check_status
echo -e "\n"
echo "# Set persist: -p (optional)"
echo "# No argument"
$TIME $APP -p
check_status
echo "# With argument"
$TIME $APP -p 1
check_status
