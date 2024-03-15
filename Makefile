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

#stencils_jit.h: src/stencils.c
#	clang -Wall -Wmissing-prototypes -Wpointer-arith -Wdeclaration-after-statement -Werror=vla -Wendif-labels -Wmissing-format-attribute -Wimplicit-fallthrough=3 -Wcast-function-type -Wshadow=compatible-local -Wformat-security -fno-strict-aliasing -fwrapv -fexcess-precision=standard -Wno-format-truncation -Wno-stringop-truncation -O3 -fno-asynchronous-unwind-tables -fno-builtin -fno-jump-tables -fno-pic -fno-stack-protector -mcmodel=large -I. -I./ -I/usr/include/postgresql/16/server -I/usr/include/postgresql/internal  -Wdate-time -D_FORTIFY_SOURCE=2 -D_GNU_SOURCE -I/usr/include/libxml2   -c -o src/stencils.o src/stencils.c
#	python3 generate-stencils.py

