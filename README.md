# KundaliniPianoMirrorLinux

ANSI C style code for the Kundalini Piano Mirror, based on PortMidi.

This software is designed to create a mirror-image keyboard, and to do other real-time MIDI remapping.s

# Raspberri PI Note

This code must be linked against Portmidi, which I accomplished like this with my Raspberri PI based system:

    gcc pianomirror.c  /usr/lib/arm-linux-gnueabihf/libportmidi.so -o pianomirror

# About
s
- More information: https://www.kundalinisoftware.com/kundalini-piano-mirror/

# Author

Benjamin Pritchard

- https://benjaminpritchard.org
- https://kundalinisoftware.com
