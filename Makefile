# contrib/pg_multiplan

EXTENSION = pg_multiplan
DATA = pg_multiplan--1.0.sql

MODULE_big = pg_multiplan

OBJS = \
	$(WIN32RES) \
	pg_multiplan.o

PGFILEDESC = "pg_multiplan - saver postgresql plans"

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = contrib/pg_multiplan
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
