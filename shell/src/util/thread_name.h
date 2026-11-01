#pragma once

#include <pthread.h>

// Names the calling thread; the Linux limit is 15 characters plus the terminator.
inline void nameThisThread(const char* name) { pthread_setname_np(pthread_self(), name); }
