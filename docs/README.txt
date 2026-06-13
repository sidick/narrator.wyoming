==============================================================================
                          narrator.wyoming
              neural-voice narrator.device via Piper / Wyoming
==============================================================================


WHAT THIS IS
------------

A drop-in replacement for the Amiga `narrator.device' that speaks with a
modern neural voice instead of the old Paula formant synthesizer. It
forwards text to a Piper TTS server over the Wyoming protocol (plain TCP)
and plays the returned PCM audio through AHI.

Because it replaces narrator.device by name in DEVS:, existing Amiga
software gets neural speech transparently -- the stock `Say' command and
any other program that opens narrator.device work with no modification.

The package contains two pieces:

  narrator.device     The drop-in device.  Goes in DEVS:.

  translator.library  A pass-through replacement for translator.library.
                      Goes in LIBS:.  The classic translator turns English
                      into ARPABET phonemes for the formant synth; Piper
                      wants English, so this one passes English through
                      unchanged.  Stock `Say' always calls translator.library,
                      so installing this is what makes English reach our
                      device.

Aimed at PiStorm / emu68k accelerated and other fast 68k Amigas, where
the accelerated CPU makes the network round-trip practical (~0.4s warm
time-to-first-audio over a local LAN).  Developed and tested under the
Amiberry emulator.


REQUIREMENTS
------------

Amiga side:

  * AHI v4 or later, with a Unit 0 audio mode configured.  The bundled
    `paula.audio' driver works under emulation; on real hardware any AHI
    output mode is fine.
  * A TCP/IP stack providing bsdsocket.library -- Roadshow, AmiTCP, or
    Amiberry's built-in bsdsocket.library emulation.
  * A fast 68k.  PiStorm / emu68k is the design target; ~68030 at 40 MHz
    upwards should also be usable.  No FPU is required.
  * No TLS / AmiSSL -- Wyoming is plain TCP.
  * OPTIONAL: codesets.library (Aminet: util/libs/codesets) for native-
    locale text input.  Without it, pure ASCII and pre-encoded UTF-8 work
    fine but ISO-8859-1 accents / umlauts go through raw and Piper may
    reject them.  See the CONFIGURATION section.

Server side:

  * A Piper TTS server reachable on your LAN with the Wyoming protocol
    enabled (default port 10200).  The official `wyoming-piper' container
    is the easiest way to get one running -- see the SERVER section below.


QUICK INSTALL (using the Install script)
----------------------------------------

From the unpacked drawer:

    Execute Install

The script copies narrator.device and translator.library into place, and
seeds ENVARC:narrator.wyoming from the example file if you do not already
have one.  Edit ENV:narrator.wyoming (and ENVARC:) to point at your Piper
server, then:

    Say "Hello from the Amiga."

If you hear neural speech, you are done.  If not, see TROUBLESHOOTING.


MANUAL INSTALL
--------------

If you prefer to install by hand:

    Copy narrator.device           DEVS:narrator.device
    Copy translator.library        LIBS:translator.library
    Copy narrator.wyoming.example  ENVARC:narrator.wyoming
    Copy ENVARC:narrator.wyoming   ENV:narrator.wyoming

Edit ENV:narrator.wyoming so `host' names your Piper server (see the
CONFIGURATION section).  If you change it later, copy it to ENVARC: too --
ENV: is in RAM and rebuilt from ENVARC: at every boot.


SERVER: running Piper / Wyoming (Docker)
----------------------------------------

narrator.wyoming needs a Piper TTS server speaking the Wyoming protocol
on your LAN.  The easiest way to get one is the official
`rhasspy/wyoming-piper' container.  Run it on any always-on machine on
your network (a NAS, a mini-PC, the host you build on, etc.):

    docker run -d --name wyoming-piper \
        -p 10200:10200 \
        -v wyoming-piper-data:/data \
        rhasspy/wyoming-piper \
        --voice en_US-lessac-medium

What the flags do:

    -p 10200:10200     Exposes the Wyoming TCP port the device connects to.
    -v ...:/data       Persists downloaded voice models across restarts
                       (so they are not re-fetched each time).
    --voice NAME       The startup voice.  Piper auto-downloads it on
                       first run.  The server listens on 0.0.0.0:10200.

Same thing as a `docker-compose.yml':

    services:
      wyoming-piper:
        image: rhasspy/wyoming-piper
        command: --voice en_US-lessac-medium
        ports:
          - "10200:10200"
        volumes:
          - wyoming-piper-data:/data
        restart: unless-stopped
    volumes:
      wyoming-piper-data:

A single wyoming-piper instance serves more than one voice.  Just name
the voice you want in the request -- if it is not local, Piper downloads
it the first time and caches it in /data.  So `voice', `voice_male', and
`voice_female' in your prefs can each point at a different voice against
the same server.  Browse the catalogue at:

    https://rhasspy.github.io/piper-samples/

Use the exact model name (e.g. en_US-ryan-high) in the prefs file.


CONFIGURATION (ENV:narrator.wyoming)
------------------------------------

The device reads ENV:narrator.wyoming at OpenDevice time.  Format is
`key value' per line; lines starting with `#' or `;' are comments, as
is anything after a whitespace + # / ; on the value side.

    host          Piper/Wyoming server address       (default 127.0.0.1)
    port          server port                        (default 10200)
    voice         default Piper voice name           (server default)
    voice_male    voice when caller selects MALE     (Say's default sex)
    voice_female  voice when caller selects FEMALE
    ahi_unit      ahi.device unit to play through    (default 0)
    gain          AHI output level, % of full scale  (default 80)
    smooth        high-cut strength, 0 = off         (default 2)
    split_words   break long input into ~N-word
                  pipelined chunks for faster start  (default 0 = off)

All keys are optional except `host'.

Notes on individual keys:

  voice / voice_male / voice_female
      Pick a clearly-articulating voice for voice_male -- it is the
      everyday voice, since `Say' defaults to the MALE sex.  Some voices
      clip the final consonant on short isolated words (en_US-ryan-high
      renders "test" as ~"tess"); en_US-lessac-medium is a safe default.

  gain
      Piper's PCM peaks reach 0 dBFS, and AHI's resample to the output
      rate overshoots full-scale peaks, which clips and sounds harsh on
      loud syllables.  Keeping `gain' below 100 leaves headroom so peaks
      do not hit the ceiling.  80 is a good default; lower it (70) if
      loud peaks still distort, raise it toward 100 for maximum loudness
      if your output stays clean.

  smooth
      AHI's resample plus 8-bit Paula output turn the 8-11 kHz "s" band
      sibilant.  `smooth' applies a gentle high-cut before AHI (a cascade
      of N (x+x_prev)/2 averagers) to tame it.  0 = off, 1 = gentle,
      2 = the validated default.  Raise it if "s" still hisses; drop it
      if speech sounds muffled.

  split_words
      Piper synthesizes one SENTENCE at a time, so a single long
      sentence has a long wait before any sound (about 2.3s for a 40-word
      run-on).  Set `split_words' to a word count (e.g. 12) to break the
      text into smaller chunks sent pipelined on the one connection --
      playback of the first chunk starts in about 0.25s while the server
      renders the rest.  Splits happen ONLY at punctuation (full stops
      always; commas / semicolons / colons once a chunk passes the word
      target), so seams land in the natural pauses Piper already inserts.

      CAVEAT: AHI's read-ahead is only two buffers (about 370 ms total at
      22050 Hz / 16-bit / mono).  If per-chunk Piper synthesis + network
      transfer takes longer than the lookahead drains, you'll hear a
      brief stutter or click at the chunk seam.  Under Amiberry's
      bsdsocket emulation this is observable on smaller chunk sizes; on
      real hardware with PiStorm and a TCP stack like Roadshow the
      headroom is larger.  If you hear boundary artefacts, raise the
      word target (fewer, longer chunks) or drop back to 0 (one
      request, slower start, but no seams).

      0 (or omitted) = off: send the whole text in one request (most
      natural prosody).

The IOSpeech fields `rate' and `pitch' are accepted but ignored -- the
Wyoming synthesize request has no per-request rate/pitch knob (those are
properties of the Piper voice, set server-side).  `volume' (0-64) and
`sex' are honoured (sex selects the configured voice).

Text codeset.  The Wyoming JSON sent to Piper is UTF-8 only.  The device
auto-detects the caller's text: valid UTF-8 (including pure ASCII) is
passed through, and anything else is transcoded from ISO-8859-1 to UTF-8
via the optional `codesets.library' (Aminet:util/libs/codesets) if it
is installed.  Without codesets.library, non-UTF-8 input is passed
through unchanged and Piper may reject it -- install codesets.library
to handle native-locale text (accented characters, umlauts, etc.).

The prefs are snapshotted when the device task starts.  Edits do not take
effect until the device is closed and reopened.  In practice each `Say'
invocation opens a fresh device, so live edits keep working; a custom
daemon that holds the device open across edits will keep using the
values it loaded at OpenDevice.


PERSISTING SETTINGS ACROSS REBOOTS
----------------------------------

ENV: lives in RAM and is rebuilt from ENVARC: at every boot, so editing
only ENV:narrator.wyoming lasts until the next reboot.  To make your
settings permanent, save the file to ENVARC: as well:

    Copy ENV:narrator.wyoming  ENVARC:narrator.wyoming

The Install script does this for you on first install (seeds ENVARC:
from the example).  After editing, run the Copy yourself.


TROUBLESHOOTING
---------------

Symptom: `Say "..."' says nothing, no error.
    The device may have been unable to reach the server.  Check that
    `host' in ENV:narrator.wyoming matches the IP of the machine running
    wyoming-piper, and that nothing on your network is blocking TCP
    port 10200.  An unreachable server fails fast (5 second timeout) and
    Say reports an error.

Symptom: First word audibly clipped on isolated short text.
    Some Piper voices (notably en_US-ryan-high) render very short words
    with a clipped final consonant.  Use a different voice -- e.g.
    en_US-lessac-medium articulates final consonants cleanly.

Symptom: Loud syllables sound harsh / distorted.
    Lower `gain' (try 70 or 65).

Symptom: "s" sounds hiss or sizzle.
    Raise `smooth' (try 3).  If speech then sounds muffled, drop it back
    or use 1.

Symptom: Long sentences have a long delay before any sound.
    Set `split_words 12' (or similar).  Multi-sentence text starts fast
    anyway -- splitting only helps a single long sentence.

Symptom: Nothing happens at all and the system hangs / Gurus.
    Confirm AHI v4+ is installed with a working Unit 0 mode, and that
    bsdsocket.library is available (Roadshow / AmiTCP loaded, or
    emulator bsdsocket emulation enabled).  See REQUIREMENTS.


COMPANION SOFTWARE
------------------

Anything that opens narrator.device benefits from the neural voice
automatically -- that is the whole point of replacing it by name.

speak-handler by Alexander Fritsch (Aminet: util/sys/speak-handler) is
a good companion: a from-scratch native replacement for the Commodore
SPEAK: handler that drives narrator.device + translator.library.  Mount
its SPEAK: and you can pipe text straight to neural speech:

    Type README.txt TO SPEAK:
    echo "Hello from the Amiga" >SPEAK:


LIMITATIONS
-----------

  * No `rate' or `pitch' control (Wyoming limitation -- those are voice
    properties set server-side, not per request).

  * Direct ARPABET phoneme input (the classic narrator contract,
    bypassing translator.library) is silently discarded -- Piper takes
    text, not phonemes.  Normal English, including all-caps words, is
    always spoken.

  * CMD_READ mouth-shapes and word / syllable sync are not implemented
    (Piper returns no phoneme timing).  CMD_READ returns ND_NoWrite, so
    talking-head software gets no animation but does not hang.

  * First request after the server has been idle is slow (~1.8s) while
    Piper loads the voice model; warm requests are fast.

  * Validated under Amiberry only; real PiStorm hardware is untested by
    the author.


SOURCE / SUPPORT
----------------

Full source, build instructions, and design notes:

    https://github.com/sidick/narrator.wyoming

Built with the Bebbo m68k-amigaos GCC cross-toolchain.  Issues / patches
welcome on the GitHub project.


LICENSE
-------

MIT -- see the LICENSE file in this drawer.

==============================================================================
