CC ?= cc
CFLAGS ?= -std=c11 -Wall -Wextra -Werror -pedantic
CPPFLAGS += -Iinclude -D_POSIX_C_SOURCE=200809L
LDFLAGS ?=

SRC := $(wildcard src/*.c src/*/*.c)
OBJ := $(SRC:.c=.o)
CORE_SRC := $(filter-out src/main.c,$(SRC))
TEST_SRC := $(wildcard tests/*.c)
TEST_BIN := build/phantom_tests
BIN := build/phantom

# Windows builds. Cross-compile from macOS/Linux with either:
#   make gui WINCC="python3 -m ziglang cc -target x86_64-windows-gnu"
#   make gui WINCC=x86_64-w64-mingw32-gcc
# or compile the same sources with MSVC/clang-cl on Windows.
WINCC ?= x86_64-w64-mingw32-gcc
GUI_BIN := build/PhantomC.exe
CLI_WIN_BIN := build/phantom-cli.exe
GUI_SRC := $(wildcard gui/*.c)
GUI_LIBS := -lcomctl32 -lgdi32 -luser32 -lshell32 -ladvapi32 -lsrclient -lcomdlg32 -ldwmapi -luxtheme -lpsapi

.PHONY: all test gui release clean

all: $(BIN)

build:
	mkdir -p build

%.o: %.c
	$(CC) $(CFLAGS) $(CPPFLAGS) -c -o $@ $<

$(BIN): build $(OBJ)
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ $(OBJ) $(LDFLAGS)

$(TEST_BIN): build $(filter-out src/main.o,$(OBJ)) $(TEST_SRC)
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ $(filter-out src/main.o,$(OBJ)) $(TEST_SRC) $(LDFLAGS)

test: $(TEST_BIN)
	./$(TEST_BIN)

gui: build
	$(WINCC) -std=c11 -O2 -Wall -Wextra -Iinclude -municode \
	  -Wl,--subsystem,windows -o $(GUI_BIN) $(GUI_SRC) $(CORE_SRC) $(GUI_LIBS)
	$(WINCC) -std=c11 -O2 -Wall -Wextra -Iinclude \
	  -o $(CLI_WIN_BIN) $(SRC) -lsrclient

# Assemble a ready-to-zip Windows release: GUI, CLI, catalogs, sample config.
release: gui
	mkdir -p build/release/Data
	cp -f $(GUI_BIN) $(CLI_WIN_BIN) build/release/
	cp -f Data/*.json build/release/Data/
	cp -f sample-config.json README.md build/release/

clean:
	rm -rf build $(OBJ)
