/*
 * SPDX-FileCopyrightText: 2025 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <linux/io_uring.h>
#include <fcntl.h>
#include <errno.h>
#include <pthread.h>
#include <urcu.h>
#include <urcu/wfcqueue.h>

#define BUFFER_SIZE 4096
#define QUEUE_DEPTH 1024

struct task {
	char *buffer;
	int bytes_read;
};

// Global wfcqueue
struct cds_wfcq task_queue;

int setup_io_uring(struct io_uring *ring)
{
	if (io_uring_queue_init(QUEUE_DEPTH, ring, 0) < 0) {
		perror("io_uring_queue_init");
		return -1;
	}
	return 0;
}

void *worker_thread(void *arg)
{
	while (1) {
		struct task *task;
		cds_wfcq_dequeue(&task_queue,
				 (void **)&task); // Dequeue from wfcqueue

		if (task) {
			printf("Received: %.*s", task->bytes_read,
			       task->buffer);
			free(task->buffer);
			free(task);
		}
	}
	return NULL;
}

int main()
{
	int listen_fd, ret, i;
	struct sockaddr_in address;
	struct io_uring ring;
	pthread_t thread;

	if (setup_io_uring(&ring) < 0) {
		return 1;
	}

	// Initialize wfcqueue
	cds_wfcq_init(&task_queue);

	listen_fd = socket(AF_INET, SOCK_STREAM, 0);
	if (listen_fd < 0) {
		perror("socket");
		return 1;
	}

	address.sin_family = AF_INET;
	address.sin_addr.s_addr = INADDR_ANY;
	address.sin_port = htons(8080);

	if (bind(listen_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
		perror("bind");
		return 1;
	}

	if (listen(listen_fd, 10) < 0) {
		perror("listen");
		return 1;
	}

	pthread_create(&thread, NULL, worker_thread, NULL);

	struct io_uring_sqe *sqe;
	struct io_uring_cqe *cqe;
	struct sockaddr_in client_address;
	socklen_t client_len = sizeof(client_address);
	int client_fd;

	sqe = io_uring_get_sqe(&ring);
	io_uring_prep_accept(sqe, listen_fd, (struct sockaddr *)&client_address,
			     &client_len, 0);
	io_uring_submit(&ring);

	while (1) {
		io_uring_wait_cqe(&ring, &cqe);
		if (cqe->res < 0) {
			fprintf(stderr, "CQE error: %s\n", strerror(-cqe->res));
		} else {
			if (cqe->flags & IORING_CQE_F_MORE) {
				io_uring_cqe_seen(&ring, cqe);
				continue;
			}

			if (cqe->flags == 0) { // Accept
				client_fd = cqe->res;

				char *buffer = malloc(BUFFER_SIZE);
				if (!buffer) {
					perror("malloc");
					return 1;
				}

				sqe = io_uring_get_sqe(&ring);
				io_uring_prep_read(sqe, client_fd, buffer,
						   BUFFER_SIZE, 0);
				sqe->user_data = (uintptr_t)buffer;
				io_uring_submit(&ring);

			} else { //Read
				int bytes_read = cqe->res;
				char *buffer = (char *)cqe->user_data;

				if (bytes_read > 0) {
					struct task *task =
						malloc(sizeof(struct task));
					task->buffer = buffer;
					task->bytes_read = bytes_read;
					add_task(task);

					sqe = io_uring_get_sqe(&ring);
					io_uring_prep_write(sqe, client_fd,
							    buffer, bytes_read,
							    0);
					sqe->user_data = (uintptr_t)buffer;
					io_uring_submit(&ring);

				} else {
					close(client_fd);
					free((void *)cqe->user_data);
				}
			}
		}
		io_uring_cqe_seen(&ring, cqe);

		sqe = io_uring_get_sqe(&ring);
		io_uring_prep_accept(sqe, listen_fd,
				     (struct sockaddr *)&client_address,
				     &client_len, 0);
		io_uring_submit(&ring);
	}

	io_uring_queue_exit(&ring);
	close(listen_fd);
	cds_wfcq_destroy(&task_queue); // Destroy the queue
	return 0;
}

void add_task(struct task *task)
{
	cds_wfcq_enqueue(&task_queue, task); // Enqueue to wfcqueue
}
