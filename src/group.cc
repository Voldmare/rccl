/*************************************************************************
 * Copyright (c) 2015-2021, NVIDIA CORPORATION. All rights reserved.
 * Modifications Copyright (c) 2019-2021 Advanced Micro Devices, Inc. All rights reserved.
 *
 * See LICENSE.txt for license information
 ************************************************************************/

#include "group.h"
#include "debug.h"
#include "enqueue.h"
#include "transport.h"
#include <unistd.h>

#define MAX_ASYNC_OPS 128
thread_local pthread_t ncclGroupThreads[MAX_ASYNC_OPS];
thread_local int ncclGroupIndex = 0;
thread_local int ncclGroupMode = 0;
thread_local ncclResult_t ncclGroupError = ncclSuccess;
extern struct allocationTracker allocTracker[];

bool ncclAsyncMode() {
  return ncclGroupMode > 0;
}

ncclResult_t ncclAsyncErrCheck(ncclResult_t ret) {
  if (ncclGroupError == ncclSuccess || ret != ncclSuccess) ncclGroupError = ret;
  return ret;
}

struct ncclInitArgs {
  ncclInitFunc_t func;
  int cudaDev;
  ncclComm_t* newcomm;
  int ndev;
  ncclUniqueId commId;
  int myrank;
};
struct ncclCollArgs {
  ncclComm_t comm;
  uint16_t connIndex;
};

enum ncclAsyncFuncType {
  ASYNC_FUNC_INVALID = 0,
  ASYNC_FUNC_INIT = 1,
  ASYNC_FUNC_COLL = 2,
};
struct ncclAsyncArgs {
  ncclResult_t ret;
  enum ncclAsyncFuncType funcType;
  union {
    ncclCollArgs coll;
    ncclInitArgs init;
  };
};

thread_local struct ncclAsyncArgs ncclGroupArgs[MAX_ASYNC_OPS];

#define NCCLCHECKTHREAD(a) do { \
  if ((args->ret = (a)) != ncclSuccess) { \
    INFO(NCCL_INIT,"%s:%d -> %d [Async thread]", __FILE__, __LINE__, args->ret); \
    return args; \
  } \
} while(0)

#define CUDACHECKTHREAD(a) do { \
  if ((a) != hipSuccess) { \
    INFO(NCCL_INIT,"%s:%d -> %d [Async thread]", __FILE__, __LINE__, args->ret); \
    args->ret = ncclUnhandledCudaError; \
    return args; \
  } \
} while(0)

void* ncclAsyncThreadMain(void* args_) {
  struct ncclAsyncArgs* args = (struct ncclAsyncArgs*)args_;
  NCCLCHECKTHREAD(args->init.func(args->init.newcomm, args->init.ndev, args->init.commId, args->init.myrank, args->init.cudaDev));
  return args;
}

ncclResult_t ncclAsyncInit(ncclInitFunc_t func, ncclComm_t* newcomm, int ndev, ncclUniqueId commId, int myrank, int cudaDev) {
  if (ncclGroupIndex >= MAX_ASYNC_OPS) {
    WARN("Too many async operations in progress, max is %d", MAX_ASYNC_OPS);
    return ncclAsyncErrCheck(ncclInvalidUsage);
  }
  int index = ncclGroupIndex++;
  struct ncclAsyncArgs* args = ncclGroupArgs+index;
  args->funcType = ASYNC_FUNC_INIT;
  args->init.func = func;
  args->init.cudaDev = cudaDev;
  args->init.newcomm = newcomm;
  args->init.ndev = ndev;
  memcpy(&args->init.commId, &commId, sizeof(commId));
  args->init.myrank = myrank;
  return ncclSuccess;
}

ncclResult_t ncclAsyncColl(ncclComm_t comm) {
  struct ncclAsyncArgs* args = ncclGroupArgs;
  for (int i=0; i<ncclGroupIndex; i++) {
    if (args->coll.comm == comm) return ncclSuccess;
    args++;
  }
  if (ncclGroupIndex >= MAX_ASYNC_OPS) {
    WARN("Too many async operations in progress, max is %d", MAX_ASYNC_OPS);
    return ncclAsyncErrCheck(ncclInvalidUsage);
  }
  ncclGroupIndex++;
  args->funcType = ASYNC_FUNC_COLL;
  args->coll.comm = comm;
  return ncclSuccess;
}

