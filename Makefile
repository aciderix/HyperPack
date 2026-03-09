CC = gcc
CFLAGS = -O2 -Wall
LDFLAGS = -lm -lpthread
SRC = src/hyperpack.c
BIN = hyperpack

all: $(BIN)

$(BIN): $(SRC)
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

clean:
	rm -f $(BIN) *.hpk

.PHONY: all clean
