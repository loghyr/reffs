/*
 * SPDX-FileCopyrightText: 2025 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <liburing.h>
#include <rocksdb/c.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>

/* Thread pool configuration */
#define THREAD_POOL_MIN_SIZE 2
#define THREAD_POOL_MAX_SIZE 32
#define DEFAULT_QUEUE_CAPACITY 1024

/* Forward declarations */
struct reffs_kv_op;
struct reffs_kv_store;
struct reffs_thread_pool;
struct reffs_task_queue;
struct reffs_kv_batch;

/* Callback and codec function types */
typedef void (*reffs_kv_completion_cb)(struct reffs_kv_op *op, int status);
typedef int (*reffs_key_encoder)(const void *key, size_t key_len,
				 void **encoded_key, size_t *encoded_key_len);
typedef int (*reffs_key_decoder)(const void *encoded_key,
				 size_t encoded_key_len, void **key,
				 size_t *key_len);
typedef int (*reffs_value_encoder)(const void *value, size_t value_len,
				   void **encoded_value,
				   size_t *encoded_value_len);
typedef int (*reffs_value_decoder)(const void *encoded_value,
				   size_t encoded_value_len, void **value,
				   size_t *value_len);

/* Operation types */
enum reffs_kv_op_type {
    REFFS_KV_OP_GET,
    REFFS_KV_OP_PUT,
    REFFS_KV_OP_DELETE,
    REFFS_KV_OP_RANGE,
    REFFS_KV_OP_BATCH,
    /* Add more operation types as needed */
}

int kv_close(struct reffs_kv_store *rks)
{
	if (!rks) {
		return EINVAL;
	}

	/* Clean up io_uring if enabled */
	if (rks->rks_flags & RKS_ASYNC_MODE) {
		kv_cleanup_io_uring(rks);
	}

	/* Clean up RocksDB resources */
	if (rks->rks_kv) {
		rocksdb_close(rks->rks_kv);
		rks->rks_kv = NULL;
	}

	if (rks->rks_wopt) {
		rocksdb_writeoptions_destroy(rks->rks_wopt);
		rks->rks_wopt = NULL;
	}

	if (rks->rks_ropt) {
		rocksdb_readoptions_destroy(rks->rks_ropt);
		rks->rks_ropt = NULL;
	}

	return 0;
}

/* Extended open function with encoder/decoder support */
int kv_open_extended(const char *kv_path, struct reffs_kv_store *rks,
		     reffs_key_encoder key_encoder,
		     reffs_key_decoder key_decoder,
		     reffs_value_encoder value_encoder,
		     reffs_value_decoder value_decoder,
		     unsigned int queue_depth, unsigned int thread_count)
{
	int ret = kv_open(kv_path, rks);
	if (ret != 0) {
		return ret;
	}

	/* Set the encoders and decoders */
	rks->rks_key_encoder = key_encoder;
	rks->rks_key_decoder = key_decoder;
	rks->rks_value_encoder = value_encoder;
	rks->rks_value_decoder = value_decoder;

	/* Initialize io_uring if queue_depth > 0 */
	if (queue_depth > 0) {
		ret = kv_init_io_uring(rks, queue_depth, thread_count);
		if (ret != 0) {
			kv_close(rks);
			return ret;
		}
	}

	return 0;
}

/* Example callback for get operation */
void example_get_callback(struct reffs_kv_op *op, int status)
{
	if (status == 0) {
		printf("Get operation completed successfully. Key: %.*s, Value: %.*s\n",
		       (int)op->rkop_key_len, (char *)op->rkop_key,
		       (int)op->rkop_value_len, (char *)op->rkop_value);
	} else {
		printf("Get operation failed with status: %d\n", status);
	}

	/* Clean up the operation */
	if (op->rkop_value) {
		free(op->rkop_value);
	}
	free(op);
}

/* Example callback for put operation */
void example_put_callback(struct reffs_kv_op *op, int status)
{
	if (status == 0) {
		printf("Put operation completed successfully. Key: %.*s\n",
		       (int)op->rkop_key_len, (char *)op->rkop_key);
	} else {
		printf("Put operation failed with status: %d\n", status);
	}

	/* Clean up the operation */
	free(op);
}