NCCL_API(ncclResult_t, ncclGroupStart);
ncclResult_t ncclGroupStart() {
  NVTX3_FUNC_RANGE_IN(nccl_domain);
  if (ncclGroupMode == 0) {
    memset(ncclGroupArgs, 0, sizeof(struct ncclAsyncArgs)*MAX_ASYNC_OPS);
  }
  ncclGroupMode++;
  return ncclSuccess;
}

static ncclResult_t scheduleSendRecv(struct ncclComm* comm, int delta, int channelId, ssize_t recvbytes,
  void* recvbuff, ssize_t sendbytes, const void* sendbuff, uint16_t sendIdx, uint16_t recvIdx) {
  struct ncclInfo info = { ncclFuncSendRecv, "SendRecv",
    sendbuff, recvbuff, (size_t)std::max<ssize_t>(sendbytes,recvbytes), ncclInt8, ncclSum, -1, comm, comm->userStream, /* Args */
    1, 1 };
  info.delta = delta;
  info.channelId = channelId;
  info.sendbytes = sendbytes;
  info.recvbytes = recvbytes;
  info.sendIdx = sendIdx;
  info.recvIdx = recvIdx;
  if (delta == 0 && sendbytes != recvbytes) return ncclInvalidUsage;
  NCCLCHECK(ncclSetupP2pKernel(&info));
  return ncclSuccess;
}

void* ncclAsyncThreadPreconnect(void* args_) {
  struct ncclAsyncArgs* args = (struct ncclAsyncArgs*)args_;
  struct ncclComm* comm = args->coll.comm;
  CUDACHECKTHREAD(hipSetDevice(comm->cudaDev));
  if (CPU_COUNT(&comm->cpuAffinity)) sched_setaffinity(0, sizeof(cpu_set_t), &comm->cpuAffinity);
  NCCLCHECKTHREAD(ncclTransportP2pSetup(comm, NULL, args->coll.connIndex));
  return args;
}

static size_t getP2pChunkSize(size_t totalSize, int minChannels, int maxChannels, size_t minSize, size_t maxSize) {
  size_t size = std::max(minSize, DIVUP(totalSize, minChannels));
  int nChannels = minChannels;
  while (size > maxSize && nChannels <= maxChannels/2) {
    nChannels *= 2;
    size = DIVUP(totalSize, nChannels);
  }
  ALIGN_SIZE(size, minSize);
  return size;
}

RCCL_PARAM(P2pNetThreshold, "P2P_NET_THRESHOLD", 131072);

