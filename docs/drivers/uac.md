# uac: USB Audio Class

Plays the console's mixed sound on a USB sound card or headset. UAC1 and UAC2, chosen from
the device's own descriptors, no list of products.

**Not in the default build: with its stream running, Aurora crashes.** Tested on a TI PCM2706
(08bb:2706, full speed, controller 3). The USB side works: about 14000 transfers, 4 packets late,
none bad. But Aurora dies 15-25 s after boot, at a different address each time (illegal instruction,
execute at 0x003A002C, read at 0), and only while the stream runs: registering for the sound without
a stream ran clean. Moving the stream's buffers from physical memory to the image changed nothing.
Cause open. To build it anyway: `make DRIVERS="... uac"`.

What it takes: a full-speed device on a companion (OHCI) controller with a stereo PCM output
at 48 kHz, 16, 24 or 32 bits, adaptive, synchronous or asynchronous with explicit feedback.

What it refuses:

- High-speed devices (controller 0 or 1): the kernel's isochronous path was only read for
  OHCI. `step` 4 in `xectl status`; the device keeps its USB slot until unplugged.
- Microsoft's vendor id: the kernel's own drivers keep those.
- A second card, or a second streaming interface of the same card: never matched.

How it works:

- `src/audio/capture.c` registers an XAudio MEC client (kernel 800, 802). The callback takes
  each mixed frame, 6 x 256 floats every 5.33 ms, and keeps the first two channels in a ring.
- `stream.c` queues 3 transfers of 4 packets (4 ms each). Each completion refills itself from
  the ring and queues itself again from the USB DPC.
- An asynchronous device's feedback endpoint sets the frames per packet (`pacer.h`).
- 16 failed transfers in a row stop the stream (`gave_up`) so completions cannot chain.

`audio_volume` in xedrivers.ini is 0-100, default 100. `tools/xectl.py audio` prints the counters.

Not done: microphone (XamVoice), the 4 ms + ring latency is not tuned, UAC2 clock selectors.
The isochronous request layout and status words come from usb-audio-x360's reading of retail
17559, not from a claim of our own.
