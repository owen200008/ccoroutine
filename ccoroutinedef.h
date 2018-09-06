#pragma once

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <assert.h>
#include <cstring>

#ifdef __GNUC__
#include <pthread.h>
#include <emmintrin.h>
#include <thread>
#define CCTHREADID_DWORD    pthread_t
#define CCORUTINETLS_Key	pthread_key_t
#define CCoroutineLikely(x) __builtin_expect((x), true)
#define CCoroutineUnLikely(x) __builtin_expect((x), false)
#elif defined(_MSC_VER)
#include <Windows.h>
#define CCTHREADID_DWORD    DWORD
#define CCORUTINETLS_Key	DWORD
#define CCoroutineLikely(x) x
#define CCoroutineUnLikely(x) x
#endif

#ifndef ccsnprintf
#ifdef __GNUC__
#define ccsnprintf snprintf
#else
#define ccsnprintf sprintf_s
#endif
#endif

enum CCoroutineLogStatus {
    CCoroutineLogStatus_Info = 0,
    CCoroutineLogStatus_Error = 1,
};



