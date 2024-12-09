CC = gcc-10
CFLAGS = -Wall -g -pthread

TARGET = m6

SRC = m6.c 

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $(TARGET) $(SRC) -lm

clean:
	rm -f $(TARGET)

.PHONY: all clean