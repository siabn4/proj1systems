#
# Test that minibash correctly sets the $? variable to the exit code of the last program
# even if it terminates with a signal.  More specifically, if a program terminates with a
# signal, the value of $? shall be 128 + numeric value of the signal as returned by WTERMSIG()
#
# Assumes `die` is in PATH
#

die -segfault 
echo $?
# 139

die -abort
echo $?
# 134

die -divzero
echo $?
# 136
