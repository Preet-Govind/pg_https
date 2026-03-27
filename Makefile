EXTENSION = pg_https
MODULE_big = pg_https

# LIBDIR = pg_https.so


# VPATH = src
OBJS = src/pg_https.o src/https_core.o


DATA = sql/pg_https--1.0.sql

PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)

SHLIB_LINK += -lcurl