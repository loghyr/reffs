/*
 * SPDX-FileCopyrightText: 2025 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: GPL-2.0+
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

// ---------- Protocol Layer Types ----------

typedef enum {
	PROTOCOL_JSON,
	PROTOCOL_PROTOBUF,
	PROTOCOL_MSGPACK,
	// Add more protocols as needed
} ProtocolType;

typedef struct {
	void *(*encode)(const void *data, size_t *out_size);
	void *(*decode)(const void *data, size_t size);
	void (*free_buffer)(void *buffer);
} ProtocolHandlers;

// ---------- RPC Layer Types ----------

typedef enum {
	AUTH_NONE,
	AUTH_BASIC,
	AUTH_TOKEN,
	AUTH_OAUTH,
	// Add more auth types as needed
} AuthType;

typedef struct {
	void *(*prepare_request)(const void *payload, size_t payload_size,
				 AuthType auth_type, const void *auth_data,
				 size_t *out_size);
	void *(*extract_response)(const void *rpc_data, size_t rpc_size,
				  size_t *out_size);
	void (*free_buffer)(void *buffer);
} RPCHandlers;

// ---------- Connection Context ----------

typedef struct {
	ProtocolType protocol_type;
	ProtocolHandlers protocol_handlers;
	RPCHandlers rpc_handlers;
	AuthType auth_type;
	void *auth_data;
	size_t auth_data_size;
	// Add more fields like connection info, etc.
} RPCConnection;

// ---------- Global Registry ----------

#define MAX_PROTOCOL_TYPES 10
static ProtocolHandlers g_protocol_registry[MAX_PROTOCOL_TYPES];
static RPCHandlers g_default_rpc_handlers;
static int g_registry_initialized = 0;

// ---------- Implementation ----------

void initialize_registry()
{
	if (!g_registry_initialized) {
		memset(g_protocol_registry, 0, sizeof(g_protocol_registry));
		memset(&g_default_rpc_handlers, 0,
		       sizeof(g_default_rpc_handlers));
		g_registry_initialized = 1;
	}
}

void register_protocol_handlers(ProtocolType type, ProtocolHandlers handlers)
{
	initialize_registry();

	if (type < MAX_PROTOCOL_TYPES) {
		g_protocol_registry[type] = handlers;
	}
}

void register_rpc_handlers(RPCHandlers handlers)
{
	initialize_registry();
	g_default_rpc_handlers = handlers;
}

RPCConnection *rpc_connection_create(ProtocolType protocol_type)
{
	initialize_registry();

	RPCConnection *conn = (RPCConnection *)malloc(sizeof(RPCConnection));
	if (conn) {
		conn->protocol_type = protocol_type;
		conn->protocol_handlers = g_protocol_registry[protocol_type];
		conn->rpc_handlers = g_default_rpc_handlers;
		conn->auth_type = AUTH_NONE;
		conn->auth_data = NULL;
		conn->auth_data_size = 0;
	}
	return conn;
}

void rpc_connection_destroy(RPCConnection *connection)
{
	if (connection) {
		if (connection->auth_data) {
			free(connection->auth_data);
		}
		free(connection);
	}
}

void rpc_connection_set_auth(RPCConnection *connection, AuthType auth_type,
			     const void *auth_data, size_t auth_data_size)
{
	if (connection) {
		connection->auth_type = auth_type;

		// Free existing auth data if present
		if (connection->auth_data) {
			free(connection->auth_data);
			connection->auth_data = NULL;
			connection->auth_data_size = 0;
		}

		// Copy new auth data
		if (auth_data && auth_data_size > 0) {
			connection->auth_data = malloc(auth_data_size);
			if (connection->auth_data) {
				memcpy(connection->auth_data, auth_data,
				       auth_data_size);
				connection->auth_data_size = auth_data_size;
			}
		}
	}
}

void *rpc_connection_call(RPCConnection *connection, const void *request_data,
			  size_t *response_size)
{
	if (!connection || !request_data || !response_size) {
		return NULL;
	}

	// Step 1: Encode the request data using the protocol layer
	size_t payload_size = 0;
	void *encoded_payload = connection->protocol_handlers.encode(
		request_data, &payload_size);
	if (!encoded_payload) {
		return NULL;
	}

	// Step 2: Prepare the RPC request with the encoded payload
	size_t rpc_request_size = 0;
	void *rpc_request = connection->rpc_handlers.prepare_request(
		encoded_payload, payload_size, connection->auth_type,
		connection->auth_data, &rpc_request_size);

	// Free the encoded payload as it's no longer needed
	connection->protocol_handlers.free_buffer(encoded_payload);

	if (!rpc_request) {
		return NULL;
	}

	// Step 3: Send the RPC request and receive the response
	// (This would typically involve network operations, but we'll simulate it)
	void *rpc_response = simulate_rpc_network_call(
		rpc_request, rpc_request_size, response_size);

	// Free the RPC request as it's no longer needed
	connection->rpc_handlers.free_buffer(rpc_request);

	if (!rpc_response) {
		*response_size = 0;
		return NULL;
	}

	// Step 4: Extract the payload from the RPC response
	size_t payload_response_size = 0;
	void *payload_response = connection->rpc_handlers.extract_response(
		rpc_response, *response_size, &payload_response_size);

	// Free the RPC response as it's no longer needed
	connection->rpc_handlers.free_buffer(rpc_response);

	if (!payload_response) {
		*response_size = 0;
		return NULL;
	}

	// Step 5: Decode the payload using the protocol layer
	void *decoded_response = connection->protocol_handlers.decode(
		payload_response, payload_response_size);

	// Free the payload response as it's no longer needed
	connection->protocol_handlers.free_buffer(payload_response);

	// Update the response size - this would typically be the size of the decoded structure
	// but for simplicity we'll just set it to non-zero if successful
	if (decoded_response) {
		*response_size = 1;
	} else {
		*response_size = 0;
	}

	return decoded_response;
}

// Simulate an RPC network call (in a real implementation, this would make a network request)
void *simulate_rpc_network_call(const void *request, size_t request_size,
				size_t *response_size)
{
	// In a real implementation, this would send the request over the network
	// and receive a response. For simplicity, we'll just create a dummy response.

	// Dummy response - just allocate a buffer and copy the request
	void *response = malloc(request_size);
	if (response) {
		memcpy(response, request, request_size);
		*response_size = request_size;
	} else {
		*response_size = 0;
	}

	return response;
}

// ---------- Protocol Implementation Examples ----------

// Example JSON protocol implementation
void *json_encode(const void *data, size_t *out_size)
{
	// In a real implementation, this would serialize the data to JSON
	// For simplicity, we'll just create a dummy JSON string
	const char *json =
		"{\"method\":\"example\",\"params\":{\"key\":\"value\"}}";
	*out_size = strlen(json) + 1;

	char *buffer = (char *)malloc(*out_size);
	if (buffer) {
		strcpy(buffer, json);
	}

	return buffer;
}

void *json_decode(const void *data, size_t size)
{
	// In a real implementation, this would parse the JSON data
	// For simplicity, we'll just create a dummy structure

	// Deserialize your JSON data here and return the structure
	// For this example, we'll just return a copy of the data
	void *result = malloc(size);
	if (result) {
		memcpy(result, data, size);
	}

	return result;
}

void json_free_buffer(void *buffer)
{
	free(buffer);
}

// Example Protocol Buffers implementation
// (In a real implementation, this would use the protobuf-c library)
void *protobuf_encode(const void *data, size_t *out_size)
{
	// Encode using Protocol Buffers
	// For simplicity, we'll just create a dummy binary buffer
	*out_size = 64; // Arbitrary size

	void *buffer = malloc(*out_size);
	if (buffer) {
		// Fill the buffer with some dummy data
		memset(buffer, 0xAB, *out_size);
	}

	return buffer;
}

void *protobuf_decode(const void *data, size_t size)
{
	// Decode using Protocol Buffers
	// For simplicity, we'll just create a dummy structure

	// Deserialize your protobuf data here and return the structure
	// For this example, we'll just return a copy of the data
	void *result = malloc(size);
	if (result) {
		memcpy(result, data, size);
	}

	return result;
}

void protobuf_free_buffer(void *buffer)
{
	free(buffer);
}

// ---------- RPC Implementation Examples ----------

// Example RPC request preparation
void *default_prepare_request(const void *payload, size_t payload_size,
			      AuthType auth_type, const void *auth_data,
			      size_t *out_size)
{
	// In a real implementation, this would create an RPC request envelope
	// with the payload and authentication data

	// For simplicity, we'll just prepend a small header to the payload

	// Header format: 4 bytes for auth type, 4 bytes for payload size
	const size_t HEADER_SIZE = 8;
	*out_size = HEADER_SIZE + payload_size;

	void *buffer = malloc(*out_size);
	if (buffer) {
		// Set the authentication type in the header
		*((uint32_t *)buffer) = (uint32_t)auth_type;

		// Set the payload size in the header
		*((uint32_t *)((char *)buffer + 4)) = (uint32_t)payload_size;

		// Copy the payload
		memcpy((char *)buffer + HEADER_SIZE, payload, payload_size);
	}

	return buffer;
}

void *default_extract_response(const void *rpc_data, size_t rpc_size,
			       size_t *out_size)
{
	// In a real implementation, this would extract the payload from the RPC response

	// For simplicity, we'll assume the same format as in prepare_request

	// Header format: 4 bytes for auth type, 4 bytes for payload size
	const size_t HEADER_SIZE = 8;

	if (rpc_size < HEADER_SIZE) {
		*out_size = 0;
		return NULL;
	}

	// Extract the payload size from the header
	uint32_t payload_size = *((uint32_t *)((char *)rpc_data + 4));

	// Check if the payload size is valid
	if (HEADER_SIZE + payload_size > rpc_size) {
		*out_size = 0;
		return NULL;
	}

	// Allocate a buffer for the payload
	void *buffer = malloc(payload_size);
	if (buffer) {
		// Copy the payload
		memcpy(buffer, (char *)rpc_data + HEADER_SIZE, payload_size);
		*out_size = payload_size;
	} else {
		*out_size = 0;
	}

	return buffer;
}

void default_free_buffer(void *buffer)
{
	free(buffer);
}

// ---------- Example Usage ----------

void register_default_handlers()
{
	// Register the JSON protocol handlers
	ProtocolHandlers json_handlers = { .encode = json_encode,
					   .decode = json_decode,
					   .free_buffer = json_free_buffer };
	register_protocol_handlers(PROTOCOL_JSON, json_handlers);

	// Register the Protocol Buffers handlers
	ProtocolHandlers protobuf_handlers = { .encode = protobuf_encode,
					       .decode = protobuf_decode,
					       .free_buffer =
						       protobuf_free_buffer };
	register_protocol_handlers(PROTOCOL_PROTOBUF, protobuf_handlers);

	// Register the default RPC handlers
	RPCHandlers default_rpc = { .prepare_request = default_prepare_request,
				    .extract_response =
					    default_extract_response,
				    .free_buffer = default_free_buffer };
	register_rpc_handlers(default_rpc);
}

// Example struct for user data
typedef struct {
	int user_id;
	char username[32];
} UserData;

int main()
{
	// Register the default handlers
	register_default_handlers();

	// Create a connection using the JSON protocol
	RPCConnection *conn = rpc_connection_create(PROTOCOL_JSON);

	// Set up token authentication
	const char *token = "my_auth_token";
	rpc_connection_set_auth(conn, AUTH_TOKEN, token, strlen(token) + 1);

	// Create some example user data
	UserData user = { .user_id = 123, .username = "example_user" };

	// Make an RPC call
	size_t response_size = 0;
	void *response = rpc_connection_call(conn, &user, &response_size);

	if (response) {
		printf("RPC call successful!\n");

		// In a real application, you would cast the response to the expected type
		// and process it accordingly.

		// Free the response
		conn->protocol_handlers.free_buffer(response);
	} else {
		printf("RPC call failed!\n");
	}

	// Destroy the connection
	rpc_connection_destroy(conn);

	return 0;
}
