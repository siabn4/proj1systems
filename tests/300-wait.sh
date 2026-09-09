# test wait

# assumes `exitwith` is in PATH
# sleep 3 seconds, then exit with 42
exitwith 3 42 &
# this becomes job %1

# sleep 3 seconds, then exit with 17
exitwith 4 17 &
# this becomes job %2

# wait for job 1, should report 42
wait %1; echo $?

# wait for job 2, should report 17
wait %2; echo $?

# start a few more jobs
exitwith 1 20 &
exitwith 1 21 &
exitwith 1 22 &
exitwith 1 23 &
# wait for all of them
wait
# wait in this case will return 0 always
echo $?
# and all exitwith processes should have exited, so this outputs 0
ps | grep -c exitwith
