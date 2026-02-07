// C wrapper header for gRPC server
#ifndef GRPC_WRAPPER_H
#define GRPC_WRAPPER_H

#include <stdint.h>
#include <stdlib.h>

#ifdef __cplusplus
extern "C" {
#endif

// Configuration structure (matches AppConfig in C code)
typedef struct {
    char* host;
    int port;
    char* camera_name;
    char* encoder_type;
    int width;
    int height;
    int framerate;
} grpc_config_t;

// Configuration update structure (for optional fields)
typedef struct {
    const char* host;
    int port;
    const char* camera_name;
    const char* encoder_type;
    int width;
    int height;
    int framerate;
    // Flags to indicate which fields are set
    int has_host;
    int has_port;
    int has_camera_name;
    int has_encoder_type;
    int has_width;
    int has_height;
    int has_framerate;
} grpc_config_update_t;

// Camera info structure
typedef struct {
    char* name;
    char* path;
} grpc_camera_info_t;

// Encoder info structure
typedef struct {
    char* name;
    int available;
} grpc_encoder_info_t;

// Available devices structure
typedef struct {
    grpc_camera_info_t* cameras;
    int num_cameras;
    grpc_encoder_info_t* encoders;
    int num_encoders;
} grpc_devices_t;

// Callback structure - these are called by gRPC server when requests come in
typedef struct {
    // Health check callback
    // Output: status_out should be allocated with malloc() (or NULL for default "healthy")
    void (*health_callback)(void* user_data, char** status_out);

    // Get stats callback
    // Output: Fill in the three stats pointers
    void (*get_stats_callback)(void* user_data, uint64_t* total_bytes, uint64_t* frame_count, double* bitrate);

    // Get config callback
    // Output: Fill in config structure (strings should be allocated with malloc/strdup)
    void (*get_config_callback)(void* user_data, grpc_config_t* config);

    // Update config callback
    // Input: update structure with optional fields
    // Output: new_config should be filled in (strings allocated), error_msg if failed (allocated)
    // Return: 1 for success, 0 for failure
    int (*update_config_callback)(void* user_data, const grpc_config_update_t* update,
                                  grpc_config_t* new_config, char** error_msg);

    // Swap resolution callback
    // Input: swap (0 = force landscape width > height, 1 = force portrait width < height)
    // Output: new_config should be filled in (strings allocated), error_msg if failed (allocated)
    // Return: 1 for success, 0 for failure
    int (*swap_resolution_callback)(void* user_data, int swap, grpc_config_t* new_config, char** error_msg);

    // Update host callback
    // Input: new host string
    // Output: error_msg if failed (allocated)
    // Return: 1 for success, 0 for failure
    int (*update_host_callback)(void* user_data, const char* host, char** error_msg);

    // Get available devices callback
    // Output: devices structure (all strings and arrays should be allocated)
    void (*get_devices_callback)(void* user_data, grpc_devices_t* devices);

    // User data pointer passed to all callbacks
    void* user_data;
} grpc_callbacks;

// Opaque server handle
typedef struct f1sh_grpc_server f1sh_grpc_server_t;

// Start gRPC server
// address: e.g., "0.0.0.0:50051"
// callbacks: callback structure (will be copied, so can be stack-allocated)
// Returns: server handle, or NULL on failure
f1sh_grpc_server_t* f1sh_grpc_server_start(const char* address, const grpc_callbacks* callbacks);

// Stop gRPC server
void f1sh_grpc_server_stop(f1sh_grpc_server_t* server);

// Wait for server to finish (blocking)
void f1sh_grpc_server_wait(f1sh_grpc_server_t* server);

#ifdef __cplusplus
}
#endif

#endif // GRPC_WRAPPER_H
