#pragma once
// Host-only MSVC adapter. Target ESP builds use the SDK's real POSIX layer.
#ifdef _WIN32
#include <io.h>
#include <cstdlib>
#include <fcntl.h>
#include <sys/stat.h>
#include <cstdint>
using ssize_t = std::intptr_t;
inline int fsync(int fd) { return _commit(fd); }
inline int lstat(const char* path, struct stat* result) { return stat(path, result); }
#ifndef S_ISREG
#define S_ISREG(mode) (((mode) & _S_IFMT) == _S_IFREG)
#endif
#ifndef S_ISDIR
#define S_ISDIR(mode) (((mode) & _S_IFMT) == _S_IFDIR)
#endif
// RecordStore reads with open/read and writes fdopen(...,"wb"). A Windows text
// descriptor would translate CRLF or stop at 0x1a, invalidating byte-level tests.
struct PlusHostBinaryMode { PlusHostBinaryMode() { _set_fmode(_O_BINARY); } };
inline PlusHostBinaryMode plus_host_binary_mode;
#endif

