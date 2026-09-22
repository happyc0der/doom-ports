# ----------------------------
NAME        = DOOMCE
DESCRIPTION = "Doom CE raycaster"
COMPRESSED  = YES
ARCHIVED    = NO

CFLAGS   = -Wall -Wextra -O3
CXXFLAGS = -Wall -Wextra -O3
# ----------------------------
include $(shell cedev-config --makefile)
