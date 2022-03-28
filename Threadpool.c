#include "ThreadPool.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <sys/select.h>
#include <sys/types.h>
#include <unistd.h>

#define NUMSTEP 5
#define BUFSIZE 1024

typedef struct Task_t
{
    void (*function)(void *);
    void *arg;
} Task_t;

struct ThreadPool_t
{
    Task_t *taskQueue;
    int queueCapacity;
    int queueSize;
    int queueRear;
    int queueFront;

    pthread_t managerID;
    pthread_t *workerIDs;
    int numMax;
    int numMin;
    int numLive;
    int numBusy;
    int numExit;

    pthread_mutex_t mutexPool;
    pthread_mutex_t mutexBusy;
    pthread_cond_t notFull;
    pthread_cond_t notEmpty;

    int shutstatus;
};

static void printStatus(ThreadPool_t *argPool)
{
    struct ThreadPool_t *pool = (struct ThreadPool_t *)argPool;
    int numLive, numBusy;
    int i;
    char buf[BUFSIZE];
    memset(buf, 0, BUFSIZE);

    pthread_mutex_lock(&pool->mutexPool);
    numLive = pool->numLive;
    pthread_mutex_unlock(&pool->mutexPool);

    pthread_mutex_lock(&pool->mutexBusy);
    numBusy = pool->numBusy;
    pthread_mutex_unlock(&pool->mutexBusy);

    for (i = 0; i < numBusy; i++)
        strcat(buf, "+");
    for (i = 0; i < numLive - numBusy; i++)
        strcat(buf, "=");
    fprintf(stdout, "[ %s ] : busy == %d, live == %d\n", buf, numBusy, numLive);
}

static void *working(void *arg)
{
    struct ThreadPool_t *pool = (struct ThreadPool_t *)arg;
    Task_t task;
    while (1)
    {
        pthread_mutex_lock(&pool->mutexPool);
        while (pool->queueSize == 0 && pool->shutstatus == 0)
        {
            pthread_cond_wait(&pool->notEmpty, &pool->mutexPool);
            if (pool->numExit > 0)
            {
                pool->numExit--;
                if (pool->numLive > pool->numMin)
                {
                    pool->numLive--;
                    pthread_mutex_unlock(&pool->mutexPool);
                    thread_Exit(pool);
                }
            }
        }
        if (pool->shutstatus == -1)
        {
            pthread_mutex_unlock(&pool->mutexPool);
            thread_Exit(pool);
        }

        task.function = pool->taskQueue[pool->queueFront].function;
        task.arg = pool->taskQueue[pool->queueFront].arg;
        memset(&pool->taskQueue[pool->queueFront], 0, sizeof(Task_t));
        pool->queueFront = (pool->queueFront + 1) % pool->queueCapacity;
        pool->queueSize--;
        pthread_cond_signal(&pool->notFull);
        pthread_mutex_unlock(&pool->mutexPool);

        pthread_mutex_lock(&pool->mutexBusy);
        pool->numBusy++;
        pthread_mutex_unlock(&pool->mutexBusy);
#ifdef DEBUG
        fprintf(stdout, "[thread = %ld] is going to work...\n", pthread_self());
#endif // DEBUG

        task.function(task.arg);

#ifdef DEBUG
        fprintf(stdout, "[thread = %ld] is done work...\n", pthread_self());
#endif // DEBUG

        free(task.arg);
        task.function = NULL;
        task.arg = NULL;
        pthread_mutex_lock(&pool->mutexBusy);
        pool->numBusy--;
        pthread_mutex_unlock(&pool->mutexBusy);
        sched_yield();
    }
    pthread_exit(NULL);
}

static void *manager(void *arg)
{
    struct ThreadPool_t *pool = (struct ThreadPool_t *)arg;
    struct timeval tv;
    int numLive, numBusy, queueSize;
    int i, count;
    while (pool->shutstatus == 0)
    {
        tv.tv_sec = 0;
        tv.tv_usec = 500000;
        select(0, NULL, NULL, NULL, &tv);

        pthread_mutex_lock(&pool->mutexPool);
        numLive = pool->numLive;
        queueSize = pool->queueSize;
        pthread_mutex_unlock(&pool->mutexPool);

        pthread_mutex_lock(&pool->mutexBusy);
        numBusy = pool->numBusy;
        pthread_mutex_unlock(&pool->mutexBusy);

        count = 0;
        if (numLive < queueSize && numLive < pool->numMax)
        {
            pthread_mutex_lock(&pool->mutexPool);
            for (i = 0; pool->numLive < pool->numMax && count < NUMSTEP && i < pool->numMax; i++)
            {
                if (pool->workerIDs[i] == 0)
                {
                    pthread_create(&pool->workerIDs[i], NULL, working, pool);
                    count++;
                    pool->numLive++;
                }
            }
            pthread_mutex_unlock(&pool->mutexPool);
        }

        if (numBusy * 2 < numLive && numLive > pool->numMin)
        {
            pthread_mutex_lock(&pool->mutexPool);
            pool->numExit = NUMSTEP;
            pthread_mutex_unlock(&pool->mutexPool);
            for (i = 0; i < NUMSTEP; i++)
            {
                pthread_cond_signal(&pool->notEmpty);
            }
        }
        printStatus(pool);
        sched_yield();
    }
    pthread_exit(NULL);
}

