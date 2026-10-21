#pragma once
// newlib's <sys/unistd.h> declares the POSIX file calls arduino's vfs_api uses
// (unlink, rmdir, fsync); a host libc keeps them in <unistd.h>.
#include_next <sys/unistd.h>
#include <unistd.h>
