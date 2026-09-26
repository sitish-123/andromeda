#pragma once

#include <cstddef>
#include <string>

// A compositor holds a file descriptor for every client connection, every wl_shm pool, every dmabuf plane and every
// explicit-sync fence it dups. The 1024 soft limit inherited from the session manager is small enough that a single
// client churning shm pools can exhaust it, after which eglDupNativeFenceFDANDROID fails on every frame and the session
// wedges. Raise the soft limit to the hard limit at startup, and put it back before exec'ing children so they keep the
// conventional 1024 (a large soft limit breaks select() and slows down anything that loops over the fd table).

void raiseFileDescriptorLimit();
void restoreFileDescriptorLimit();
