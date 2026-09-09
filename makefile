NAME = VIDPLAY
ICON = icon.png
DESCRIPTION = "TI-84 CE Video Player"
COMPRESSED = YES
ARCHIVED = YES

CFLAGS = -Wall -Wextra -Oz
CXXFLAGS = -Wall -Wextra -Oz

include $(shell cedev-config --makefile)