ThreadPool_t *threadPool_Create(int min, int max, int queueCapacity)
{
    struct ThreadPool_t *pool = malloc(sizeof(struct ThreadPool_t));
    int i;
    do
    {
        if (pool == NULL)
        {
            fprintf(stderr, "threadpool malloc failed ...\n");
            break;
        }

        pool->taskQueue = malloc(sizeof(Task_t) * queueCapacity);
        if (pool->taskQueue == NULL)
        {
            fprintf(stderr, "taskQueue malloc failed ...\n");
            break;
        }
        memset(pool->taskQueue, 0, sizeof(Task_t) * queueCapacity);
        pool->queueCapacity = queueCapacity;
        pool->queueSize = 0;
        pool->queueRear = 0;
        pool->queueFront = 0;

        pool->workerIDs = malloc(sizeof(pthread_t) * max);
        if (pool->workerIDs == NULL)
        {
            fprintf(stderr, "workerIDs malloc failed ...\n");
            break;
        }
        memset(pool->workerIDs, 0, sizeof(pthread_t) * max);
        pool->numMax = max;
        pool->numMin = min;
        pool->numLive = min;
        pool->numBusy = 0;
        pool->numExit = 0;

        if (pthread_mutex_init(&pool->mutexPool, NULL) != 0 ||
            pthread_mutex_init(&pool->mutexBusy, NULL) != 0 ||
            pthread_cond_init(&pool->notFull, NULL) != 0 ||
            pthread_cond_init(&pool->notEmpty, NULL) != 0)
        {
            fprintf(stderr, "lock init failed ...\n");
            break;
        }
        pool->shutstatus = 0;
        pthread_create(&pool->managerID, NULL, manager, pool);
        for (i = 0; i < min; i++)
        {
            if (pool->workerIDs[i] == 0)
            {
                pthread_create(&pool->workerIDs[i], NULL, working, pool);
            }
        }
        return pool;
    } while (0);
    if (pool != NULL && pool->workerIDs != NULL)
        free(pool->workerIDs);
    if (pool != NULL && pool->taskQueue != NULL)
        free(pool->taskQueue);
    if (pool != NULL)
        free(pool);
    return NULL;
}

int threadPool_Destroy(ThreadPool_t *argPool)
{
    struct ThreadPool_t *pool = (struct ThreadPool_t *)argPool;
    if (pool == NULL)
    {
        fprintf(stderr, "thread pool is not existed...\n");
        return -1;
    }

    pool->shutstatus = -1;

    pthread_join(pool->managerID, NULL);
    for (int i = 0; i < pool->numLive; i++)
    {
        pthread_cond_signal(&pool->notEmpty);
    }

    pthread_mutex_destroy(&pool->mutexBusy);
    pthread_mutex_destroy(&pool->mutexPool);
    pthread_cond_destroy(&pool->notEmpty);
    pthread_cond_destroy(&pool->notFull);

    if (pool != NULL && pool->workerIDs != NULL)
        free(pool->workerIDs);
    if (pool != NULL && pool->taskQueue != NULL)
        free(pool->taskQueue);
    if (pool != NULL)
        free(pool);
    pool = NULL;
#ifdef DEBUG
    fprintf(stdout, "thread pool is going to be destroyed...\n");
#endif // DEBUG
    return 0;
}

int threadPool_Addtask(ThreadPool_t *argPool, void (*function)(void *), void *arg)
{
    struct ThreadPool_t *pool = (struct ThreadPool_t *)argPool;

    pthread_mutex_lock(&pool->mutexPool);
    while (pool->queueSize == pool->queueCapacity && pool->shutstatus == 0)
    {
        pthread_cond_wait(&pool->notFull, &pool->mutexPool);
    }
    if (pool->shutstatus == -1)
    {
        fprintf(stderr, "thread pool has been shutdown...\n");
        pthread_mutex_unlock(&pool->mutexPool);
        return -1;
    }

    pool->taskQueue[pool->queueRear].function = function;
    pool->taskQueue[pool->queueRear].arg = arg;
    pool->queueRear = (pool->queueRear + 1) % pool->queueCapacity;
    pool->queueSize++;
    pthread_cond_signal(&pool->notEmpty);
    pthread_mutex_unlock(&pool->mutexPool);
    return 0;
}

void thread_Exit(ThreadPool_t *argPool)
{
    struct ThreadPool_t *pool = (struct ThreadPool_t *)argPool;
    pthread_t tmptid = pthread_self();
    for (int i = 0; i < pool->numMax; i++)
    {
        if (pool->workerIDs[i] == tmptid)
        {
            pool->workerIDs[i] = 0;
            break;
        }
    }
#ifdef DEBUG
    fprintf(stdout, "[thread = %ld] is going to exit...\n", tmptid);
#endif // DEBUG
    pthread_exit(NULL);
}

int getThreadLive(ThreadPool_t *argPool)
{
    struct ThreadPool_t *pool = (struct ThreadPool_t *)argPool;
    int num;
    pthread_mutex_lock(&pool->mutexPool);
    num = pool->numLive;
    pthread_mutex_unlock(&pool->mutexPool);
    return num;
}

int getThreadBusy(ThreadPool_t *argPool)
{
    struct ThreadPool_t *pool = (struct ThreadPool_t *)argPool;
    int num;
    pthread_mutex_lock(&pool->mutexBusy);
    num = pool->numBusy;
    pthread_mutex_unlock(&pool->mutexBusy);
    return num;
}
