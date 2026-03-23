# This file is part of the MicroPython project, http://micropython.org/
# The MIT License (MIT)
# Copyright (c) 2022-2023 Damien P. George

# Set the location of the top of the MicroPython repository.
MICROPYTHON_TOP = ../..

# The default MPY_LIB_DIR points at the micropython-lib submodule, but this
# example manifest only uses files already in the main repository.
override MPY_LIB_DIR = $(MICROPYTHON_TOP)

# Freeze the asyncio Python package so `import asyncio` works out of the box.
FROZEN_MANIFEST = manifest.py

# Include the main makefile fragment to build the MicroPython component.
include $(MICROPYTHON_TOP)/ports/embed/embed.mk
