/*************************************************************************
 * Copyright (c) 2019-2021, NVIDIA CORPORATION. All rights reserved.
 * Modifications Copyright (c) 2019-2021 Advanced Micro Devices, Inc. All rights reserved.
 *
 * See LICENSE.txt for license information
 ************************************************************************/

#ifndef NCCL_CHECKS_H_
#define NCCL_CHECKS_H_

#include "debug.h"

// Check CUDA calls
#define CUDACHECK(cmd) do {                                 \
    hipError_t err = cmd;                                    \
    if( err != hipSuccess ) {                                \
        WARN("HIP failure '%s'", hipGetErrorString(err));   \
        return ncclUnhandledCudaError;                      \
    }                                                       \
} while(false)

#define CUDACHECKGOTO(cmd, res, label) do {                 \
    hipError_t err = cmd;                                    \
    if( err != hipSuccess ) {                                \
        WARN("HIP failure '%s'", hipGetErrorString(err));   \
        res = ncclUnhandledCudaError;                       \
        goto label;                                         \
    }                                                       \
} while(false)

// Report failure but clear error and continue
#define CUDACHECKIGNORE(cmd) do {  \
    hipError_t err = cmd;         \
    if( err != hipSuccess ) {     \
        INFO(NCCL_ALL,"%s:%d Cuda failure '%s'", __FILE__, __LINE__, hipGetErrorString(err)); \
        (void) hipGetLastError(); \
    }                              \
} while(false)

#include <errno.h>
// Check system calls
#define SYSCHECK(call, name) do { \
  int retval; \
  SYSCHECKVAL(call, name, retval); \
} while (false)

#define SYSCHECKVAL(call, name, retval) do { \
  SYSCHECKSYNC(call, name, retval); \
  if (retval == -1) { \
    WARN("Call to " name " failed : %s", strerror(errno)); \
    return ncclSystemError; \
  } \
} while (false)

#define SYSCHECKSYNC(call, name, retval) do { \
  retval = call; \
  if (retval == -1 && (errno == EINTR || errno == EWOULDBLOCK || errno == EAGAIN)) { \
    INFO(NCCL_ALL,"Call to " name " returned %s, retrying", strerror(errno)); \
  } else { \
    break; \
  } \
} while(true)

// Propagate errors up
#define NCCLCHECK(call) do { \
  ncclResult_t res = call; \
  if (res != ncclSuccess) { \
    /* Print the back trace*/ \
    if (ncclDebugNoWarn == 0) INFO(NCCL_ALL,"%s:%d -> %d", __FILE__, __LINE__, res);    \
    return res; \
  } \
} while (0);

#define NCCLCHECKGOTO(call, res, label) do { \
  res = call; \
  if (res != ncclSuccess) { \
    /* Print the back trace*/ \
    if (ncclDebugNoWarn == 0) INFO(NCCL_ALL,"%s:%d -> %d", __FILE__, __LINE__, res);    \
    goto label; \
  } \
} while (0);

#endif
