#!/bin/sh
for i in $(seq 1 30); do
    printf "[step %2d/30] Compiling module_%d.c\n" "$i" "$i"
    sleep 0.15
done
