EXE := fat.out

ifdef FAT64_DSYM
	DSFLAGS_EXTRAS=-fat64
endif

include Makefile.rules

all: fat.out

fat.out: fat.arm64.out fat.x86_64.out
	lipo -fat64 -create -o $@ $^

fat.x86_64.out: fat.x86_64.o
	$(CC) -isysroot $(SDKROOT) -target x86_64-apple-macosx10.9 -o $@ $<

fat.arm64.out: fat.arm64.o
	$(CC) -isysroot $(SDKROOT) -target arm64-apple-macosx10.9 -o $@ $<

fat.x86_64.o: main.c
	$(CC) -isysroot $(SDKROOT) -g -O0 -target x86_64-apple-macosx11 -c -o $@ $<

fat.arm64.o: main.c
	$(CC) -isysroot $(SDKROOT) -g -O0 -target arm64-apple-macosx11 -c -o $@ $<
