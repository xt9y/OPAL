#include <opal/low_latency_thread.hpp>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__APPLE__)
#include <pthread.h>
#endif

namespace opal {

void prioritize_low_latency_thread() noexcept
{
#if defined(_WIN32)
    HANDLE thread = GetCurrentThread();
    (void)SetThreadPriority(thread, THREAD_PRIORITY_HIGHEST);
    (void)SetThreadPriorityBoost(thread, FALSE);
#elif defined(__APPLE__)
    (void)pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#endif
}

}
