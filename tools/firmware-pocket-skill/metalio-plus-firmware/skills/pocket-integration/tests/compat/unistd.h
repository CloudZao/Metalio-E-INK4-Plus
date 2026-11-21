#pragma once
#ifdef _WIN32
#include <io.h>
#include <cstdlib>
#include <fcntl.h>
#include <sys/stat.h>
#include <cstdint>
using ssize_t=std::intptr_t;
inline int fsync(int fd){return _commit(fd);}
inline int lstat(const char* path,struct stat* result){return stat(path,result);}
#ifndef S_ISREG
#define S_ISREG(mode) (((mode)&_S_IFMT)==_S_IFREG)
#endif
#ifndef S_ISDIR
#define S_ISDIR(mode) (((mode)&_S_IFMT)==_S_IFDIR)
#endif
struct HostBinaryMode{HostBinaryMode(){_set_fmode(_O_BINARY);}};
inline HostBinaryMode host_binary_mode;
#endif
