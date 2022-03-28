#ifndef __THREADPOOL_H__
#define __THREADPOOL_H__

#define DEBUG

typedef void ThreadPool_t;

ThreadPool_t* threadPool_Create(int min, int max, int queueCapacity);

int threadPool_Addtask(ThreadPool_t *, void (*)(void *), void *);

int threadPool_Destroy(ThreadPool_t *);

void thread_Exit(ThreadPool_t *);

int getThreadLive(ThreadPool_t *);

int getThreadBusy(ThreadPool_t *);

#endif
