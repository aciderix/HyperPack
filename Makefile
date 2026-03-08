CC = gcc
CFLAGS = -O2 -Wall
LDFLAGS = -lm -lpthread

TARGET = hyperpack
SRC = src/hyperpack.c

.PHONY: all clean test

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

clean:
	rm -f $(TARGET)

test: $(TARGET)
	@echo "Testing compression round-trip..."
	@echo "The quick brown fox jumps over the lazy dog" > /tmp/hp_test_input.txt
	@./$(TARGET) c /tmp/hp_test_input.txt /tmp/hp_test.hp
	@./$(TARGET) d /tmp/hp_test.hp /tmp/hp_test_output.txt
	@diff /tmp/hp_test_input.txt /tmp/hp_test_output.txt && echo "✅ Round-trip OK" || echo "❌ FAILED"
	@rm -f /tmp/hp_test_input.txt /tmp/hp_test.hp /tmp/hp_test_output.txt
