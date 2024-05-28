NAME=postgresql-copyjit
MODULES      = src/copyjit
PG_CONFIG    ?= pg_config

PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)

VERSION=`git describe | sed 's/^v//; s/-/./g' `

version:
	@(echo $(VERSION))

name:
	@(echo $(NAME))

fullname:
	@(echo $(NAME)-$(VERSION))


DIST_FILES = \
	Makefile \
	src \
	README.md COPYING

dist: clean
	-mkdir sdist
	rm -rf sdist/$(NAME)-$(VERSION)
	mkdir -p sdist/$(NAME)-$(VERSION)
	for i in $(DIST_FILES); do \
		cp -R "$$i" sdist/$(NAME)-$(VERSION); \
	done

dist-bzip2: dist
	-mkdir sdist
	cd sdist && tar cfj ../sdist/$(NAME)-$(VERSION).tar.bz2 $(NAME)-$(VERSION)

src/stencils.o: src/stencils.c
	clang -Wall -Wpointer-arith -Wdeclaration-after-statement -Werror=vla -Wendif-labels -Wmissing-format-attribute -Wimplicit-fallthrough=3 -Wcast-function-type -Wshadow=compatible-local -Wformat-security -fno-strict-aliasing -fwrapv -fexcess-precision=standard -Wno-format-truncation -Wno-stringop-truncation -O3 -fno-asynchronous-unwind-tables -fno-builtin -fno-jump-tables -fno-pic -fno-stack-protector -mcmodel=large $(CPPFLAGS) -c -o src/stencils2.o src/stencils.c

src/stencils.json: src/stencils2.o
	llvm-readobj --elf-output-style=JSON --pretty-print --expand-relocs --section-data --section-relocations --section-symbols --sections src/stencils2.o > src/stencils2.json

src/built-stencils.h: src/stencils2.json src/stencil-builder.py
	python3 src/stencil-builder.py src/stencils2.json > src/built-stencils2.h