/* Example callback for batch operation */
void example_batch_callback(struct reffs_kv_op *op, int status)
{
	if (status == 0) {
		printf("Batch operation completed successfully. Operations: %zu\n",
		       op->rkop_batch->operation_count);
	} else {
		printf("Batch operation failed with status: %d\n", status);
	}

	/* Clean up the batch and operation */
	kv_batch_destroy(op->rkop_batch);
	free(op);
}

/* Example usage functions */
int example_async_get(struct reffs_kv_store *rks, const char *key)
{
	if (!(rks->rks_flags & RKS_ASYNC_MODE)) {
		return ENOTSUP; /* Async mode not enabled */
	}

	/* Create a copy of the key since we need it to outlive the function call */
	char *key_copy = strdup(key);
	if (!key_copy) {
		return ENOMEM;
	}

	struct reffs_kv_op *op =
		kv_op_create(rks, REFFS_KV_OP_GET, key_copy, strlen(key_copy),
			     NULL, 0, /* No value for GET operation */
			     example_get_callback, NULL);

	if (!op) {
		free(key_copy);
		return ENOMEM;
	}

	return kv_submit_get(rks, op);
}

int example_async_put(struct reffs_kv_store *rks, const char *key,
		      const char *value)
{
	if (!(rks->rks_flags & RKS_ASYNC_MODE)) {
		return ENOTSUP; /* Async mode not enabled */
	}

	/* Create copies of key and value */
	char *key_copy = strdup(key);
	char *value_copy = strdup(value);

	if (!key_copy || !value_copy) {
		free(key_copy);
		free(value_copy);
		return ENOMEM;
	}

	struct reffs_kv_op *op = kv_op_create(rks, REFFS_KV_OP_PUT, key_copy,
					      strlen(key_copy), value_copy,
					      strlen(value_copy),
					      example_put_callback, NULL);

	if (!op) {
		free(key_copy);
		free(value_copy);
		return ENOMEM;
	}

	return kv_submit_put(rks, op);
}

int example_async_batch(struct reffs_kv_store *rks, const char **keys,
			const char **values, size_t count)
{
	if (!(rks->rks_flags & RKS_ASYNC_MODE)) {
		return ENOTSUP; /* Async mode not enabled */
	}

	struct reffs_kv_batch *batch = kv_batch_create();
	if (!batch) {
		return ENOMEM;
	}

	/* Add operations to the batch */
	for (size_t i = 0; i < count; i++) {
		int ret = kv_batch_put(batch, rks, (void *)keys[i],
				       strlen(keys[i]), (void *)values[i],
				       strlen(values[i]));
		if (ret != 0) {
			kv_batch_destroy(batch);
			return ret;
		}
	}

	/* Submit the batch */
	return kv_submit_batch(rks, batch, example_batch_callback, NULL);
}

/* Example main function demonstrating usage */
int example_main()
{
	struct reffs_kv_store store = {
		.rks_name = "example_db", .rks_flags = 0 /* Read-write mode */
	};

	int ret = kv_open_extended("/tmp/rocksdb_test", &store, NULL, NULL,
				   NULL, NULL, /* No encoders/decoders */
				   128, /* io_uring queue depth */
				   4 /* Thread pool size */
	);

	if (ret != 0) {
		fprintf(stderr, "Failed to open KV store: %s\n", strerror(ret));
		return 1;
	}

	/* Example operations */
	example_async_put(&store, "key1", "value1");
	example_async_put(&store, "key2", "value2");
	example_async_get(&store, "key1");

	/* Example batch operation */
	const char *batch_keys[] = { "batch1", "batch2", "batch3" };
	const char *batch_values[] = { "value1", "value2", "value3" };
	example_async_batch(&store, batch_keys, batch_values, 3);

	/* Sleep to allow async operations to complete */
	sleep(1);

	/* Close the store */
	kv_close(&store);

	return 0;
};

