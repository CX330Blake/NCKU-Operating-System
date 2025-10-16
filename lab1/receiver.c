#include "receiver.h"
#include <bits/time.h>
#include <stdio.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>

struct timespec start, end;
double time_taken;

static inline void time_start()
{
	clock_gettime(CLOCK_MONOTONIC, &start);
}

static inline void time_end()
{
	clock_gettime(CLOCK_MONOTONIC, &end);
}

void recv_via_msg_passing(message_t *message_ptr, mailbox_t *mailbox_ptr)
{
	// Message Queue: poll using IPC_NOWAIT; measure only the successful msgrcv() call
	for (;;) {
		time_start();
		ssize_t received_size = msgrcv(
			mailbox_ptr->storage.msqid, message_ptr,
			sizeof(message_ptr->msgText), 0,
			IPC_NOWAIT); // Non-blocking: return immediately if no message
		time_end();

		if (received_size == -1) {
			if (errno == ENOMSG) {
				// No message yet → do not count this attempt
				struct timespec ts = {
					.tv_sec = 0,
					.tv_nsec = 1 * 1000 * 1000 // sleep 1ms
				};
				nanosleep(&ts, NULL);
				continue;
			}
			perror("msgrcv");
			exit(EXIT_FAILURE);
		}

		time_taken += ((end.tv_sec - start.tv_sec) +
			       (end.tv_nsec - start.tv_nsec) / 1e9);

		// Null-terminate the received text
		if (received_size >= (ssize_t)sizeof(message_ptr->msgText)) {
			message_ptr->msgText[sizeof(message_ptr->msgText) - 1] =
				'\0';
		} else {
			message_ptr->msgText[received_size] = '\0';
		}
		break;
	}
}

void recv_via_memory_sharing(message_t *message_ptr, mailbox_t *mailbox_ptr)
{
	// Shared Memory: measure only actual memory access, not semaphore waits
	shm_mailbox_t *shared_box =
		(shm_mailbox_t *)mailbox_ptr->storage.shm_addr;
	if (shared_box == NULL) {
		fprintf(stderr, "[Receiver] Shared memory not attached.\n");
		exit(EXIT_FAILURE);
	}

	// Wait until the shared memory is ready (not counted)
	while (!shared_box->ready) {
		usleep(1000);
	}

	// Wait for "full" semaphore (producer ready) — not counted
	int sem_result = 0;
	do {
		sem_result = sem_wait(&shared_box->full);
	} while (sem_result == -1 && errno == EINTR);
	if (sem_result == -1) {
		perror("sem_wait(full)");
		exit(EXIT_FAILURE);
	}

	// Enter critical section — waiting time not measured
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

	time_start();

	memcpy(message_ptr->msgText, shared_box->buffer, copy_length);
	message_ptr->msgText[copy_length] = '\0';
	message_ptr->mType = shared_box->is_exit ? 2 : 1;
	// Clear shared buffer flags (also part of memory access)
	shared_box->length = 0;
	shared_box->is_exit = 0;
	shared_box->buffer[0] = '\0';

	time_end();

	// Exit critical section and signal the empty semaphore
	if (sem_post(&shared_box->mutex) == -1) {
		perror("sem_post(mutex)");
		exit(EXIT_FAILURE);
	}
	if (sem_post(&shared_box->empty) == -1) {
		perror("sem_post(empty)");
		exit(EXIT_FAILURE);
	}
	time_taken += ((end.tv_sec - start.tv_sec) +
		       (end.tv_nsec - start.tv_nsec) / 1e9);
}

void receive(message_t *message_ptr, mailbox_t *mailbox_ptr)
{
	/*
		TODO:
		1. Use flag to determine the communication mechanism.
		2. Receive the message according to the chosen mechanism.
	*/

	switch (mailbox_ptr->flag) {
	case MSG_PASSING: {
		recv_via_msg_passing(message_ptr, mailbox_ptr);
		break;
	}
	case SHARED_MEM: {
		recv_via_memory_sharing(message_ptr, mailbox_ptr);
		break;
	}
	default:
		fprintf(stderr, "[Receiver] Unsupported IPC mechanism: %d\n",
			mailbox_ptr->flag);
		exit(EXIT_FAILURE);
	}
}

int main(int argc, char *argv[])
{
	/*
		TODO:
		1) Call receive(&message, &mailbox) according to the flow in slide 4.
		2) Total receiving time is accumulated inside receive() (g_receiver_elapsed_ns).
		3) Read IPC mechanism from command line argument (e.g. ./receiver 1).
		4) Print information to the console according to the required output format.
		5) When the exit message is received, print total time and terminate.
	*/

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
			// Wait for shared memory initialization
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
		// Precise measurement: receive() internally updates g_receiver_elapsed_ns
		receive(&message, &mailbox);

		if (strcmp(message.msgText, EXIT_MESSAGE) == 0) {
			printf("\033[91mSender exit!\033[0m\n");
			exit_received = 1;
		} else {
			printf("\033[92mReceiving message:\033[0m %s\n",
			       message.msgText);
		}
	}

	printf("Total time taken in receiving msg: %.6f s\n", time_taken);

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
