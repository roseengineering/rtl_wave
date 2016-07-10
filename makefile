
CFLAGS?=-O2 -g -Wall
LDLIBS+=-lrtlsdr
CC?=gcc
PROGNAME=rtl_wave

all: $(PROGNAME)

%.o: %.c
	$(CC) $(CFLAGS) -c $<

$(PROGNAME): $(PROGNAME).o convenience.c  
	$(CC) -g -o $@ $^ $(LDFLAGS) $(LDLIBS)

clean:
	rm -f *.o $(PROGNAME)

