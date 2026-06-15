# SPDX-FileCopyrightText: Copyright (c) 2025-2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: GPL-2.0-or-later

noinst_LTLIBRARIES += %D%/libcswp.la

%C%_libcswp_la_SOURCES = \
	%D%/cswp.c \
	%D%/cswp.h

# Add CSWP include directory to search path
%C%_libcswp_la_CPPFLAGS = $(AM_CPPFLAGS) -I$(srcdir)/%D% $(CSWP_CFLAGS)

contrib_cswpdir="$(pkgdatadir)/contrib/cswp"
contrib_cswp_DATA=$(CSWP_SO_LIBS)
