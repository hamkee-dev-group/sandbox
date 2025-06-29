CC      := clang
CFLAGS  := -Wall -Wextra -pedantic -g -O2 -fstack-protector-strong
LDFLAGS := -lcap
TARGET  := sandbox
SRC     := sandbox.c

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

clean:
	rm -f $(TARGET)