NCCL_API(ncclResult_t, ncclGroupEnd);
ncclResult_t ncclGroupEnd() {
  NVTX3_FUNC_RANGE_IN(nccl_domain);
  if (ncclGroupMode == 0) {
    WARN("ncclGroupEnd: not in a group call.");
    return ncclInvalidUsage;
  }
  ncclGroupMode--;
  if (ncclGroupMode > 0) return ncclSuccess;
  int savedDev;
  CUDACHECK(hipGetDevice(&savedDev));
  int activeThreads = 0;
  int doneArray[MAX_ASYNC_OPS];
  for (int i=0; i<ncclGroupIndex; i++) doneArray[i] = 1;
  ncclResult_t ret = ncclGroupError;
  int usingCudaGraphAll = -1;
  hipGraph_t* graphs = NULL;
  if (ret != ncclSuccess) goto group_cleanup;

  /* Launch async ncclCommInitRank */
  for (int i=0; i<ncclGroupIndex; i++) {
    struct ncclAsyncArgs* args = ncclGroupArgs+i;
    if (args->funcType == ASYNC_FUNC_INIT) {
      pthread_create(ncclGroupThreads+i, NULL, ncclAsyncThreadMain, args);
      activeThreads++;
      doneArray[i] = 0;
    }
  }
  /* For init, since we use threads, we just wait for threads to complete */
  while (activeThreads) {
    for (int i=0; i<ncclGroupIndex; i++) {
      struct ncclAsyncArgs* args = ncclGroupArgs+i;
      if (args->funcType == ASYNC_FUNC_INIT && doneArray[i] == 0) {
        int err = pthread_tryjoin_np(ncclGroupThreads[i], NULL);
        if (err == EBUSY) continue;
        if (err != 0) ret = ncclSystemError;
        if (args->ret != ncclSuccess) ret = args->ret;
        doneArray[i] = 1;
        activeThreads--;
      }
    }
  }

  for (int i=0; i<ncclGroupIndex; i++) {
    struct ncclAsyncArgs* args = ncclGroupArgs+i;
    if (args->funcType == ASYNC_FUNC_COLL && args->coll.comm->connect[0]) {
      args->coll.connIndex = 0;
      pthread_create(ncclGroupThreads+i, NULL, ncclAsyncThreadPreconnect, args);
    }
  }

  for (int i=0; i<ncclGroupIndex; i++) {
    struct ncclAsyncArgs* args = ncclGroupArgs+i;
    if (args->funcType == ASYNC_FUNC_COLL && args->coll.comm->connect[0]) {
      int err = pthread_join(ncclGroupThreads[i], NULL);
      if (err != 0) {
        WARN("Error waiting for pthread_join : %s", strerror(errno));
        return ncclSystemError;
      }
      INFO(NCCL_INIT, "comm %p rank %d total %ld bytes - P2P preconnect COMPLETE", args->coll.comm, args->coll.comm->rank, allocTracker[args->coll.comm->cudaDev].totalAllocSize);
      NCCLCHECKGOTO(args->ret, ret, end);
      args->coll.comm->connect[0] = 0;
    }
  }

  for (int i=0; i<ncclGroupIndex; i++) {
    struct ncclAsyncArgs* args = ncclGroupArgs+i;
    if (args->funcType == ASYNC_FUNC_COLL && args->coll.comm->connect[NCCL_CONN_IDX_P2P_NET]) {
      args->coll.connIndex = NCCL_CONN_IDX_P2P_NET;
      pthread_create(ncclGroupThreads+i, NULL, ncclAsyncThreadPreconnect, args);
    }
  }

  for (int i=0; i<ncclGroupIndex; i++) {
    struct ncclAsyncArgs* args = ncclGroupArgs+i;
    if (args->funcType == ASYNC_FUNC_COLL && args->coll.comm->connect[NCCL_CONN_IDX_P2P_NET]) {
      int err = pthread_join(ncclGroupThreads[i], NULL);
      if (err != 0) {
        WARN("Error waiting for pthread_join : %s", strerror(errno));
        return ncclSystemError;
      }
      INFO(NCCL_INIT, "comm %p rank %d total %ld bytes - P2P NET preconnect COMPLETE", args->coll.comm, args->coll.comm->rank, allocTracker[args->coll.comm->cudaDev].totalAllocSize);
      NCCLCHECKGOTO(args->ret, ret, end);
      args->coll.comm->connect[NCCL_CONN_IDX_P2P_NET] = 0;
    }
  }

  for (int i=0; i<ncclGroupIndex; i++) {
    struct ncclAsyncArgs* args = ncclGroupArgs+i;
    if (args->funcType == ASYNC_FUNC_COLL) {
      struct ncclComm* comm = args->coll.comm;
      int rank = comm->rank;
      int nRanks = comm->nRanks;

      // Compute how much to split operations
      // Natural step size matching buffer steps.
      ssize_t stepSize = comm->buffSizes[NCCL_PROTO_SIMPLE] / NCCL_STEPS;
      // Try to use all channels
      int nChannelsMax = comm->p2pnChannelsPerPeer;
      int nChannelsMin = nChannelsMax;
      // Try to use all channels, but one channel per operation.
      while (nChannelsMin*comm->nRanks > std::max(comm->nChannels, comm->p2pnChannels) && nChannelsMin > 1) nChannelsMin /= 2;
      // Avoid overloading channels with 8+ operations as we loose the sync warp, hence a bit of bandwidth.
      while (nChannelsMax*comm->nRanks > std::max(comm->nChannels, comm->p2pnChannels)*4 && nChannelsMax > 1) nChannelsMax /= 2;

      while (comm->p2pSendCount > 0 || comm->p2pRecvCount > 0) {
        // schedule delta 0, +1, -1, +2, -2, ...
        // also make sure we don't do 0 twice, nor +n/2 and -n/2 if n is even.
        for (int d=0; d<=nRanks/4; d++) {
          int deltas[4] = { d, (nRanks-d)%nRanks, nRanks/2-d, (nRanks-(nRanks/2-d))%nRanks };
          int index = 0;
          int delta = deltas[index];
sched_delta:
          uint32_t from = (rank+nRanks-delta)%nRanks;
          uint32_t to = (rank+delta)%nRanks;
          struct ncclP2Pinfo* recv = comm->p2pRecvs[from] ? comm->p2pRecvs[from]->getNext() : NULL;
          struct ncclP2Pinfo* send = comm->p2pSends[to] ? comm->p2pSends[to]->getNext() : NULL;
          if (recv != NULL || send != NULL) {
            ssize_t totRecvBytes = -1, totSendBytes = -1;
            if (recv != NULL) totRecvBytes = recv->nbytes;
            if (send != NULL) totSendBytes = send->nbytes;
            ssize_t recvChunkSize = getP2pChunkSize(totRecvBytes, nChannelsMin, nChannelsMax, stepSize, SENDRECV_SLICEFACTOR*stepSize);
            ssize_t sendChunkSize = getP2pChunkSize(totSendBytes, nChannelsMin, nChannelsMax, stepSize, SENDRECV_SLICEFACTOR*stepSize);

            uint16_t sendIdx = 0, recvIdx = 0;
            if(comm->p2pNet && totSendBytes > rcclParamP2pNetThreshold())
              sendIdx = NCCL_CONN_IDX_P2P_NET;
            if(comm->p2pNet && totRecvBytes > rcclParamP2pNetThreshold())
              recvIdx = NCCL_CONN_IDX_P2P_NET;

            ssize_t sendOffset = 0;
            ssize_t recvOffset = 0;
            int sendRemaining = 1, recvRemaining = 1;
            int chunk = 0;
            do {
              int channelId = (delta+comm->p2pChannels[chunk%comm->p2pnChannelsPerPeer]) % comm->p2pnChannels;
              ssize_t recvbytes = totRecvBytes-recvOffset;
              ssize_t sendbytes = totSendBytes-sendOffset;
              if (recvbytes > recvChunkSize) { recvbytes = recvChunkSize; } else { recvRemaining = 0; }
              if (sendbytes > sendChunkSize) { sendbytes = sendChunkSize; } else { sendRemaining = 0; }
              // 0-bytes send/recv are considered as syncs. Make sure we only add syncs when requested
              // (total size == 0), otherwise set size to -1 so that the kernel skips the operation.
              if (sendbytes == 0 && totSendBytes != 0) sendbytes = -1;
              if (recvbytes == 0 && totRecvBytes != 0) recvbytes = -1;
              if (sendbytes >= 0 || recvbytes >= 0) {
                NCCLCHECKGOTO(scheduleSendRecv(comm, delta, channelId,
                      recvbytes, recv ? ((char*)(recv->buff)) + recvOffset : NULL,
                      sendbytes, send ? ((const char*)(send->buff)) + sendOffset : NULL, sendIdx, recvIdx), ret, group_cleanup);
              }
              recvOffset += recvChunkSize;
              sendOffset += sendChunkSize;
              chunk++;
            } while (sendRemaining || recvRemaining);
            if (recv) comm->p2pRecvCount--;
            if (send) comm->p2pSendCount--;
          }
          if (recv == NULL && comm->p2pRecvs[from]) comm->p2pRecvs[from]->recycle();
          if (send == NULL && comm->p2pSends[to]) comm->p2pSends[to]->recycle();
          index++;
          if (index == 1 && deltas[1] == deltas[0]) index++;
          if (index == 2 && deltas[2] == deltas[0]) index++;
          if (index == 3 && deltas[3] == deltas[2]) index++;
          if (index == 3 && deltas[3] == deltas[1]) index++;
          if (index < 4) {
            delta = deltas[index];
            goto sched_delta;
          }
        }
      }
    }
  }

  /* Collectives are done in three steps :
   * 0. Save kernels previously enqueued. Compute channel, algo, proto, etc.
   * 1. Barrier Check In. Only the last call may call cudaLaunchKernel[cooperative]
   * 2. Barrier Wait. No CUDA call is permitted
   * 3. Enqueue Events. CUDA event wait/enqueue.
   * This is needed because step 2 cannot call any CUDA primitive, otherwise if
   * cudaFree happens between 1 and 3, it could block that CUDA call and
   * prevent some ranks from launching their network threads, which would
   * prevent the NCCL call from completing, blocking the cudaFree call.
   */

  // Check whether we are in cuda graph mode
  NCCLCHECK(ncclCalloc(&graphs, ncclGroupIndex));
  for (int i=0; i<ncclGroupIndex; i++) {
    struct ncclAsyncArgs* args = ncclGroupArgs+i;
    if (args->funcType == ASYNC_FUNC_COLL) {
      ncclComm_t comm = args->coll.comm;
      NCCLCHECKGOTO(ncclGetCudaGraph(comm, graphs+i), ret, group_cleanup);
      if (usingCudaGraphAll == -1) {
        usingCudaGraphAll = comm->usingCudaGraph;
      } else if (usingCudaGraphAll != comm->usingCudaGraph) {
        WARN("Illegal to have some communicators in graph mode while others not");
        ret = ncclInvalidUsage;
        goto group_cleanup;
      }
    }
  }
  for (int i=0; i<ncclGroupIndex; i++) {
    struct ncclAsyncArgs* args = ncclGroupArgs+i;
    if (args->funcType == ASYNC_FUNC_COLL) {
      ncclComm_t comm = args->coll.comm;
      NCCLCHECKGOTO(ncclSetupAsyncKernels(comm), ret, group_cleanup);
    }
  }
  for (int i=0; i<ncclGroupIndex; i++) {
    struct ncclAsyncArgs* args = ncclGroupArgs+i;
    if (args->funcType == ASYNC_FUNC_COLL) {
      if (args->coll.comm->userStream == hipStreamDefault/* ||
          args->coll.comm->userStream == hipStreamPerThread ||
          args->coll.comm->userStream == hipStreamLegacy*/)
        CUDACHECKGOTO(hipSetDevice(args->coll.comm->cudaDev), ret, end);
      if (usingCudaGraphAll == 1) {
        NCCLCHECKGOTO(ncclCudaGraphHostSetup(args->coll.comm, graphs[i]), ret, end);
      } else {
        ncclEnqueueHostSetup<0>(args->coll.comm->enqueueInfo);
      }
      NCCLCHECKGOTO(ncclLaunchBarrier(args->coll.comm), ret, end);
    }
  }
  for (int i=0; i<ncclGroupIndex; i++) {
    struct ncclAsyncArgs* args = ncclGroupArgs+i;
    if (args->funcType == ASYNC_FUNC_COLL) {
      CUDACHECKGOTO(hipSetDevice(args->coll.comm->cudaDev), ret, end);
      NCCLCHECKGOTO(ncclLaunchKernel(args->coll.comm), ret, end);
    }
  }
  for (int i=0; i<ncclGroupIndex; i++) {
    struct ncclAsyncArgs* args = ncclGroupArgs+i;
    if (args->funcType == ASYNC_FUNC_COLL) {
      if (args->coll.comm->userStream == hipStreamDefault/* ||
          args->coll.comm->userStream == hipStreamPerThread ||
          args->coll.comm->userStream == hipStreamLegacy*/)
        CUDACHECKGOTO(hipSetDevice(args->coll.comm->cudaDev), ret, end);
      NCCLCHECKGOTO(ncclRecordEvents(args->coll.comm), ret, end);
      NCCLCHECKGOTO(ncclLaunchReset(args->coll.comm), ret, end);
    }
  }

  goto end;
group_cleanup:
  if (ret != ncclSuccess) {
    // At least one call in the group failed. Since we want to make that group
    // an atomic operation, we need to cancel all operations.
    for (int i=0; i<ncclGroupIndex; i++) {
      struct ncclAsyncArgs* args = ncclGroupArgs+i;
      if (args->funcType == ASYNC_FUNC_INIT) {
        if (args->init.newcomm) ncclCommDestroy(*args->init.newcomm);
        *args->init.newcomm = NULL;
      } else {
        struct ncclComm* comm = args->coll.comm;
        // Reset aggregation counters
        comm->asyncOpCount = 0;
        comm->asyncTotalSize = 0;
        // Dequeue p2p lists
        if (comm->p2pSendCount > 0 || comm->p2pRecvCount > 0) {
          for (int peer=0; peer<comm->nRanks; peer++) {
            if (comm->p2pSends[peer]) comm->p2pSends[peer]->recycle();
            if (comm->p2pRecvs[peer]) comm->p2pRecvs[peer]->recycle();
          }
          comm->p2pSendCount = comm->p2pRecvCount = 0;
        }
        /* Free all proxy ops in state->nextOps */
        struct ncclProxyState* state = &comm->proxyState;
	pthread_mutex_lock(&state->poolMutex);
	for (struct ncclProxyArgs *op = state->nextOps; op; op = op->next) {
          op->next = state->pool;
          state->pool = op;
        }
	pthread_mutex_unlock(&state->poolMutex);
        state->nextOps = NULL;

        ncclLaunchReset(comm);
      }
    }
  }
end:
  ncclGroupError = ncclSuccess;
  ncclGroupIndex = 0;
  CUDACHECK(hipSetDevice(savedDev)); // do other clean-ups first before calling hipSetDevice, because this call can fail too
  if (graphs) free(graphs);
  return ret;
}
