pianomirror: pianomirror.c
ifdef USE_NATS
	gcc -D USE_NATS=1 pianomirror.c /usr/lib/x86_64-linux-gnu/libportmidi.so  nats/libnats_static.a -pthread  -o pianomirror
else
	gcc pianomirror.c /usr/lib/x86_64-linux-gnu/libportmidi.so  -o pianomirror
endif