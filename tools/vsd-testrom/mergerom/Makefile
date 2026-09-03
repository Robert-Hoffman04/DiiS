# Thin wrapper over the stock BlocksDS rom_arm9 Makefile.
# Build inside skylyrac/blocksds:slim-latest (see ../build.sh):
#   make NAME=merge_normal DEFINES=""
#   make NAME=merge_det    DEFINES="-DMT_DETERMINISTIC"
#   make NAME=merge_frozen DEFINES="-DMT_DETERMINISTIC -DMT_FREEZE"
#   make NAME=merge_fblend DEFINES="-DMT_DETERMINISTIC -DMT_FRONTBLEND"
#   make NAME=merge_mbright DEFINES="-DMT_DETERMINISTIC -DMT_MASTERBRIGHT"
#   make NAME=merge_mbsplit DEFINES="-DMT_DETERMINISTIC -DMT_MASTERBRIGHT -DMT_MB_SPLIT"

export BLOCKSDS      ?= /opt/wonderful/thirdparty/blocksds/core
export BLOCKSDSEXT   ?= /opt/wonderful/thirdparty/blocksds/external

GAME_TITLE    := GX Merge Test
GAME_SUBTITLE := desmumewii harness
GAME_AUTHOR   := desmumewii

include $(BLOCKSDS)/sys/default_makefiles/rom_arm9/Makefile
