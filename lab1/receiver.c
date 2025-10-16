#include "receiver.h"
#include <stdio.h>

static double g_receiver_elapsed_ns = 0.0;

static inline double elapsed_ns(const struct timespec *start,
				const struct timespec *end)
{
	return ((double)(end->tv_sec - start->tv_sec) * 1e9) +
	       (double)(end->tv_nsec - start->tv_nsec);
}

static inline int monotonic_now(struct timespec *ts)
{
	if (clock_gettime(CLOCK_MONOTONIC, ts) == -1) {
		perror("clock_gettime");
		return -1;
	}
	return 0;
}

void receive(message_t *message_ptr, mailbox_t *mailbox_ptr)
{
	/*  TODO: 
        1. Use flag to determine the communication method
        2. According to the communication method, receive the message
    */

	// (1) 判斷模式
	if (mailbox_ptr == NULL || message_ptr == NULL) {
		fprintf(stderr,
			"[Receiver] Invalid mailbox or message pointer.\n");
		exit(EXIT_FAILURE);
	}

	if (mailbox_ptr->flag == MSG_PASSING) {
		ssize_t received_size =
			msgrcv(mailbox_ptr->storage.msqid, message_ptr,
			       sizeof(message_ptr->msgText), 0, 0);
		if (received_size == -1) {
			perror("msgrcv");
			exit(EXIT_FAILURE);
		}
		if (received_size >= (ssize_t)sizeof(message_ptr->msgText)) {
			message_ptr->msgText[sizeof(message_ptr->msgText) - 1] =
				'\0';
		} else {
			message_ptr->msgText[received_size] = '\0';
		}
	} else if (mailbox_ptr->flag == SHARED_MEM) {
		shm_mailbox_t *shared_box =
			(shm_mailbox_t *)mailbox_ptr->storage.shm_addr;
		if (shared_box == NULL) {
			fprintf(stderr,
				"[Receiver] Shared memory not attached.\n");
			exit(EXIT_FAILURE);
		}

		while (!shared_box->ready) {
			usleep(1000);
		}

		int sem_result = 0;
		do {
			sem_result = sem_wait(&shared_box->full);
		} while (sem_result == -1 && errno == EINTR);
		if (sem_result == -1) {
			perror("sem_wait(full)");
			exit(EXIT_FAILURE);
		}

		do {
			sem_result = sem_wait(&shared_box->mutex);
		} while (sem_result == -1 && errno == EINTR);
		if (sem_result == -1) {
			perror("sem_wait(mutex)");
			exit(EXIT_FAILURE);
		}

		size_t copy_length = shared_box->length;
		if (copy_length >= sizeof(message_ptr->msgText)) {
			copy_length = sizeof(message_ptr->msgText) - 1;
		}
		memcpy(message_ptr->msgText, shared_box->buffer, copy_length);
		message_ptr->msgText[copy_length] = '\0';
		message_ptr->mType = shared_box->is_exit ? 2 : 1;

		shared_box->length = 0;
		shared_box->is_exit = 0;
		shared_box->buffer[0] = '\0';

		if (sem_post(&shared_box->mutex) == -1) {
			perror("sem_post(mutex)");
			exit(EXIT_FAILURE);
		}
		if (sem_post(&shared_box->empty) == -1) {
			perror("sem_post(empty)");
			exit(EXIT_FAILURE);
		}
	} else {
		fprintf(stderr, "[Receiver] Unsupported IPC mechanism: %d\n",
			mailbox_ptr->flag);
		exit(EXIT_FAILURE);
	}
}

