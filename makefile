pianomirror: pianomirror.c
ifdef USE_NATS
	gcc  -pthread -g -D USE_NATS=1 pianomirror.c metronome.c /usr/lib/x86_64-linux-gnu/libportmidi.so nats/libnats_static.a -pthread -llua5.3 -o pianomirror
else
	gcc  -pthread -g pianomirror.c metronome.c /usr/lib/x86_64-linux-gnu/libportmidi.so -pthread -ldl -lm -llua5.3 -o pianomirror
endif