/* Task queue structure */
struct reffs_task_queue {
	struct reffs_kv_op **tasks;
	size_t capacity;
	size_t head;
	size_t tail;
	size_t count;
	pthread_mutex_t lock;
	pthread_cond_t not_empty;
	pthread_cond_t not_full;
	bool shutdown;
};

/* Thread pool structure */
struct reffs_thread_pool {
	pthread_t *threads;
	size_t thread_count;
	struct reffs_task_queue queue;
	struct reffs_kv_store *rks; /* Reference to the store this pool serves */
	pthread_t completion_thread; /* Thread for handling completions */
	bool running;
};

/* Batch operation structure */
struct reffs_kv_batch {
	rocksdb_writebatch_t *batch;
	size_t operation_count;
};

/* Operation structure - one per async operation */
struct reffs_kv_op {
	enum reffs_kv_op_type rkop_type;
	struct reffs_kv_store *rkop_store;

	/* Key and value buffers */
	void *rkop_key;
	size_t rkop_key_len;
	void *rkop_value;
	size_t rkop_value_len;

	/* For range operations */
	void *rkop_end_key;
	size_t rkop_end_key_len;

	/* For batch operations */
	struct reffs_kv_batch *rkop_batch;

	/* User data and callback */
	void *rkop_user_data;
	reffs_kv_completion_cb rkop_completion_cb;

	/* For internal use */
	struct io_uring_sqe *rkop_sqe;
	int rkop_status;
};

/* Store structure - one per RocksDB instance */
struct reffs_kv_store {
	rocksdb_t *rks_kv;
	rocksdb_readoptions_t *rks_ropt;
	rocksdb_writeoptions_t *rks_wopt;
	const char *rks_name;

	/* io_uring integration */
	struct io_uring *rks_ring;

	/* Thread pool for async operations */
	struct reffs_thread_pool *rks_thread_pool;

	/* Encoders and decoders */
	reffs_key_encoder rks_key_encoder;
	reffs_key_decoder rks_key_decoder;
	reffs_value_encoder rks_value_encoder;
	reffs_value_decoder rks_value_decoder;

	/* Flags and state */
#define RKS_READ_ONLY (1 << 0)
#define RKS_ASYNC_MODE (1 << 1)
#define RKS_SHUTDOWN (1 << 2)
	uint32_t rks_flags;

	/* Statistics and monitoring */
	uint64_t rks_ops_submitted;
	uint64_t rks_ops_completed;
	uint64_t rks_op_errors;
};

/* Task queue functions */
int task_queue_init(struct reffs_task_queue *queue, size_t capacity)
{
	queue->tasks = calloc(capacity, sizeof(struct reffs_kv_op *));
	if (!queue->tasks) {
		return ENOMEM;
	}

	queue->capacity = capacity;
	queue->head = 0;
	queue->tail = 0;
	queue->count = 0;
	queue->shutdown = false;

	pthread_mutex_init(&queue->lock, NULL);
	pthread_cond_init(&queue->not_empty, NULL);
	pthread_cond_init(&queue->not_full, NULL);

	return 0;
}

void task_queue_destroy(struct reffs_task_queue *queue)
{
	pthread_mutex_lock(&queue->lock);

	/* Free any remaining tasks */
	if (queue->count > 0) {
		for (size_t i = 0; i < queue->count; i++) {
			size_t idx = (queue->head + i) % queue->capacity;
			/* Note: In a real implementation, you might want to complete
             * these tasks with an error status */
			free(queue->tasks[idx]);
		}
	}

	free(queue->tasks);
	queue->tasks = NULL;
	queue->capacity = 0;
	queue->count = 0;

	pthread_mutex_unlock(&queue->lock);
	pthread_mutex_destroy(&queue->lock);
	pthread_cond_destroy(&queue->not_empty);
	pthread_cond_destroy(&queue->not_full);
}

