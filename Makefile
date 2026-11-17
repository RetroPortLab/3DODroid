# 3DODroid desktop build
CC      ?= gcc
CFLAGS  ?= -O2 -Wall -Wno-unused
CHDR    := third_party/libchdr
CHDR_CF := -I$(CHDR)/include -I$(CHDR)/deps/lzma-26.02/include -I$(CHDR)/deps/miniz-3.1.2 -I$(CHDR)/deps/zstd-1.5.7 -DZ7_ST
CORE    := $(wildcard core/*.c)

all: desktop/run3do tools/disctest/disctest

build/chdr.o: $(CHDR)/unity.c | build
	$(CC) -O2 -w $(CHDR_CF) -c $< -o $@
build/chdr_av.o: $(CHDR)/src/libchdr_codec_avhuff.c | build
	$(CC) -O2 -w $(CHDR_CF) -c $< -o $@
build:
	mkdir -p build

desktop/run3do: desktop/run3do.c $(CORE) core/*.h build/chdr.o build/chdr_av.o
	$(CC) $(CFLAGS) -I$(CHDR)/include -o $@ desktop/run3do.c $(CORE) build/chdr.o build/chdr_av.o -lm

tools/disctest/disctest: tools/disctest/disctest.c core/disc.c core/disc.h build/chdr.o build/chdr_av.o
	$(CC) $(CFLAGS) -I$(CHDR)/include -o $@ tools/disctest/disctest.c core/disc.c build/chdr.o build/chdr_av.o -lm

test: tools/disctest/disctest
	python3 tools/disctest/make_test_disc.py build/testdisc
	cd build/testdisc && ../../tools/disctest/disctest test.iso test.cue $$(test -f test.chd && echo test.chd)

clean:
	rm -rf build desktop/run3do tools/disctest/disctest

.PHONY: all test clean
