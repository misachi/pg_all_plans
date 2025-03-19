MODULE_big = pg_show_plans
OBJS = \
        $(WIN32RES) \
        show.o

EXTENSION = pg_show_plans
DATA = pg_show_plans--1.0.sql
PGFILEDESC = "pg_show_plans - Show all plans"
REGRESS = pg_show_plans

ifndef OLD_INSTALL
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = contrib/pg_show_plans
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif