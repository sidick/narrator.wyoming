; narrator.wyoming -- on-target test runner
;
; AmigaDOS script, auto-run from the system drive's S:User-Startup via
;   If Exists Narrator:boot / Execute Narrator:boot
; (Narrator: == NR0: == this repo).
;
; Reads server settings from Narrator:config/narrator.wyoming (libnix does not hand a usable
; argv to a command launched from the startup-sequence, so the test programs take
; their parameters from a config file rather than the command line). The
; standalone test programs write a log read back from the host side.
;
; Networking is bsdsocket.library mapped straight to a host OS socket, no
; guest TCP/IP stack involved: Copperline's [hostsocket] board in
; net = "host" mode (see config/copperline-narrator.toml.example), or
; Amiberry's equivalent bsdsocket_emu=true. Both boot this same script.

CD Narrator:
; The AmigaDOS default 4KB CLI stack is too small (these programs use several KB
; of locals + bsdsocket/AHI need headroom); too small -> corruption -> Guru.
Stack 131072

; Install runtime prefs (server address + voice mapping) where the device reads
; them. The file uses "key value" lines: host / port / voice / voice_male /
; voice_female. Nothing is baked into the binary.
Copy >NIL: Narrator:config/narrator.wyoming ENV:narrator.wyoming
Copy >NIL: Narrator:build/amiga/narrator.device   DEVS:narrator.device
Copy >NIL: Narrator:build/amiga/translator.library LIBS:translator.library

; --- Say acceptance test (the drop-in goal: stock Say -> our device) ---
Echo "narrator.wyoming Say test starting"
SYS:Utilities/Say "Hello world. This is the Amiga, speaking with a neural voice over Wyoming and A.H.I."
IF WARN
  Echo "*** Say returned WARN/ERROR ***"
  Why
EndIF
Echo "narrator.wyoming Say test done. (window held open 30s for inspection)"
Wait 30

; --- Direct device test (uncomment to run instead) ---
; build/amiga/devtest >Narrator:devtest.log
; Wait 30

; --- Standalone test programs (uncomment to run instead) ---
; build/amiga/saytest     >Narrator:saytest.log
; build/amiga/wyomingtest >Narrator:wyomingtest.log
; build/amiga/failtest    >Narrator:failtest.log    ; graceful-failure scenarios
