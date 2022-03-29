#include "ThreadPool.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <sys/select.h>
#include <sys/types.h>
#include <unistd.h>

#define NUMSTEP 5               // 线程数操作步长
#define BUFSIZE 1024

typedef struct Task_t           // 定义任务类型
{
    void (*function)(void *);   // 保存添加进来的任务函数
    void *arg;                  // 保存任务函数的参数
} Task_t;

struct ThreadPool_t // 定义线程池类型
{
    Task_t *taskQueue;          // 任务队列数组
    int queueCapacity;          // 任务队列最大容量
    int queueSize;              // 任务队列当前任务数
    int queueRear;              // 队尾
    int queueFront;             // 队头

    pthread_t managerID;        // 管理者线程ID
    pthread_t *workerIDs;       // 工作者线程ID数组
    int numMax;                 // 工作者线程最大的线程数
    int numMin;                 // 工作者线程最小的线程数
    int numLive;                // 工作者线程存活的线程数
    int numBusy;                // 工作者线程忙的线程数
    int numExit;                // 工作者线程需要退出的线程数

    pthread_mutex_t mutexPool;  // 线程池锁
    pthread_mutex_t mutexBusy;  // 忙线程数锁
    pthread_cond_t notFull;     // 非满条件变量，用于唤醒生产者(添加任务函数)
    pthread_cond_t notEmpty;    // 非空条件变量，用于唤醒消费者(工作者线程)

    int shutstatus;             // 线程池状态，0 打开，-1 关闭
};

/**
 * printStatus ： 统计忙线程数和存活线程数，并可视化打印
 *
 * 返回值 ： 无
 *
 * 参数 ： ThreadPool_t *线程池类型
 *
 * 打印样例 ： [ ++++++++++----- ] : busy == 10, live == 15
 *
 */
static void printStatus(ThreadPool_t *argPool)
{
    struct ThreadPool_t *pool = (struct ThreadPool_t *)argPool;
    int numLive, numBusy;
    int i;
    char buf[BUFSIZE];
    memset(buf, 0, BUFSIZE);

    numLive = getThreadLive(pool);
    numBusy = getThreadBusy(pool);

    for (i = 0; i < numBusy; i++)
        strcat(buf, "+");
    for (i = 0; i < numLive - numBusy; i++)
        strcat(buf, "-");
    fprintf(stdout, "[ %s ] : busy == %d, live == %d\n", buf, numBusy, numLive);
}

/**
 * working ： 工作者线程任务函数，负责从任务队列中取出任务并执行
 *
 * 返回值 ： NULL
 *
 * 参数 ： void *类型，传入线程池
 *
 * 打印样例 ： [thread = 139987412481792] is going to work...
 *
 */
static void *working(void *arg)
{
    struct ThreadPool_t *pool = (struct ThreadPool_t *)arg;
    Task_t task;
    while (1)
    {
        pthread_mutex_lock(&pool->mutexPool); // 对线程池加锁
        while (pool->queueSize == 0 && pool->shutstatus == 0)
        {
            pthread_cond_wait(&pool->notEmpty, &pool->mutexPool); // 阻塞直到任务队列不为空
            if (pool->numExit > 0)                                // 若唤醒后，发现线程池中有需要退出的线程数
            {
                pool->numExit--;
                if (pool->numLive > pool->numMin)
                {
                    pool->numLive--;
                    pthread_mutex_unlock(&pool->mutexPool);
                    thread_Exit_unlock(pool); // 线程自杀
                }
            }
        }
        if (pool->shutstatus == -1) // 若线程池已经关闭，线程自杀
        {
            pthread_mutex_unlock(&pool->mutexPool);
            thread_Exit_unlock(pool);
        }

        task.function = pool->taskQueue[pool->queueFront].function;     // 取出任务
        task.arg = pool->taskQueue[pool->queueFront].arg;
        memset(&pool->taskQueue[pool->queueFront], 0, sizeof(Task_t));   // 从队列取出任务后，将队列中相应任务清空
        pool->queueFront = (pool->queueFront + 1) % pool->queueCapacity; // 移动队头指针
        pool->queueSize--;                                               // 任务队列中任务数量-1
        pthread_cond_signal(&pool->notFull);                             // 唤醒任务生产者
        pthread_mutex_unlock(&pool->mutexPool);

        pthread_mutex_lock(&pool->mutexBusy); // 加锁，改变线程池中忙线程数
        pool->numBusy++;
        pthread_mutex_unlock(&pool->mutexBusy);
#ifdef DEBUG
        fprintf(stdout, "[thread = %ld] is going to work...\n", pthread_self());
#endif // DEBUG

        task.function(task.arg); // 执行任务

#ifdef DEBUG
        fprintf(stdout, "[thread = %ld] is done work...\n", pthread_self());
#endif // DEBUG

        free(task.arg); // 释放任务资源
        task.function = NULL;
        task.arg = NULL;
        pthread_mutex_lock(&pool->mutexBusy);
        pool->numBusy--;
        pthread_mutex_unlock(&pool->mutexBusy);
        sched_yield(); // 出让调度器给其他线程
    }
    pthread_exit(NULL);
}

