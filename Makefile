CC := clang
AR := llvm-ar
CFLAGS  := -Wall -Wextra -Wpedantic -Werror -O2 -g -std=c11 -fPIC -flto \
	-fsanitize=address,undefined -fstack-protector-all -D_FORTIFY_SOURCE=3 \
	-ffunction-sections -fdata-sections -march=native -mtune=native \
	-I quicksand/include -g -O0
LDFLAGS := -flto -fsanitize=address,undefined
ARCH:=x86_64

all: build/libquicksand.so build/libquicksand.a

format:
	clang-format -style=file -i $$(find . | grep -e '\.c$$' -e '\.h$$')

build/libquicksand.so: build/quicksand_now.o build/quicksand_time.o build/quicksand.o
	$(CC) -shared -o build/libquicksand.so \
		build/quicksand_now.o \
		build/quicksand_time.o \
		build/quicksand.o \
		$(LDFLAGS)


build/libquicksand.a: build/quicksand_now.o build/quicksand_time.o build/quicksand.o
	$(AR) rcs build/libquicksand.a \
		build/quicksand_now.o \
		build/quicksand_time.o \
		build/quicksand.o

build/quicksand_now.o: quicksand/src/timestamp+$(ARCH).s
	mkdir -p build
	$(CC) -c -o build/quicksand_now.o -fPIC quicksand/src/timestamp+$(ARCH).s

build/quicksand_time.o: quicksand/src/time.c
	mkdir -p build
	$(CC) -c -o build/quicksand_time.o $(CFLAGS) quicksand/src/time.c

build/quicksand.o: quicksand/src/quicksand.c
	mkdir -p build
	$(CC) -c -o build/quicksand.o $(CFLAGS) quicksand/src/quicksand.c


### TESTS ###

build/test/basic: build/libquicksand.a test/test_basic.c
	mkdir -p build/test
	$(CC) -o build/test/basic test/test_basic.c $(CFLAGS) \
		build/libquicksand.a # (shared:) -L build -lquicksand

build/test/time: build/libquicksand.a test/test_time.c
	mkdir -p build/test
	$(CC) -o build/test/time test/test_time.c $(CFLAGS) \
		build/libquicksand.a

build/test/pub: build/libquicksand.a test/test_pub.c
	mkdir -p build/test
	$(CC) -o build/test/pub test/test_pub.c $(CFLAGS) \
		build/libquicksand.a

build/test/sub: build/libquicksand.a test/test_sub.c
	mkdir -p build/test
	$(CC) -o build/test/sub test/test_sub.c $(CFLAGS) \
		build/libquicksand.a

check: build/test/basic build/test/time
	./build/test/time
	./build/test/basic

compile_commands.json: Makefile
	@echo '[\n' \
	'{"directory":"$(PWD)","command":"$(CC) -c -fPIC quicksand/src/timestamp+$(ARCH).s -o build/quicksand_now.o","file":"quicksand/src/timestamp+$(ARCH).s"},\n' \
	'{"directory":"$(PWD)","command":"$(CC) -c $(CFLAGS) quicksand/src/time.c -o build/quicksand_time.o","file":"quicksand/src/time.c"},\n' \
	'{"directory":"$(PWD)","command":"$(CC) -c $(CFLAGS) quicksand/src/quicksand.c -o build/quicksand.o","file":"quicksand/src/quicksand.c"},\n' \
	'{"directory":"$(PWD)","command":"$(CC) -shared -o build/libquicksand.so build/quicksand_now.o build/quicksand_time.o build/quicksand.o $(LDFLAGS)","file":"build/libquicksand.so"},\n' \
	'{"directory":"$(PWD)","command":"$(AR) rcs build/libquicksand.a build/quicksand_now.o build/quicksand_time.o build/quicksand.o","file":"build/libquicksand.a"},\n' \
	'{"directory":"$(PWD)","command":"$(CC) -o build/test/basic test/test_basic.c $(CFLAGS) build/libquicksand.a","file":"test/test_basic.c"},\n' \
	'{"directory":"$(PWD)","command":"$(CC) -o build/test/time test/test_time.c $(CFLAGS) build/libquicksand.a","file":"test/test_time.c"},\n' \
	'{"directory":"$(PWD)","command":"$(CC) -o build/test/pub test/test_pub.c $(CFLAGS) build/libquicksand.a","file":"test/test_pub.c"},\n' \
	'{"directory":"$(PWD)","command":"$(CC) -o build/test/sub test/test_sub.c $(CFLAGS) build/libquicksand.a","file":"test/test_sub.c"}\n]' \
	> $@

.PHONY: format all check
