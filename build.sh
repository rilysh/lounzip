#!/usr/bin/env sh

has_compiler() {
    command -v cc 1>/dev/null
}

build() {
    has_compiler
    if [ $? -ne 0 ]
    then
        printf "error: no C compiler was found.\n"
	exit 1
    fi

    PROGRAM=lounzip.c
    LIB=zip
    cc $PROGRAM -o ${PROGRAM%%.c} -l$LIB
}

build