int main(int argc, char *argv[])
{
	/*  TODO: 
        1) Call receive(&message, &mailbox) according to the flow in slide 4
        2) Measure the total receiving time
        3) Get the mechanism from command line arguments
            • e.g. ./receiver 1
        4) Print information on the console according to the output format
        5) If the exit message is received, print the total receiving time and terminate the receiver.c
    */

	struct timespec start_time;
	struct timespec end_time;
	mailbox_t mailbox;
	memset(&mailbox, 0, sizeof(mailbox));

	key_t ipc_key = (key_t)-1;
	int msqid = -1;
	int shmid = -1;
	int created_shared_memory = 0;
	int sem_ready = 0;
	shm_mailbox_t *shared_block = NULL;
	int exit_code = EXIT_SUCCESS;
	int exit_received = 0;

	g_receiver_elapsed_ns = 0.0;

	if (argc != 2) {
		fprintf(stderr, "Usage: %s <mechanism>\n", argv[0]);
		return EXIT_FAILURE;
	}

	int mechanism = atoi(argv[1]);

	if (mechanism == MSG_PASSING) {
		printf("\033[92mMessage Passing\033[0m\n");
		ipc_key = ftok(".", 'Q');
		if (ipc_key == -1) {
			perror("ftok");
			exit_code = EXIT_FAILURE;
			goto cleanup;
		}
		msqid = msgget(ipc_key, IPC_CREAT | 0666);
		if (msqid == -1) {
			perror("msgget");
			exit_code = EXIT_FAILURE;
			goto cleanup;
		}
		mailbox.flag = MSG_PASSING;
		mailbox.storage.msqid = msqid;
	} else if (mechanism == SHARED_MEM) {
		printf("\033[92mShared Memory\033[0m\n");
		ipc_key = ftok(".", 'S');
		if (ipc_key == -1) {
			perror("ftok");
			exit_code = EXIT_FAILURE;
			goto cleanup;
		}
		shmid = shmget(ipc_key, sizeof(shm_mailbox_t),
			       IPC_CREAT | IPC_EXCL | 0666);
		if (shmid == -1) {
			if (errno != EEXIST) {
				perror("shmget");
				exit_code = EXIT_FAILURE;
				goto cleanup;
			}
			shmid = shmget(ipc_key, sizeof(shm_mailbox_t), 0666);
			if (shmid == -1) {
				perror("shmget");
				exit_code = EXIT_FAILURE;
				goto cleanup;
			}
		} else {
			created_shared_memory = 1;
		}

		shared_block = (shm_mailbox_t *)shmat(shmid, NULL, 0);
		if (shared_block == (void *)-1) {
			perror("shmat");
			shared_block = NULL;
			if (created_shared_memory && shmid != -1) {
				shmctl(shmid, IPC_RMID, NULL);
			}
			exit_code = EXIT_FAILURE;
			goto cleanup;
		}

		mailbox.flag = SHARED_MEM;
		mailbox.storage.shm_addr = (char *)shared_block;

		if (created_shared_memory) {
			memset(shared_block, 0, sizeof(*shared_block));
			if (sem_init(&shared_block->mutex, 1, 1) == -1) {
				perror("sem_init(mutex)");
				exit_code = EXIT_FAILURE;
				goto cleanup;
			}
			if (sem_init(&shared_block->empty, 1, 1) == -1) {
				perror("sem_init(empty)");
				sem_destroy(&shared_block->mutex);
				exit_code = EXIT_FAILURE;
				goto cleanup;
			}
			if (sem_init(&shared_block->full, 1, 0) == -1) {
				perror("sem_init(full)");
				sem_destroy(&shared_block->empty);
				sem_destroy(&shared_block->mutex);
				exit_code = EXIT_FAILURE;
				goto cleanup;
			}
			shared_block->ready = 1;
			sem_ready = 1;
		} else {
			while (!shared_block->ready) {
				usleep(1000);
			}
			sem_ready = 1;
		}

	} else {
		fprintf(stderr, "Invalid mechanism type: %d\n", mechanism);
		exit_code = EXIT_FAILURE;
		goto cleanup;
	}

	message_t message;
	memset(&message, 0, sizeof(message));

	while (!exit_received) {
		if (monotonic_now(&start_time) == -1) {
			exit_code = EXIT_FAILURE;
			goto cleanup;
		}
		receive(&message, &mailbox);
		if (monotonic_now(&end_time) == -1) {
			exit_code = EXIT_FAILURE;
			goto cleanup;
		}
		g_receiver_elapsed_ns += elapsed_ns(&start_time, &end_time);

		if (strcmp(message.msgText, EXIT_MESSAGE) == 0) {
			printf("\033[91mSender exit!\033[0m\n");
			exit_received = 1;
		} else {
			printf("\033[92mReceiving message:\033[0m %s\n",
			       message.msgText);
		}
	}

	printf("Total time taken in receiving msg: %.6f s\n",
	       g_receiver_elapsed_ns / 1e9);

cleanup:
	if (mailbox.flag == MSG_PASSING && msqid != -1 &&
	    exit_code == EXIT_SUCCESS) {
		if (msgctl(msqid, IPC_RMID, NULL) == -1) {
			perror("msgctl");
		}
	}

	if (mailbox.flag == SHARED_MEM && shared_block != NULL) {
		int should_remove = exit_code == EXIT_SUCCESS ||
				    created_shared_memory;
		if (should_remove && sem_ready) {
			sem_destroy(&shared_block->full);
			sem_destroy(&shared_block->empty);
			sem_destroy(&shared_block->mutex);
		}
		shmdt(shared_block);
		if (should_remove && shmid != -1) {
			if (shmctl(shmid, IPC_RMID, NULL) == -1) {
				perror("shmctl");
			}
		}
	}

	return exit_code;
}
