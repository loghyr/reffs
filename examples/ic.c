/*
 * SPDX-FileCopyrightText: 2025 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

// A simple buffer pool structure
struct buffer_pool {
	struct io_context **contexts; // Array of context pointers
	size_t capacity;
	size_t used;
	pthread_mutex_t lock; // For thread safety
};

// Initialize a buffer pool
struct buffer_pool *create_buffer_pool(size_t capacity)
{
	struct buffer_pool *pool = malloc(sizeof(struct buffer_pool));
	if (!pool)
		return NULL;

	pool->contexts = malloc(capacity * sizeof(struct io_context *));
	if (!pool->contexts) {
		free(pool);
		return NULL;
	}

	pool->capacity = capacity;
	pool->used = 0;
	pthread_mutex_init(&pool->lock, NULL);

	return pool;
}

// Get an io_context from the pool or create a new one if empty
struct io_context *get_io_context(struct buffer_pool *pool, int op_type, int fd)
{
	struct io_context *context = NULL;

	pthread_mutex_lock(&pool->lock);

	if (pool->used > 0) {
		// Reuse existing context
		context = pool->contexts[--pool->used];

		// Reset the context fields
		context->ic_op_type = op_type;
		context->ic_fd = fd;
		// Buffer already allocated, reuse it
	} else {
		// Create new context with buffer
		char *buffer = malloc(BUFFER_SIZE);
		if (!buffer) {
			pthread_mutex_unlock(&pool->lock);
			return NULL;
		}

		context = create_io_context(op_type, fd, buffer);
		if (!context) {
			free(buffer);
			pthread_mutex_unlock(&pool->lock);
			return NULL;
		}
	}

	pthread_mutex_unlock(&pool->lock);
	return context;
}

// Return an io_context to the pool instead of freeing it
void return_io_context(struct buffer_pool *pool, struct io_context *context)
{
	pthread_mutex_lock(&pool->lock);

	if (pool->used < pool->capacity) {
		pool->contexts[pool->used++] = context;
		// Keep the buffer attached to the context
	} else {
		// Pool is full, just free it
		free(context->ic_buffer);
		free(context);
	}

	pthread_mutex_unlock(&pool->lock);
}
