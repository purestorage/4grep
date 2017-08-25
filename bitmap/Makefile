CC=gcc
CFLAGS=-Wall -std=gnu11 -O3 -fPIC
LIBS=-lz ./lib/zstd/lib/libzstd.a -llockfile -lpthread
INCLUDES = -I./src -I./lib -I./lib/xxhash -I./lib/zstd/lib
HEADERS := $(shell find ./src -name "*.h")

SRCS_FILES := $(shell find ./src -name "*.c")
SRCS_FILES += ./lib/xxhash/xxhash.c
EXEDIR=exec
MAINDIR=main
UNAME_S := $(shell uname -s)
LIBFLAGS=
ifeq ($(UNAME_S),Linux)
	LIBFLAGS += -Wl,-soname,4grep.so
endif

ZSTD_STATIC=./lib/zstd/lib/libzstd.a

all: $(EXEDIR)/test $(EXEDIR)/generate_bitmap 4grep.so

SRCS_OBJECTS := $(patsubst %.c, %.o, $(SRCS_FILES))

%.o: %.c $(HEADERS)
	@$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(ZSTD_STATIC):
	@$(MAKE) -s -C ./lib/zstd/lib CFLAGS="-fPIC -O3" libzstd.a

4grep.so: $(SRCS_OBJECTS) $(ZSTD_STATIC)
	@$(CC) $(CFLAGS) $(LIBFLAGS) -o 4grep.so $(SRCS_OBJECTS) -shared $(LIBS)

$(EXEDIR)/generate_bitmap: $(MAINDIR)/generate_bitmap.o $(SRCS_OBJECTS) $(ZSTD_STATIC)
	@$(CC) $(CFLAGS) $(SRCS_OBJECTS) $(MAINDIR)/generate_bitmap.o -o \
		$(EXEDIR)/generate_bitmap $(LIBS)

$(EXEDIR)/test: $(MAINDIR)/test.o $(SRCS_OBJECTS) $(ZSTD_STATIC)
	@$(CC) $(CFLAGS) $(SRCS_OBJECTS) $(MAINDIR)/test.o -o $(EXEDIR)/test $(LIBS)

clean:
	@$(RM) $(EXEDIR)/generate_bitmap $(EXEDIR)/4gram_filter $(EXEDIR)/test */*.o 4grep.so $(ZSTD_STATIC) ./lib/xxhash/*.o
	@$(MAKE) -C ./lib/zstd clean

.PHONY: all clean
