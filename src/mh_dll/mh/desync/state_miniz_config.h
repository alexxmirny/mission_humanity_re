//
// desync/state_miniz_config.h -- mp:D46: the one miniz configuration, shared by state_miniz.c (which
// compiles the vendored library) and by every C++ file that includes miniz.h. Only the deflate and
// inflate cores and the CRC-32 are wanted: no stdio, no clock, no zip archive, no zlib-style API.
//
#pragma once
#define MINIZ_NO_STDIO
#define MINIZ_NO_TIME
#define MINIZ_NO_ARCHIVE_APIS
#define MINIZ_NO_ZLIB_APIS
