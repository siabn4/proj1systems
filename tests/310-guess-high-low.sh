#!/bin/bash

#
# Play a guessing game
#
# seed srandom() literally with 42 (different from bash behavior)
# remove this to play the actual game
RANDOM=42
low=1
high=100
range=$(expr "$high" - "$low" + 1)
randnum=$RANDOM
number=$(expr "$randnum" % "$range" + "$low")
guesses=0

echo "I'm thinking of a number between $low and $high."

while true; do
    read -p "Your guess: " guess

    if echo "$guess" | grep -q '^[0-9][0-9]*$'; then
        guesses=$(expr "$guesses" + 1)

        if [ "$guess" -lt "$number" ]; then
            echo "Too low."
        elif [ "$guess" -gt "$number" ]; then
            echo "Too high."
        else
            echo "Correct! You got it in $guesses guesses."
            break
        fi
    else
        echo "Please enter a number."
    fi
done
