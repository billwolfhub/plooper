# Project Name
TARGET = Plooper

# Sources
CPP_SOURCES = Plooper.cpp

# Library Locations (shared copies one level up, alongside other Daisy projects)
LIBDAISY_DIR = ../libDaisy
DAISYSP_DIR = ../DaisySP

# ReverbSc lives in DaisySP-LGPL
USE_DAISYSP_LGPL = 1

# Load through the Daisy bootloader (keeps the bootloader intact)
APP_TYPE = BOOT_SRAM

# Core location, and generic Makefile.
SYSTEM_FILES_DIR = $(LIBDAISY_DIR)/core
include $(SYSTEM_FILES_DIR)/Makefile
