#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/msg.h>
#include <sys/shm.h>
#include <semaphore.h>
#include <time.h>
#include <errno.h>

#define MSG_PASSING 1
#define SHARED_MEM 2

typedef struct {
    int flag;      // 1 for message passing, 2 for shared memory
    union{
        int msqid; //for system V api. You can replace it with structure for POSIX api
        char* shm_addr;
    }storage;
} mailbox_t;


typedef struct {
    /*  TODO: 
        Message structure for wrapper
    */
    long mType;
    char msgText[1024];
} message_t;

#define EXIT_MESSAGE "__IPC_EXIT__"

typedef struct {
    int ready;             // set to 1 after semaphore initialization completes
    sem_t mutex;           // protects access to shared message fields
    sem_t full;            // counts available messages
    sem_t empty;           // counts available slots (single-slot buffer)
    size_t length;         // number of bytes in buffer (excluding null terminator)
    int is_exit;           // non-zero when the stored message is the exit signal
    char buffer[1024];     // shared message storage
} shm_mailbox_t;

void send(message_t message, mailbox_t* mailbox_ptr);
