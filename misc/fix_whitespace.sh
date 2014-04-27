#!/bin/bash
sed -i 's/[ \t]*$//' $@ # kill all trailing whitespace
sed -i 's/\t/    /' $@  # turn each tab into 4 spaces
