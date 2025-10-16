#include "sender.h"

static double g_sender_elapsed_ns = 0.0;

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

void send(message_t message, mailbox_t *mailbox_ptr)
{
	/*  TODO: 
        1. Use flag to determine the communication method
        2. According to the communication method, send the message
    */

	// (1)
	if (mailbox_ptr == NULL) {
		fprintf(stderr, "[Sender] Invalid mailbox pointer.\n");
		exit(EXIT_FAILURE);
	}

	if (mailbox_ptr->flag == MSG_PASSING) {
		size_t payload_size = strlen(message.msgText);
		if (msgsnd(mailbox_ptr->storage.msqid, &message,
			   payload_size + 1, 0) == -1) {
			perror("msgsnd");
			exit(EXIT_FAILURE);
		}
	} else if (mailbox_ptr->flag == SHARED_MEM) {
		shm_mailbox_t *shared_box =
			(shm_mailbox_t *)mailbox_ptr->storage.shm_addr;
		if (shared_box == NULL) {
			fprintf(stderr,
				"[Sender] Shared memory not attached.\n");
			exit(EXIT_FAILURE);
		}
		while (!shared_box->ready) {
			usleep(1000);
		}

		size_t payload_size = strlen(message.msgText);
		if (payload_size >= sizeof(shared_box->buffer)) {
			payload_size = sizeof(shared_box->buffer) - 1;
		}

		int sem_result = 0;
		do {
			sem_result = sem_wait(&shared_box->empty);
		} while (sem_result == -1 && errno == EINTR);
		if (sem_result == -1) {
			perror("sem_wait(empty)");
			exit(EXIT_FAILURE);
		}

		do {
			sem_result = sem_wait(&shared_box->mutex);
		} while (sem_result == -1 && errno == EINTR);
		if (sem_result == -1) {
			perror("sem_wait(mutex)");
			exit(EXIT_FAILURE);
		}

		memcpy(shared_box->buffer, message.msgText, payload_size);
		shared_box->buffer[payload_size] = '\0';
		shared_box->length = payload_size;
		shared_box->is_exit =
			(strcmp(message.msgText, EXIT_MESSAGE) == 0) ? 1 : 0;

		if (sem_post(&shared_box->mutex) == -1) {
			perror("sem_post(mutex)");
			exit(EXIT_FAILURE);
		}
		if (sem_post(&shared_box->full) == -1) {
			perror("sem_post(full)");
			exit(EXIT_FAILURE);
		}
	} else {
		fprintf(stderr, "[Sender] Unsupported IPC mechanism: %d\n",
			mailbox_ptr->flag);
		exit(EXIT_FAILURE);
	}
}

int main(int argc, char *argv[])
{
	/*  TODO: 
        1) Call send(message, &mailbox) according to the flow in slide 4
        2) Measure the total sending time
        3) Get the mechanism and the input file from command line arguments
            • e.g. ./sender 1 input.txt
                    (1 for Message Passing, 2 for Shared Memory)
        4) Get the messages to be sent from the input file
        5) Print information on the console according to the output format
        6) If the message form the input file is EOF, send an exit message to the receiver.c
        7) Print the total sending time and terminate the sender.c
    */

	struct timespec start_time;
	struct timespec end_time;
	mailbox_t mailbox;
	memset(&mailbox, 0, sizeof(mailbox));

	key_t ipc_key = (key_t)-1;
	int msqid = -1;
	int shmid = -1;
	int created_shared_memory = 0;
	shm_mailbox_t *shared_block = NULL;
	FILE *input_file = NULL;
	int exit_code = EXIT_SUCCESS;
	int exit_sent = 0;

	g_sender_elapsed_ns = 0.0;

	if (argc != 3) {
		fprintf(stderr, "Usage: %s <mechanism> <input_file>\n", argv[0]);
		return EXIT_FAILURE;
	}

	int mechanism = atoi(argv[1]);
	const char *input_path = argv[2];

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
			exit_code = EXIT_FAILURE;
			goto cleanup;
		}

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
		} else {
			while (!shared_block->ready) {
				usleep(1000);
			}
		}

		mailbox.flag = SHARED_MEM;
		mailbox.storage.shm_addr = (char *)shared_block;
	} else {
		fprintf(stderr, "Invalid mechanism type: %d\n", mechanism);
		exit_code = EXIT_FAILURE;
		goto cleanup;
	}

	input_file = fopen(input_path, "r");
	if (input_file == NULL) {
		perror("fopen");
		exit_code = EXIT_FAILURE;
		goto cleanup;
	}

	char line_buffer[sizeof(((message_t *)0)->msgText)];
	message_t message;
	memset(&message, 0, sizeof(message));

	while (fgets(line_buffer, sizeof(line_buffer), input_file) != NULL) {
		size_t len = strcspn(line_buffer, "\n");
		line_buffer[len] = '\0';

		if (strcmp(line_buffer, "EOF") == 0) {
			strncpy(message.msgText, EXIT_MESSAGE,
				sizeof(message.msgText) - 1);
			message.msgText[sizeof(message.msgText) - 1] = '\0';
			message.mType = 2;
			printf("[Sender] Exit token found in input. Notifying receiver.\n");
			exit_sent = 1;
		} else {
			strncpy(message.msgText, line_buffer,
				sizeof(message.msgText) - 1);
			message.msgText[sizeof(message.msgText) - 1] = '\0';
			message.mType = 1;
			printf("\033[92mSending message:\033[0m %s\n", message.msgText);
		}

		if (monotonic_now(&start_time) == -1) {
			exit_code = EXIT_FAILURE;
			goto cleanup;
		}
		send(message, &mailbox);
		if (monotonic_now(&end_time) == -1) {
			exit_code = EXIT_FAILURE;
			goto cleanup;
		}
		g_sender_elapsed_ns += elapsed_ns(&start_time, &end_time);

		if (exit_sent) {
			break;
		}
	}

	if (!exit_sent) {
		strncpy(message.msgText, EXIT_MESSAGE,
			sizeof(message.msgText) - 1);
		message.msgText[sizeof(message.msgText) - 1] = '\0';
		message.mType = 2;
		printf("\033[91mEnd of input file! exit!\033[0m\n");
		if (monotonic_now(&start_time) == -1) {
			exit_code = EXIT_FAILURE;
			goto cleanup;
		}
		send(message, &mailbox);
		if (monotonic_now(&end_time) == -1) {
			exit_code = EXIT_FAILURE;
			goto cleanup;
		}
		g_sender_elapsed_ns += elapsed_ns(&start_time, &end_time);
	}

	printf("Total time taken in sending msg: %.6f s\n",
	       g_sender_elapsed_ns / 1e9);

cleanup:
	if (input_file != NULL) {
		fclose(input_file);
	}

	if (mailbox.flag == SHARED_MEM && shared_block != NULL) {
		if (exit_code != EXIT_SUCCESS && created_shared_memory) {
			sem_destroy(&shared_block->full);
			sem_destroy(&shared_block->empty);
			sem_destroy(&shared_block->mutex);
		}
		shmdt(shared_block);
		if (exit_code != EXIT_SUCCESS && created_shared_memory &&
		    shmid != -1) {
			shmctl(shmid, IPC_RMID, NULL);
		}
	}

	if (mailbox.flag == MSG_PASSING && exit_code != EXIT_SUCCESS &&
	    msqid != -1) {
		msgctl(msqid, IPC_RMID, NULL);
	}

	return exit_code;
}
