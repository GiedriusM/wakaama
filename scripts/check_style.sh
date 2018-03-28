#!/bin/sh

ASTYLE=./astyle/build/gcc/bin/astyle

if [ ! -x $ASTYLE ]; then
    wget https://sourceforge.net/projects/astyle/files/astyle/astyle%203.1/astyle_3.1_linux.tar.gz || die
    tar xzvf astyle_3.1_linux.tar.gz || die
    cd astyle/build/gcc || die
    make || die
    cd ../../..
fi

output=`$ASTYLE --options=.astyle.conf --dry-run examples/rest-server/*.h examples/rest-server/*.c`
if [ ! -z "$output" ]; then
    exit 1
fi
