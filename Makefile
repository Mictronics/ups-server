CC = gcc
CPPFLAGS += -D_GNU_SOURCE

DIALECT = -std=c18
CFLAGS += $(DIALECT) -od -g -W -D_DEFAULT_SOURCE -Wall -fno-common -Wmissing-declarations
LIBS = -lpthread -lwebsockets -lm -lconfig -ljson-c
LDFLAGS =

all: ups-server

%.o: server/%.c server/*.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

ups-server: server/ups-server.o server/bicker.o
	$(CC) -g -o server/$@ $^ $(LDFLAGS) $(LIBS)

clean:
	rm -f server/*.o server/ups-server