int task_queue_push(struct reffs_task_queue *queue, struct reffs_kv_op *task)
{
	pthread_mutex_lock(&queue->lock);

	/* Wait while the queue is full and not shutting down */
	while (queue->count == queue->capacity && !queue->shutdown) {
		pthread_cond_wait(&queue->not_full, &queue->lock);
	}

	/* Check if we're shutting down */
	if (queue->shutdown) {
		pthread_mutex_unlock(&queue->lock);
		return ECANCELED;
	}

	/* Add the task to the queue */
	queue->tasks[queue->tail] = task;
	queue->tail = (queue->tail + 1) % queue->capacity;
	queue->count++;

	/* Signal that the queue is not empty */
	pthread_cond_signal(&queue->not_empty);
	pthread_mutex_unlock(&queue->lock);

	return 0;
}

struct reffs_kv_op *task_queue_pop(struct reffs_task_queue *queue)
{
	pthread_mutex_lock(&queue->lock);

	/* Wait while the queue is empty and not shutting down */
	while (queue->count == 0 && !queue->shutdown) {
		pthread_cond_wait(&queue->not_empty, &queue->lock);
	}

	/* Check if we're shutting down and the queue is empty */
	if (queue->count == 0) {
		pthread_mutex_unlock(&queue->lock);
		return NULL;
	}

	/* Get the task from the queue */
	struct reffs_kv_op *task = queue->tasks[queue->head];
	queue->head = (queue->head + 1) % queue->capacity;
	queue->count--;

	/* Signal that the queue is not full */
	pthread_cond_signal(&queue->not_full);
	pthread_mutex_unlock(&queue->lock);

	return task;
}

/* Completion handler thread function */
void *completion_handler_thread(void *arg)
{
	struct reffs_thread_pool *pool = (struct reffs_thread_pool *)arg;
	struct reffs_kv_store *rks = pool->rks;
	struct io_uring_cqe *cqe;

	while (pool->running) {
		int ret = io_uring_wait_cqe(rks->rks_ring, &cqe);
		if (ret < 0) {
			/* Handle error */
			if (ret == -EINTR) {
				/* Interrupted, just retry */
				continue;
			}
			fprintf(stderr, "io_uring_wait_cqe error: %s\n",
				strerror(-ret));
			continue;
		}

		struct reffs_kv_op *op = io_uring_cqe_get_data(cqe);
		if (!op) {
			io_uring_cqe_seen(rks->rks_ring, cqe);
			continue;
		}

		/* Execute the callback */
		if (op->rkop_completion_cb) {
			op->rkop_completion_cb(op, op->rkop_status);
		}

		io_uring_cqe_seen(rks->rks_ring, cqe);
		rks->rks_ops_completed++;
	}

	return NULL;
}

