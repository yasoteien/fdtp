// This file is part of FDTP (File & Directory Tool for Pomera).
// SPDX-FileCopyrightText: 2026 yasoteien
// SPDX-License-Identifier: AGPL-3.0-or-later
#ifndef FDTP_EXPAT_COMPAT_H
#define FDTP_EXPAT_COMPAT_H

#include <expat.h>
#include <stdint.h>

/* The fixed-width location API was added in Expat 2.9.0. Older headers
 * (including the macOS SDK) expose only the original API. A backport can
 * opt in with -DFDTP_EXPAT_LOCATION64=1 when its headers/library provide it. */
#ifndef FDTP_EXPAT_LOCATION64
#define FDTP_EXPAT_LOCATION64 (XML_MAJOR_VERSION > 2 || \
    (XML_MAJOR_VERSION == 2 && XML_MINOR_VERSION >= 9))
#endif

static inline int64_t fdtp_xml_byte_index(XML_Parser parser)
{
#if FDTP_EXPAT_LOCATION64
    return XML_GetCurrentByteIndex64(parser);
#else
    return (int64_t)XML_GetCurrentByteIndex(parser);
#endif
}

static inline uint64_t fdtp_xml_byte_count(XML_Parser parser)
{
#if FDTP_EXPAT_LOCATION64
    return XML_GetCurrentByteCount64(parser);
#else
    return (uint64_t)XML_GetCurrentByteCount(parser);
#endif
}

static inline uint64_t fdtp_xml_line_number(XML_Parser parser)
{
#if FDTP_EXPAT_LOCATION64
    return XML_GetCurrentLineNumber64(parser);
#else
    return (uint64_t)XML_GetCurrentLineNumber(parser);
#endif
}

#endif
