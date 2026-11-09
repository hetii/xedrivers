# uac: USB Audio Class

Plays the console's mixed sound on a USB sound card or headset. UAC1 and UAC2, chosen from
the device's own descriptors, no list of products.

Tested on a TI PCM2706 (08bb:2706, full speed, controller 3): sound heard in headphones during the
boot intro; Aurora up on 5 of 5 boots; two hours with network traffic beside it, 3.7 million packets.
The stream came back by itself after each of 3 device resets, and through cards swapped by hand.
Listened to in games on the PCM2706 and two other DACs: like HDMI, voice included, since the 5.1 mix.

What it takes: a full-speed device on a companion (OHCI) controller or a high-speed one on EHCI, with a stereo
or 5.1 PCM output at 48 kHz, 16, 24 or 32 bits (the most bits the card has that fit the speed), adaptive, synchronous or asynchronous with explicit feedback.

What it refuses:

- A full-speed device behind a high-speed hub: it would need split transactions, not read.
  `step` 4.
- Microsoft's vendor id: the kernel's own drivers keep those.
- A second card, or a second streaming interface of the same card: never matched.

How it works:

- `src/audio/capture.c` registers an XAudio MEC client (kernel 800, 802). The callback takes
  each mixed frame, 6 x 256 floats every 5.33 ms, and keeps it in a ring as the card is to get it
  (stereo mix or six channels, see Channels).
- `stream.c` queues 3 transfers of 4 ms each (4 packets at full speed, 32 at high speed). Each completion refills itself from
  the ring and queues itself again from the USB DPC.
- An asynchronous device's feedback endpoint sets the frames per packet (`pacer.h`).
- 16 failed transfers in a row stop the stream (`gave_up`) so completions cannot chain.

`audio_volume` in xedrivers.ini is 0-100, default 50 (-6 dB). `tools/xectl.py audio` prints the counters.

## High speed

A high-speed device gets 32 packets a transfer, one a microframe (4 ms), of which every
2^(bInterval-1)-th carries data. The kernel's EHCI side (17559, 0x800df798) fills one microframe
a packet from the next free one; an empty packet is an inactive transaction, which completion
(0x800ddbf8) passes. The endpoint is opened with interval 1 whatever the descriptor says:
opened with 4, the iTDs of the frames in between were never run.

Tested with the Orange Pi as a UAC2 card (configfs `uac2`, with the `Usb:` medium in the same
device), test tone from bench=debug (`audio_test_tone`): every microframe and every 8th, the
channels where they belong, 30 s without a lost sample at bInterval 4. At bInterval 1 the Pi's
own capture lost whole packets every 2.2 s while the console sent them without a gap, which is
taken as the Pi's controller (musb, 8000 interrupts a second). The Pi cannot be a 5.1 card: its
controller's FIFO does not take the 588-byte full-speed packet the gadget declares.

## Channels

A stereo card gets the console's 5.1 mixed down (ITU-R BS.775: centre and surrounds at -3 dB
into both sides, LFE left out), as the console does for HDMI. A card with a six-channel output
gets the six channels when the dashboard's audio setting is Dolby Digital 5.1 or Dolby Digital
with WMA Pro (`XAudioGetSpeakerConfig`, kernel 511: 0x00000003 digital stereo, 0x00010001
Dolby Digital, 0x00030001 with WMA Pro), else the stereo mix in its front pair. The setting is
read about every 3 s; a change resets the card, which then chooses again.

## Microphone

A card's microphone (`mic.c`) is captured beside its output in its best setting, all channels
averaged at 24-bit scale, then brought to 16 kHz mono through a 63-tap low-pass (pcm.c:
-1 dB at 6 kHz, under -70 dB from 9), the chat's fixed format, and offered to the console's voice chat (`src/audio/voice.c`, after
usb-audio-x360): XamVoiceHeadsetPresent, XamVoiceSubmitPacket and XamVoiceGetBatteryStatus are
entered through ours first (`src/core/detour.c`), the title's microphone requests are answered
from the capture as 16 kHz PCM, and player 0's voice route is pointed at a virtual headset
through xam's binding routine (0x816D9060 on 17559, checked word for word), the old route put
back when the microphone goes. A profile has to be signed in on player 0.

Tested with a CM108's output looped into its microphone input and bench=debug's test tone:
`xectl.py cmd 26` makes a microphone request through the real XamVoiceSubmitPacket, and it came
back answered, 640 bytes of 16 kHz PCM carrying the tone's 1, 3 and 5.3 kHz. Not yet heard in a
game's chat.

## Issues

- Pulling the card out by hand is untested; the same path ran on 3 device resets.
- An asynchronous card (feedback endpoint) and UAC2 are written and tested on the host only.
- **The six-channel output is tested on the host only.** No 5.1 card was at hand, and the Orange Pi
  cannot act as one (see High speed).
- **A high-speed asynchronous card** (feedback every microframe) is written, not run.
- At boot the speaker setting reads 0x00000003 before the dashboard applies its own; a 5.1 card
  is corrected by the 3 s check, a stereo card does not care.

Not done: the 4 ms + ring latency is not tuned, UAC2 clock selectors.
The isochronous request layout and status words come from usb-audio-x360's reading of retail
17559, not from a claim of our own.