/**
 * manager ： 管理者线程任务函数，负责监视、增加和减少线程池中线程的存活线程数量
 *
 * 返回值 ： NULL
 *
 * 参数 ： void *类型，传入线程池
 *
 */
static void *manager(void *arg)
{
    struct ThreadPool_t *pool = (struct ThreadPool_t *)arg;
    struct timeval tv;
    int numLive, numBusy, queueSize;
    int i, count;
    while (pool->shutstatus == 0)
    {
        tv.tv_sec = 0;                    // 定时500ms，可根据实际场景改变
        tv.tv_usec = 500000;              // ！！每次定时都需要重新设定数值！！
        select(0, NULL, NULL, NULL, &tv); // select作为延时函数，替换sleep，保证线程安全

        pthread_mutex_lock(&pool->mutexPool);
        numLive = pool->numLive;
        queueSize = pool->queueSize;
        pthread_mutex_unlock(&pool->mutexPool);

        pthread_mutex_lock(&pool->mutexBusy);
        numBusy = pool->numBusy;
        pthread_mutex_unlock(&pool->mutexBusy);

        count = 0;
        if ((numLive < queueSize || numBusy > numLive*0.8) && numLive < pool->numMax) // 当存活线程数小于待取任务数量，并且小于最大线程数
        {
            pthread_mutex_lock(&pool->mutexPool); // 添加 NUMSTEP 步长的线程
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

        if (numBusy * 2 < numLive && numLive > pool->numMin) // 当忙线程数 * 2小于存活线程数，并且存活的线程大于最小线程数
        {
            pthread_mutex_lock(&pool->mutexPool);
            pool->numExit = NUMSTEP; // 杀死 NUMSTEP 步长的线程
            pthread_mutex_unlock(&pool->mutexPool);
            for (i = 0; i < NUMSTEP; i++)
            {
                pthread_cond_signal(&pool->notEmpty); // 唤醒工作线程，让其自杀
            }
        }
        printStatus(pool); // 打印线程池中线程信息
        sched_yield();     // 出让调度器
    }
    pthread_exit(NULL);
}

/**
 * threadPool_Create ： 线程池创建函数，创建一个线程池
 *
 * 返回值 ： 失败返回 NULL，成功返回线程池对象地址
 *
 * 参数 ： min：最小线程池数，max：最大线程池数，queueCapacity：最大任务队列数
 *
 * 失败打印样例 ： taskQueue malloc failed ...
 *
 */
ThreadPool_t *threadPool_Create(int min, int max, int queueCapacity)
{
    struct ThreadPool_t *pool = malloc(sizeof(struct ThreadPool_t));
    int i;
    do
    { // do while 实现 goto 跳转
        if (pool == NULL)
        {
            fprintf(stderr, "threadpool malloc failed ...\n");
            break; // 申请内存失败就跳过剩下的初始化
        }

        pool->taskQueue = malloc(sizeof(Task_t) * queueCapacity);
        if (pool->taskQueue == NULL)
        {
            fprintf(stderr, "taskQueue malloc failed ...\n");
            break;
        }
        memset(pool->taskQueue, 0, sizeof(Task_t) * queueCapacity);
        pool->queueCapacity = queueCapacity; // 各个成员的初始化
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

        if (pthread_mutex_init(&pool->mutexPool, NULL) != 0 || // 初始化锁和条件变量
            pthread_mutex_init(&pool->mutexBusy, NULL) != 0 ||
            pthread_cond_init(&pool->notFull, NULL) != 0 ||
            pthread_cond_init(&pool->notEmpty, NULL) != 0)
        {
            fprintf(stderr, "lock init failed ...\n");
            break;
        }
        pool->shutstatus = 0;                                  // 开启线程池
        pthread_create(&pool->managerID, NULL, manager, pool); // 创建管理者线程
        for (i = 0; i < min; i++)
        {
            if (pool->workerIDs[i] == 0)
            {
                pthread_create(&pool->workerIDs[i], NULL, working, pool); // 创建工作者线程
            }
        }
        return pool;
    } while (0);
    if (pool != NULL && pool->workerIDs != NULL) // 申请内存失败跳转到这里开始，依次析构
        free(pool->workerIDs);
    if (pool != NULL && pool->taskQueue != NULL)
        free(pool->taskQueue);
    if (pool != NULL)
        free(pool);
    return NULL;
}

/**
 * threadPool_Destroy ： 线程池销毁函数，销毁一个线程池
 *
 * 返回值 ： 失败返回 -1，成功返回 0
 *
 * 参数 ： ThreadPool_t *类型，传入需要销毁的线程
 *
 */
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
        pthread_cond_signal(&pool->notEmpty); // 唤醒所有存活线程，让其自杀
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

/**
 * threadPool_Addtask ： 任务队列添加任务函数，添加一个任务
 *
 * 返回值 ： 失败返回 -1，成功返回 0
 *
 * 参数 ： argPool：需要添加任务的线程池， function：任务函数，arg：任务函数参数
 *
 */
int threadPool_Addtask(ThreadPool_t *argPool, void (*function)(void *), void *arg)
{
    struct ThreadPool_t *pool = (struct ThreadPool_t *)argPool;

    pthread_mutex_lock(&pool->mutexPool);
    while (pool->queueSize == pool->queueCapacity && pool->shutstatus == 0)
    {
        pthread_cond_wait(&pool->notFull, &pool->mutexPool); // 阻塞直到等待任务队列不为满
    }
    if (pool->shutstatus == -1)
    {
        fprintf(stderr, "thread pool has been shutdown...\n");
        pthread_mutex_unlock(&pool->mutexPool);
        return -1;
    }

    pool->taskQueue[pool->queueRear].function = function; // 将任务存储到任务队列中
    pool->taskQueue[pool->queueRear].arg = arg;
    pool->queueRear = (pool->queueRear + 1) % pool->queueCapacity; // 移动队尾指针
    pool->queueSize++;
    pthread_cond_signal(&pool->notEmpty); // 队列不为空，唤醒工作者线程
    pthread_mutex_unlock(&pool->mutexPool);
    return 0;
}

/**
 * thread_Exit_unlock ： 线程退出函数，并将该线程 ID 从工作者线程数组中删除
 *
 * 返回值 ： 无
 *
 * 参数 ： argPool：ThreadPool_t *类型，传入当前线程所在的线程池
 *
 */
void thread_Exit_unlock(ThreadPool_t *argPool)
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

/**
 * getThreadLive ： 获取线程池中存活线程数
 *
 * 返回值 ： 线程池中存活线程数
 *
 * 参数 ： argPool：ThreadPool_t *类型，传入当前线程所在的线程池
 *
 */
int getThreadLive(ThreadPool_t *argPool)
{
    struct ThreadPool_t *pool = (struct ThreadPool_t *)argPool;
    int num;
    pthread_mutex_lock(&pool->mutexPool);
    num = pool->numLive;
    pthread_mutex_unlock(&pool->mutexPool);
    return num;
}

/**
 * getThreadBusy ： 获取线程池中忙线程数
 *
 * 返回值 ： 线程池中忙线程数
 *
 * 参数 ： argPool：ThreadPool_t *类型，传入当前线程所在的线程池
 *
 */
int getThreadBusy(ThreadPool_t *argPool)
{
    struct ThreadPool_t *pool = (struct ThreadPool_t *)argPool;
    int num;
    pthread_mutex_lock(&pool->mutexBusy);
    num = pool->numBusy;
    pthread_mutex_unlock(&pool->mutexBusy);
    return num;
}