/* Worker thread function */
void *thread_pool_worker(void *arg)
{
	struct reffs_thread_pool *pool = (struct reffs_thread_pool *)arg;
	struct reffs_kv_store *rks = pool->rks;

	while (pool->running) {
		/* Get a task from the queue */
		struct reffs_kv_op *op = task_queue_pop(&pool->queue);
		if (!op) {
			/* Queue is empty and shutdown was signaled */
			break;
		}

		/* Process the operation */
		char *err = NULL;

		switch (op->rkop_type) {
		case REFFS_KV_OP_GET: {
			size_t val_len;
			void *encoded_key = op->rkop_key;
			size_t encoded_key_len = op->rkop_key_len;

			if (rks->rks_key_encoder) {
				int ret = rks->rks_key_encoder(
					op->rkop_key, op->rkop_key_len,
					&encoded_key, &encoded_key_len);
				if (ret != 0) {
					op->rkop_status = ret;
					break;
				}
			}

			char *result = rocksdb_get(rks->rks_kv, rks->rks_ropt,
						   encoded_key, encoded_key_len,
						   &val_len, &err);

			/* Clean up encoded key if needed */
			if (encoded_key != op->rkop_key &&
			    rks->rks_key_encoder) {
				free(encoded_key);
			}

			if (err != NULL) {
				op->rkop_status = EIO;
				fprintf(stderr, "RocksDB get error: %s\n", err);
				free(err);
			} else if (result) {
				/* Decode the result if needed */
				if (rks->rks_value_decoder) {
					void *decoded_value;
					size_t decoded_value_len;
					int ret = rks->rks_value_decoder(
						result, val_len, &decoded_value,
						&decoded_value_len);

					if (ret == 0) {
						op->rkop_value = decoded_value;
						op->rkop_value_len =
							decoded_value_len;
						free(result); /* Free the original result after decoding */
					} else {
						op->rkop_status = ret;
						free(result);
					}
				} else {
					op->rkop_value = result;
					op->rkop_value_len = val_len;
				}
				op->rkop_status = 0;
			} else {
				/* Key not found */
				op->rkop_status = ENOENT;
			}
			break;
		}

		case REFFS_KV_OP_PUT: {
			void *encoded_key = op->rkop_key;
			size_t encoded_key_len = op->rkop_key_len;
			void *encoded_value = op->rkop_value;
			size_t encoded_value_len = op->rkop_value_len;

			/* Encode key if needed */
			if (rks->rks_key_encoder) {
				int ret = rks->rks_key_encoder(
					op->rkop_key, op->rkop_key_len,
					&encoded_key, &encoded_key_len);
				if (ret != 0) {
					op->rkop_status = ret;
					break;
				}
			}

			/* Encode value if needed */
			if (rks->rks_value_encoder) {
				int ret = rks->rks_value_encoder(
					op->rkop_value, op->rkop_value_len,
					&encoded_value, &encoded_value_len);
				if (ret != 0) {
					if (encoded_key != op->rkop_key &&
					    rks->rks_key_encoder) {
						free(encoded_key);
					}
					op->rkop_status = ret;
					break;
				}
			}

			rocksdb_put(rks->rks_kv, rks->rks_wopt, encoded_key,
				    encoded_key_len, encoded_value,
				    encoded_value_len, &err);

			/* Clean up encoded buffers if needed */
			if (encoded_key != op->rkop_key &&
			    rks->rks_key_encoder) {
				free(encoded_key);
			}
			if (encoded_value != op->rkop_value &&
			    rks->rks_value_encoder) {
				free(encoded_value);
			}

			if (err != NULL) {
				op->rkop_status = EIO;
				fprintf(stderr, "RocksDB put error: %s\n", err);
				free(err);
			} else {
				op->rkop_status = 0;
			}
			break;
		}

		case REFFS_KV_OP_DELETE: {
			void *encoded_key = op->rkop_key;
			size_t encoded_key_len = op->rkop_key_len;

			if (rks->rks_key_encoder) {
				int ret = rks->rks_key_encoder(
					op->rkop_key, op->rkop_key_len,
					&encoded_key, &encoded_key_len);
				if (ret != 0) {
					op->rkop_status = ret;
					break;
				}
			}

			rocksdb_delete(rks->rks_kv, rks->rks_wopt, encoded_key,
				       encoded_key_len, &err);

			if (encoded_key != op->rkop_key &&
			    rks->rks_key_encoder) {
				free(encoded_key);
			}

			if (err != NULL) {
				op->rkop_status = EIO;
				fprintf(stderr, "RocksDB delete error: %s\n",
					err);
				free(err);
			} else {
				op->rkop_status = 0;
			}
			break;
		}

		case REFFS_KV_OP_BATCH: {
			if (!op->rkop_batch || !op->rkop_batch->batch) {
				op->rkop_status = EINVAL;
				break;
			}

			rocksdb_write(rks->rks_kv, rks->rks_wopt,
				      op->rkop_batch->batch, &err);

			if (err != NULL) {
				op->rkop_status = EIO;
				fprintf(stderr,
					"RocksDB batch write error: %s\n", err);
				free(err);
			} else {
				op->rkop_status = 0;
			}
			break;
		}

		default:
			op->rkop_status =
				ENOSYS; /* Operation not implemented */
			break;
		}

		/* Now submit a completion event back through io_uring */
		struct io_uring_sqe *sqe = io_uring_get_sqe(rks->rks_ring);
		if (sqe) {
			io_uring_prep_nop(sqe);
			io_uring_sqe_set_data(sqe, op);
			io_uring_submit(rks->rks_ring);
		} else {
			/* Failed to get SQE, handle this case */
			/* In a real implementation, you'd want a retry mechanism */
			if (op->rkop_completion_cb) {
				op->rkop_completion_cb(op, EAGAIN);
			}
		}
	}

	return NULL;
}

