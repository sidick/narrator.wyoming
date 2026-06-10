# version.mk -- the one place the narrator.wyoming version lives.
#
# The Amiga version IS <VERSION>.<REVISION>: VERSION is the major (the value
# OpenLibrary/OpenDevice compatibility checks read), REVISION the minor.
#   * VERSION  -- compatibility identity. Bump ONLY on an incompatible change,
#                 and never below 44 (it sits above every stock narrator/
#                 translator version so this pair is easy to tell apart).
#   * REVISION -- the minor. The working tree holds the revision you're building
#                 toward; `make release` tags it as-is (ships what you tested),
#                 and `make bump` (44.0 -> 44.1 -> ...) is run AFTER a release to
#                 open the next test cycle -- not as part of an ordinary commit.
#
# The Makefile feeds these to the device + library as -DNW_VERSION/-DNW_REVISION,
# and they derive both the lib_Version/lib_Revision consts and the $VER string
# from them -- so the number lives here and nowhere else.
VERSION  := 44
REVISION := 0
