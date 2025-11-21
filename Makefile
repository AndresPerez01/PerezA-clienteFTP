CC = gcc
CFLAGS = -lpthread
TARGET = PerezA-clienteFTP
SOURCES = PerezA-clienteFTP.c connectsock.c connectTCP.c errexit.c

all: $(TARGET)

$(TARGET): $(SOURCES)
	$(CC) -o $(TARGET) $(SOURCES) $(CFLAGS)

clean:
	rm -f $(TARGET)

.PHONY: all clean