/* Thread pool functions */
int thread_pool_init(struct reffs_thread_pool *pool, struct reffs_kv_store *rks,
		     size_t thread_count, size_t queue_capacity)
{
	int ret;

	if (thread_count < THREAD_POOL_MIN_SIZE) {
		thread_count = THREAD_POOL_MIN_SIZE;
	} else if (thread_count > THREAD_POOL_MAX_SIZE) {
		thread_count = THREAD_POOL_MAX_SIZE;
	}

	pool->threads = calloc(thread_count, sizeof(pthread_t));
	if (!pool->threads) {
		return ENOMEM;
	}

	pool->thread_count = thread_count;
	pool->rks = rks;
	pool->running = true;

	ret = task_queue_init(&pool->queue, queue_capacity);
	if (ret != 0) {
		free(pool->threads);
		return ret;
	}

	/* Create the completion handler thread */
	if (pthread_create(&pool->completion_thread, NULL,
			   completion_handler_thread, pool) != 0) {
		task_queue_destroy(&pool->queue);
		free(pool->threads);
		return EAGAIN;
	}

	/* Create the worker threads */
	for (size_t i = 0; i < thread_count; i++) {
		if (pthread_create(&pool->threads[i], NULL, thread_pool_worker,
				   pool) != 0) {
			/* Failed to create thread */
			/* Clean up previously created threads */
			pool->running = false;
			pool->queue.shutdown = true;
			pthread_cond_broadcast(&pool->queue.not_empty);

			pthread_join(pool->completion_thread, NULL);

			for (size_t j = 0; j < i; j++) {
				pthread_join(pool->threads[j], NULL);
			}

			task_queue_destroy(&pool->queue);
			free(pool->threads);
			return EAGAIN;
		}
	}

	return 0;
}

void thread_pool_shutdown(struct reffs_thread_pool *pool)
{
	/* Signal all threads to stop */
	pool->running = false;
	pthread_mutex_lock(&pool->queue.lock);
	pool->queue.shutdown = true;
	pthread_cond_broadcast(&pool->queue.not_empty);
	pthread_mutex_unlock(&pool->queue.lock);

	/* Wait for all worker threads to finish */
	for (size_t i = 0; i < pool->thread_count; i++) {
		pthread_join(pool->threads[i], NULL);
	}

	/* Wait for completion thread to finish */
	pthread_join(pool->completion_thread, NULL);

	/* Clean up resources */
	task_queue_destroy(&pool->queue);
	free(pool->threads);
	pool->threads = NULL;
	pool->thread_count = 0;
}

/* Operation creation and management */
struct reffs_kv_op *kv_op_create(struct reffs_kv_store *rks,
				 enum reffs_kv_op_type type, void *key,
				 size_t key_len, void *value, size_t value_len,
				 reffs_kv_completion_cb cb, void *user_data)
{
	struct reffs_kv_op *op = calloc(1, sizeof(struct reffs_kv_op));
	if (!op) {
		return NULL;
	}

	op->rkop_type = type;
	op->rkop_store = rks;
	op->rkop_key = key;
	op->rkop_key_len = key_len;
	op->rkop_value = value;
	op->rkop_value_len = value_len;
	op->rkop_completion_cb = cb;
	op->rkop_user_data = user_data;

	return op;
}

/* Batch operations */
struct reffs_kv_batch *kv_batch_create()
{
	struct reffs_kv_batch *batch = malloc(sizeof(struct reffs_kv_batch));
	if (!batch) {
		return NULL;
	}

	batch->batch = rocksdb_writebatch_create();
	if (!batch->batch) {
		free(batch);
		return NULL;
	}

	batch->operation_count = 0;
	return batch;
}

void kv_batch_destroy(struct reffs_kv_batch *batch)
{
	if (batch) {
		if (batch->batch) {
			rocksdb_writebatch_destroy(batch->batch);
		}
		free(batch);
	}
}

