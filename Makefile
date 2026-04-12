CC      ?= clang
CFLAGS  := -Wall -Wextra -pedantic -g -O2 -fstack-protector-strong
LDFLAGS := -lcap
TARGET  := sandbox
SRC     := sandbox.c

.PHONY: all clean preflight

all: preflight $(TARGET)

preflight:
	@command -v $(CC) >/dev/null 2>&1 || { echo "Error: compiler '$(CC)' not found. Install it or set CC to an available compiler."; exit 1; }
	@echo '#include <sys/capability.h>' | $(CC) -fsyntax-only -x c - 2>/dev/null || { echo "Error: <sys/capability.h> not found. Install libcap development headers (e.g. libcap-dev)."; exit 1; }
	@echo 'int main(void){return 0;}' | $(CC) -x c - -lcap -o /dev/null 2>/dev/null || { echo "Error: cannot link with -lcap. Install libcap (e.g. libcap-dev)."; exit 1; }

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

clean:
	rm -f $(TARGET)
