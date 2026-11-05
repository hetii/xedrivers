# uac: USB Audio Class

Plays the console's mixed sound on a USB sound card or headset. UAC1 and UAC2, chosen from
the device's own descriptors, no list of products.

**Not in the default build: it freezes the console.** With a CM108 (0d8c:013c, full speed,
controller 3) in the console, the whole console stops on or soon after the first control
transfer on the device's default endpoint; XBDM goes, the power button does not answer. Claiming
the device without any transfer is fine, and a build without uac boots with the card in. Ruled out
so far: taking two interfaces, a delay of 5 s before the transfer, endpoint 0's max packet (8).
Still open. Local branch `uac-wip` has the probes. To build it anyway: `make DRIVERS="... uac"`.

What it takes: a full-speed device on a companion (OHCI) controller with a stereo PCM output
at 48 kHz, 16, 24 or 32 bits, adaptive, synchronous or asynchronous with explicit feedback.

What it refuses:

- High-speed devices (controller 0 or 1): the kernel's isochronous path was only read for
  OHCI. `step` 4 in `xectl status`; the device keeps its USB slot until unplugged.
- Microsoft's vendor id: the kernel's own drivers keep those.
- A second card: `step` 5. The microphone's interface of the same card lands here too.

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