int kv_batch_put(struct reffs_kv_batch *batch, struct reffs_kv_store *rks,
		 void *key, size_t key_len, void *value, size_t value_len)
{
	void *encoded_key = key;
	size_t encoded_key_len = key_len;
	void *encoded_value = value;
	size_t encoded_value_len = value_len;
	int ret = 0;

	/* Encode key if needed */
	if (rks->rks_key_encoder) {
		ret = rks->rks_key_encoder(key, key_len, &encoded_key,
					   &encoded_key_len);
		if (ret != 0) {
			return ret;
		}
	}

	/* Encode value if needed */
	if (rks->rks_value_encoder) {
		ret = rks->rks_value_encoder(value, value_len, &encoded_value,
					     &encoded_value_len);
		if (ret != 0) {
			if (encoded_key != key && rks->rks_key_encoder) {
				free(encoded_key);
			}
			return ret;
		}
	}

	/* Add to batch */
	rocksdb_writebatch_put(batch->batch, encoded_key, encoded_key_len,
			       encoded_value, encoded_value_len);

	/* Clean up encoded buffers if needed */
	if (encoded_key != key && rks->rks_key_encoder) {
		free(encoded_key);
	}
	if (encoded_value != value && rks->rks_value_encoder) {
		free(encoded_value);
	}

	batch->operation_count++;
	return 0;
}

int kv_batch_delete(struct reffs_kv_batch *batch, struct reffs_kv_store *rks,
		    void *key, size_t key_len)
{
	void *encoded_key = key;
	size_t encoded_key_len = key_len;
	int ret = 0;

	/* Encode key if needed */
	if (rks->rks_key_encoder) {
		ret = rks->rks_key_encoder(key, key_len, &encoded_key,
					   &encoded_key_len);
		if (ret != 0) {
			return ret;
		}
	}

	/* Add to batch */
	rocksdb_writebatch_delete(batch->batch, encoded_key, encoded_key_len);

	/* Clean up encoded key if needed */
	if (encoded_key != key && rks->rks_key_encoder) {
		free(encoded_key);
	}

	batch->operation_count++;
	return 0;
}

/* Submit operations */
int kv_submit_get(struct reffs_kv_store *rks, struct reffs_kv_op *op)
{
	if (!(rks->rks_flags & RKS_ASYNC_MODE)) {
		return ENOTSUP; /* Async mode not enabled */
	}

	/* Submit the operation to the thread pool */
	int ret = task_queue_push(&rks->rks_thread_pool->queue, op);
	if (ret == 0) {
		rks->rks_ops_submitted++;
	}

	return ret;
}

int kv_submit_put(struct reffs_kv_store *rks, struct reffs_kv_op *op)
{
	if (!(rks->rks_flags & RKS_ASYNC_MODE)) {
		return ENOTSUP; /* Async mode not enabled */
	}

	/* Submit the operation to the thread pool */
	int ret = task_queue_push(&rks->rks_thread_pool->queue, op);
	if (ret == 0) {
		rks->rks_ops_submitted++;
	}

	return ret;
}

int kv_submit_delete(struct reffs_kv_store *rks, struct reffs_kv_op *op)
{
	if (!(rks->rks_flags & RKS_ASYNC_MODE)) {
		return ENOTSUP; /* Async mode not enabled */
	}

	/* Submit the operation to the thread pool */
	int ret = task_queue_push(&rks->rks_thread_pool->queue, op);
	if (ret == 0) {
		rks->rks_ops_submitted++;
	}

	return ret;
}

int kv_submit_batch(struct reffs_kv_store *rks, struct reffs_kv_batch *batch,
		    reffs_kv_completion_cb callback, void *user_data)
{
	if (!(rks->rks_flags & RKS_ASYNC_MODE)) {
		return ENOTSUP; /* Async mode not enabled */
	}

	struct reffs_kv_op *op = kv_op_create(rks, REFFS_KV_OP_BATCH, NULL,
					      0, /* No specific key */
					      NULL, 0, /* No specific value */
					      callback, user_data);

	if (!op) {
		return ENOMEM;
	}

	op->rkop_batch = batch;

	/* Submit to thread pool */
	int ret = task_queue_push(&rks->rks_thread_pool->queue, op);
	if (ret == 0) {
		rks->rks_ops_submitted++;
	} else {
		free(op);
	}

	return ret;
}

