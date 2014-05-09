# Variables:
PROGRAM = sym/SerialKDPProxy
OBJECTS = obj/SerialKDPProxy.o obj/kdp_serial.o

CFLAGS = -Wall -g

# Phony rules:
all: $(PROGRAM)

clean:
	rm -f $(PROGRAM) $(OBJECTS)

# Programs to build:
$(PROGRAM): $(OBJECTS)
	$(CC) -o $(PROGRAM) $(OBJECTS)

# Objects to build:
obj/SerialKDPProxy.o: src/SerialKDPProxy.c
	$(COMPILE.c) $(OUTPUT_OPTION) $<

obj/kdp_serial.o: src/kdp_serial.c
	$(COMPILE.c) $(OUTPUT_OPTION) $<


