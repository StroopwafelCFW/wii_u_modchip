#!/bin/bash

trap "echo Exited!; exit;" SIGINT SIGTERM

while true; do
    python3 monitor.py
done