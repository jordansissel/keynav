#!/bin/sh
set -e

Xvfb :4 &
PID_XVFB=$!
sleep 1

export DISPLAY=:4

./keynav 2>&1 >keynav.log &
PID_KEYNAV=$!

sleep 1
xdotool getmouselocation
sleep 1
xdotool key ctrl+space j l k h space
sleep 1
xdotool getmouselocation
sleep 1

kill -9 $PID_XVFB
kill -9 $PID_KEYNAV
