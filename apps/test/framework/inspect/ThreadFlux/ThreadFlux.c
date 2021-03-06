/*
 * This app creates and deletes Legato threads according to the specified strategy and time
 * interval.
 */

// TODO: use usleep instead of nanosleep for delete interval

#include "legato.h"

#define ThreadNameBufferSize 50

static long ThreadNum;
static char ThreadNameBuffer[ThreadNameBufferSize] = {0};
static le_thread_Ref_t* ThreadRefArray; // array storing references to the threads


// This is the "main" function for each thread
static void* ThreadMain
(
    void* context
)
{
    LE_INFO("Thread [%s] has started", le_thread_GetMyName());

    le_event_RunLoop();
    return NULL;
}


// Create N threads
void createThreads
(
    void
)
{
    long threadCnt = 0;
    while (threadCnt < ThreadNum)
    {
        snprintf(ThreadNameBuffer, ThreadNameBufferSize, "Thread%ld", threadCnt);

        // need to store the thread references in an array
        ThreadRefArray[threadCnt] = le_thread_Create(ThreadNameBuffer, ThreadMain, NULL);

        le_thread_Start(ThreadRefArray[threadCnt]);

        threadCnt++;
    }

    LE_INFO("========== Created all threads ===========");
}


// Deletes one thread
void deleteThread
(
    struct timespec* sleepTimeRef,
    int threadIndex
)
{
    LE_INFO("==== Deleting thread %d ====", threadIndex);

    // some delay between thread deletions
    nanosleep(sleepTimeRef, NULL);

    le_thread_Cancel(ThreadRefArray[threadIndex]);
}


// deleting threads from 1 to N, except for the last thread
void deleteThreadsFrom1ToN
(
    long timeIntervalNano // nanosecs
)
{
    LE_INFO("==== Deleting threads from 1 to N ====");

    // Delete threads from the first to the Nth
    long threadCnt = 0;
    struct timespec sleepTime = {0, timeIntervalNano};
    while (threadCnt < (ThreadNum - 1))
    {
        deleteThread(&sleepTime, threadCnt);
        threadCnt++;
    }
}


COMPONENT_INIT
{
    char argDeleteStrat[100];
    long argSleepIntervalNano;

    if (le_arg_NumArgs() != 3)
    {
        LE_ERROR("Usage: ThreadFlux [1toN | None] [delete interval] [number of threads created]");
        exit(EXIT_FAILURE);
    }

    strcpy(argDeleteStrat, le_arg_GetArg(0));
    argSleepIntervalNano = strtol(le_arg_GetArg(1), NULL, 0);
    ThreadNum = strtol(le_arg_GetArg(2), NULL, 0);

    // Initializing the array storing thread refs.
    ThreadRefArray = malloc(ThreadNum * sizeof(le_thread_Ref_t));

    // Create/Delete threads, according to the defined strategy
    if (strcmp(argDeleteStrat, "1toN") == 0)
    {
        createThreads();
        deleteThreadsFrom1ToN(argSleepIntervalNano);
    }
    else if (strcmp(argDeleteStrat, "None") == 0)
    {
        createThreads();
        LE_INFO("==== No threads deleted ====");
    }
    else
    {
        LE_ERROR("invalid thread delete strategy option.");
        exit(EXIT_FAILURE);
    }

    LE_INFO("========== FINISHED ===========");
}

