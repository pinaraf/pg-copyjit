#!/bin/sh

set -ex

clang -Wall -Wmissing-prototypes -Wpointer-arith -Wdeclaration-after-statement -Werror=vla -Wendif-labels -Wmissing-format-attribute -Wimplicit-fallthrough=3 -Wcast-function-type -Wshadow=compatible-local -Wformat-security -fno-strict-aliasing -fwrapv -fexcess-precision=standard -Wno-format-truncation -Wno-stringop-truncation -O3 -fno-asynchronous-unwind-tables -fno-builtin -fno-jump-tables -fno-pic -fno-stack-protector -mcmodel=large -I. -I./ -I/usr/include/postgresql/16/server -I/usr/include/postgresql/internal  -Wdate-time -D_FORTIFY_SOURCE=2 -D_GNU_SOURCE -I/usr/include/libxml2   -c -o stencils.o stencils.c

llvm-readobj --elf-output-style=JSON --pretty-print --expand-relocs --section-data --section-relocations --section-symbols --sections stencils.o > stencils.json

python stencil-builder.py stencils.json > built-stencils.h
