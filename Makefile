NAME=postgresql-copyjit
MODULES      = $(patsubst %.c,%,$(wildcard src/*.c))
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