/* Store initialization and cleanup */
int kv_init_io_uring(struct reffs_kv_store *rks, unsigned int queue_depth,
		     unsigned int thread_count)
{
	rks->rks_ring = malloc(sizeof(struct io_uring));
	if (!rks->rks_ring) {
		return ENOMEM;
	}

	int ret = io_uring_queue_init(queue_depth, rks->rks_ring, 0);
	if (ret < 0) {
		free(rks->rks_ring);
		rks->rks_ring = NULL;
		return -ret;
	}

	/* Initialize the thread pool */
	rks->rks_thread_pool = malloc(sizeof(struct reffs_thread_pool));
	if (!rks->rks_thread_pool) {
		io_uring_queue_exit(rks->rks_ring);
		free(rks->rks_ring);
		rks->rks_ring = NULL;
		return ENOMEM;
	}

	ret = thread_pool_init(rks->rks_thread_pool, rks, thread_count,
			       queue_depth * 2);
	if (ret != 0) {
		free(rks->rks_thread_pool);
		io_uring_queue_exit(rks->rks_ring);
		free(rks->rks_ring);
		rks->rks_ring = NULL;
		return ret;
	}

	rks->rks_flags |= RKS_ASYNC_MODE;
	return 0;
}

void kv_cleanup_io_uring(struct reffs_kv_store *rks)
{
	if (rks->rks_thread_pool) {
		thread_pool_shutdown(rks->rks_thread_pool);
		free(rks->rks_thread_pool);
		rks->rks_thread_pool = NULL;
	}

	if (rks->rks_ring) {
		io_uring_queue_exit(rks->rks_ring);
		free(rks->rks_ring);
		rks->rks_ring = NULL;
	}

	rks->rks_flags &= ~RKS_ASYNC_MODE;
}

/* Main KV store functions */
int kv_open(const char *kv_path, struct reffs_kv_store *rks)
{
	char *err = NULL;
	rocksdb_options_t *opt = NULL;
	char *name = NULL;
	int ret = 0;

	if (!kv_path || !rks) {
		return EINVAL;
	}

	name = malloc(strlen(kv_path) + strlen(rks->rks_name) +
		      2); /* +2 for '/' and null terminator */
	if (!name) {
		return ENOMEM;
	}
	sprintf(name, "%s/%s", kv_path, rks->rks_name);

	rks->rks_ropt = rocksdb_readoptions_create();
	rks->rks_wopt = rocksdb_writeoptions_create();
	opt = rocksdb_options_create();

	if (!rks->rks_ropt || !rks->rks_wopt || !opt) {
		ret = ENOMEM;
		goto out;
	}

	/* Set database options */
	rocksdb_options_set_create_if_missing(opt, 1);

	/* Open the database */
	if (rks->rks_flags & RKS_READ_ONLY) {
		rks->rks_kv = rocksdb_open_for_read_only(opt, name, 0, &err);
	} else {
		rks->rks_kv = rocksdb_open(opt, name, &err);
	}

	if (err != NULL) {
		fprintf(stderr, "Failed to open RocksDB: %s\n", err);
		ret = EIO;
		free(err);
		goto out;
	}

	/* Initialize counters */
	rks->rks_ops_submitted = 0;
	rks->rks_ops_completed = 0;
	rks->rks_op_errors = 0;

out:
	free(name);
	if (opt) {
		rocksdb_options_destroy(opt);
	}

	if (ret != 0) {
		if (rks->rks_ropt) {
			rocksdb_readoptions_destroy(rks->rks_ropt);
			rks->rks_ropt = NULL;
		}
		if (rks->rks_wopt) {
			rocksdb_writeoptions_destroy(rks->rks_wopt);
			rks->rks_wopt = NULL;
		}
	}

	return ret;
}
