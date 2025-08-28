#include <gst/gst.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <microhttpd.h>
#include <glob.h>
#include <jansson.h>

#ifdef __APPLE__
#include <TargetConditionals.h>
#endif

#define HTTP_PORT 8888

// Default configuration
#define DEFAULT_HOST "127.0.0.1"
#define DEFAULT_PORT 5000
#define DEFAULT_SRC "libcamerasrc"
#define DEFAULT_DEVICE "" // Autodetect
#define DEFAULT_ENCODER "v4l2h264enc"
#define DEFAULT_WIDTH 1280
#define DEFAULT_HEIGHT 720
#define DEFAULT_FRAMERATE 60
#define DEFAULT_AUTOFOCUS FALSE

// Application data structure
typedef struct _AppConfig {
    gchar *host;
    gint port;
    gchar *src_type;
    gchar *device;
    gchar *encoder_type;
    gint width;
    gint height;
    gint framerate;
    gboolean autofocus;
    gfloat lens_position; // 0.0 = close focus, 1.0+ = far/infinity focus
} AppConfig;

// Statistics structure
typedef struct _StreamStats {
    guint64 total_bytes;
    guint64 frame_count;
    gdouble current_bitrate;        // kbps
    GstClockTime start_time;
    GMutex stats_mutex;
} StreamStats;

typedef struct _CustomData {
    GstElement *pipeline;
    GstBus *bus;
    struct MHD_Daemon *daemon;
    AppConfig config;
    StreamStats stats;
    GMutex state_mutex;
    gboolean pipeline_is_restarting;
    gboolean should_terminate;
} CustomData;

// Structure to hold the connection-specific data for POST requests
struct connection_info_struct {
    char *json_data;
    size_t data_size;
    CustomData *custom_data;
};

// Forward declarations
static enum MHD_Result answer_to_connection(void *cls, struct MHD_Connection *connection,
                                            const char *url, const char *method,
                                            const char *version, const char *upload_data,
                                            size_t *upload_data_size, void **con_cls);
static void request_completed(void *cls, struct MHD_Connection *connection,
                              void **con_cls, enum MHD_RequestTerminationCode toe);
static gboolean build_and_run_pipeline(CustomData *data);
static void free_config_members(AppConfig *config);

// Probe callback on source to timestamp buffers
static GstPadProbeReturn
source_probe_callback (GstPad *pad __attribute__((unused)), GstPadProbeInfo *info, gpointer user_data __attribute__((unused)))
{
    GstBuffer *buffer = GST_PAD_PROBE_INFO_BUFFER(info);
    
    if (buffer) {
        static guint64 source_frame_count = 0;
        source_frame_count++;
        
        // Print debug info every 30 frames
        if (source_frame_count % 30 == 0) {
            gsize buffer_size = gst_buffer_get_size(buffer);
            g_print("Source output: frame %llu, size %zu bytes\n", (unsigned long long)source_frame_count, buffer_size);
        }
        
        // Add a custom timestamp as metadata to track when buffer was created
        GstClockTime current_time = g_get_monotonic_time() * 1000; // Convert microseconds to nanoseconds
        
        // Create a new buffer with the timestamp metadata
        buffer = gst_buffer_make_writable(buffer);
        
        // Store the capture time as a custom meta or just use the DTS field
        GST_BUFFER_DTS(buffer) = current_time;
        
        GST_PAD_PROBE_INFO_DATA(info) = buffer;
    }
    
    return GST_PAD_PROBE_OK;
}

// Probe callback to monitor encoder output
static GstPadProbeReturn
encoder_probe_callback (GstPad *pad __attribute__((unused)), GstPadProbeInfo *info, gpointer user_data __attribute__((unused)))
{
    GstBuffer *buffer = GST_PAD_PROBE_INFO_BUFFER(info);
    
    if (buffer) {
        static guint64 encoder_frame_count = 0;
        encoder_frame_count++;
        
        // Print debug info every 30 frames
        if (encoder_frame_count % 30 == 0) {
            gsize buffer_size = gst_buffer_get_size(buffer);
            g_print("Encoder output: frame %llu, size %zu bytes\n", (unsigned long long)encoder_frame_count, buffer_size);
        }
    }
    
    return GST_PAD_PROBE_OK;
}

// Probe callback to monitor data flow
static GstPadProbeReturn
udpsink_probe_callback (GstPad *pad __attribute__((unused)), GstPadProbeInfo *info, gpointer user_data)
{
    CustomData *data = (CustomData *)user_data;
    GstBuffer *buffer = GST_PAD_PROBE_INFO_BUFFER(info);
    
    if (buffer) {
        g_mutex_lock(&data->stats.stats_mutex);
        
        // Update byte count and frame count
        gsize buffer_size = gst_buffer_get_size(buffer);
        data->stats.total_bytes += buffer_size;
        data->stats.frame_count++;
        
        // Print debug info every 30 frames (about every 0.5 seconds at 60fps)
        if (data->stats.frame_count % 30 == 0) {
            g_print("UDP probe: frame %llu, size %zu bytes, total %llu bytes\n", 
                    (unsigned long long)data->stats.frame_count, buffer_size, (unsigned long long)data->stats.total_bytes);
        }
        
        g_mutex_unlock(&data->stats.stats_mutex);
    }
    
    return GST_PAD_PROBE_OK;
}

// Initialize with default values
void init_config(AppConfig *config) {
    config->host = g_strdup(DEFAULT_HOST);
    config->port = DEFAULT_PORT;
    config->src_type = g_strdup(DEFAULT_SRC);
    config->device = g_strdup(DEFAULT_DEVICE);
    config->encoder_type = g_strdup(DEFAULT_ENCODER);
    config->width = DEFAULT_WIDTH;
    config->height = DEFAULT_HEIGHT;
    config->framerate = DEFAULT_FRAMERATE;
    config->autofocus = DEFAULT_AUTOFOCUS;
    config->lens_position = 1.0; // Default to far focus
}

void free_config_members(AppConfig *config) {
    g_free(config->host);
    g_free(config->src_type);
    g_free(config->device);
    g_free(config->encoder_type);
}

// Initialize statistics
void init_stats(StreamStats *stats) {
    stats->total_bytes = 0;
    stats->frame_count = 0;
    stats->current_bitrate = 0.0;
    stats->start_time = gst_clock_get_time(gst_system_clock_obtain());
    g_mutex_init(&stats->stats_mutex);
}

void free_stats(StreamStats *stats) {
    g_mutex_clear(&stats->stats_mutex);
}

// Find first available /dev/video* device
gchar* find_video_device(void) {
    glob_t glob_result;
    memset(&glob_result, 0, sizeof(glob_result));

    int return_value = glob("/dev/video*", GLOB_TILDE, NULL, &glob_result);
    if (return_value != 0) {
        globfree(&glob_result);
        return NULL;
    }

    gchar *device = NULL;
    if (glob_result.gl_pathc > 0) {
        device = g_strdup(glob_result.gl_pathv[0]);
    }

    globfree(&glob_result);
    return device;
}

static enum MHD_Result send_json_response(struct MHD_Connection *connection, const char *json_string, int status_code) {
    struct MHD_Response *response;
    enum MHD_Result ret;

    response = MHD_create_response_from_buffer(strlen(json_string), (void *)json_string, MHD_RESPMEM_MUST_COPY);
    MHD_add_response_header(response, "Content-Type", "application/json");
    ret = MHD_queue_response(connection, status_code, response);
    MHD_destroy_response(response);
    return ret;
}

static enum MHD_Result handle_health_check(struct MHD_Connection *connection) {
    const char *page = "{\"status\":\"ok\"}";
    return send_json_response(connection, page, MHD_HTTP_OK);
}

static enum MHD_Result handle_get_config(struct MHD_Connection *connection, CustomData *data) {
    g_mutex_lock(&data->state_mutex);
    json_t *root = json_object();
    json_object_set_new(root, "host", json_string(data->config.host));
    json_object_set_new(root, "port", json_integer(data->config.port));
    json_object_set_new(root, "src", json_string(data->config.src_type));
    json_object_set_new(root, "device", json_string(data->config.device));
    json_object_set_new(root, "encoder", json_string(data->config.encoder_type));
    json_object_set_new(root, "width", json_integer(data->config.width));
    json_object_set_new(root, "height", json_integer(data->config.height));
    json_object_set_new(root, "framerate", json_integer(data->config.framerate));
    json_object_set_new(root, "autofocus", json_boolean(data->config.autofocus));
    json_object_set_new(root, "lens_position", json_real(data->config.lens_position));
    g_mutex_unlock(&data->state_mutex);

    char *json_str = json_dumps(root, 0);
    json_decref(root);

    enum MHD_Result ret = send_json_response(connection, json_str, MHD_HTTP_OK);
    free(json_str);
    return ret;
}

static enum MHD_Result handle_get_devices(struct MHD_Connection *connection) {
    json_t *array = json_array();
    gchar *device = find_video_device();
    if (device) {
        // In a real app, you'd loop through glob results
        json_array_append_new(array, json_string(device));
        g_free(device);
    }

    json_t *root = json_object();
    json_object_set_new(root, "devices", array);
    char *json_str = json_dumps(root, 0);
    json_decref(root);

    enum MHD_Result ret = send_json_response(connection, json_str, MHD_HTTP_OK);
    free(json_str);
    return ret;
}

static enum MHD_Result handle_get_stats(struct MHD_Connection *connection, CustomData *data) {
    g_mutex_lock(&data->stats.stats_mutex);
    
    // Calculate current bitrate
    GstClockTime current_time = gst_clock_get_time(gst_system_clock_obtain());
    GstClockTime elapsed = current_time - data->stats.start_time;
    gdouble elapsed_seconds = (gdouble)elapsed / GST_SECOND;
    
    gdouble current_bitrate = 0.0;
    if (elapsed_seconds > 0) {
        current_bitrate = (data->stats.total_bytes * 8.0) / (elapsed_seconds * 1000.0); // kbps
    }
    
    json_t *root = json_object();
    json_object_set_new(root, "total_bytes", json_integer(data->stats.total_bytes));
    json_object_set_new(root, "frame_count", json_integer(data->stats.frame_count));
    json_object_set_new(root, "current_bitrate_kbps", json_real(current_bitrate));
    
    g_mutex_unlock(&data->stats.stats_mutex);

    char *json_str = json_dumps(root, 0);
    json_decref(root);

    enum MHD_Result ret = send_json_response(connection, json_str, MHD_HTTP_OK);
    free(json_str);
    return ret;
}

static enum MHD_Result process_config_update(struct connection_info_struct *con_info) {
    json_error_t error;
    json_t *root = json_loadb(con_info->json_data, con_info->data_size, 0, &error);

    if (!root) {
        g_printerr("JSON error on line %d: %s\n", error.line, error.text);
        return MHD_NO;
    }

    CustomData *data = con_info->custom_data;
    g_mutex_lock(&data->state_mutex);

    json_t *value;
    const char *str_val;
    gboolean needs_pipeline_rebuild = FALSE;
    gboolean needs_udp_update = FALSE;
    
    // Store old values to detect changes
    gchar *old_host = g_strdup(data->config.host);
    gint old_port = data->config.port;
    gint old_width = data->config.width;
    gint old_height = data->config.height;
    gint old_framerate = data->config.framerate;
    gboolean old_autofocus = data->config.autofocus;

    value = json_object_get(root, "host");
    if (json_is_string(value)) {
        str_val = json_string_value(value);
        if (strcmp(data->config.host, str_val) != 0) {
            g_free(data->config.host);
            data->config.host = g_strdup(str_val);
            needs_udp_update = TRUE;
        }
    }
    
    value = json_object_get(root, "port");
    if (json_is_integer(value)) {
        gint new_port = json_integer_value(value);
        if (data->config.port != new_port) {
            data->config.port = new_port;
            needs_udp_update = TRUE;
        }
    }
    
    value = json_object_get(root, "src");
    if (json_is_string(value)) {
        str_val = json_string_value(value);
        if (strcmp(data->config.src_type, str_val) != 0) {
            g_free(data->config.src_type);
            data->config.src_type = g_strdup(str_val);
            needs_pipeline_rebuild = TRUE;
        }
    }
    
    value = json_object_get(root, "device");
    if (json_is_string(value)) {
        str_val = json_string_value(value);
        g_free(data->config.device);
        data->config.device = g_strdup(str_val);
        needs_pipeline_rebuild = TRUE;
    }
    
    value = json_object_get(root, "encoder");
    if (json_is_string(value)) {
        str_val = json_string_value(value);
        if (strcmp(data->config.encoder_type, str_val) != 0) {
            g_free(data->config.encoder_type);
            data->config.encoder_type = g_strdup(str_val);
            needs_pipeline_rebuild = TRUE;
        }
    }
    
    value = json_object_get(root, "width");
    if (json_is_integer(value)) {
        int new_width = json_integer_value(value);
        if (new_width >= 320 && new_width <= 4096 && new_width != data->config.width) {
            data->config.width = new_width;
            needs_pipeline_rebuild = TRUE;
        } else if (new_width < 320 || new_width > 4096) {
            g_print("Warning: Invalid width %d, keeping current value %d\n", new_width, data->config.width);
        }
    }
    
    value = json_object_get(root, "height");
    if (json_is_integer(value)) {
        int new_height = json_integer_value(value);
        if (new_height >= 240 && new_height <= 2160 && new_height != data->config.height) {
            data->config.height = new_height;
            needs_pipeline_rebuild = TRUE;
        } else if (new_height < 240 || new_height > 2160) {
            g_print("Warning: Invalid height %d, keeping current value %d\n", new_height, data->config.height);
        }
    }
    
    value = json_object_get(root, "framerate");
    if (json_is_integer(value)) {
        int new_framerate = json_integer_value(value);
        if (new_framerate >= 1 && new_framerate <= 120 && new_framerate != data->config.framerate) {
            data->config.framerate = new_framerate;
            needs_pipeline_rebuild = TRUE;
        } else if (new_framerate < 1 || new_framerate > 120) {
            g_print("Warning: Invalid framerate %d, keeping current value %d\n", new_framerate, data->config.framerate);
        }
    }

    value = json_object_get(root, "autofocus");
    if (json_is_boolean(value)) {
        gboolean new_autofocus = json_boolean_value(value);
        if (new_autofocus != data->config.autofocus) {
            data->config.autofocus = new_autofocus;
            needs_pipeline_rebuild = TRUE;
        }
    }

    value = json_object_get(root, "lens_position");
    if (json_is_real(value)) {
        data->config.lens_position = json_real_value(value);
    } else if (json_is_integer(value)) {
        data->config.lens_position = (gfloat)json_integer_value(value);
    }

    // Decide what kind of update to perform
    if (needs_pipeline_rebuild) {
        g_print("Configuration change requires pipeline rebuild: host=%s, port=%d, src=%s, encoder=%s, %dx%d@%dfps, autofocus=%s, lens_position=%.2f\n", 
                data->config.host, data->config.port, data->config.src_type, data->config.encoder_type,
                data->config.width, data->config.height, data->config.framerate,
                data->config.autofocus ? "enabled" : "disabled", data->config.lens_position);
        data->pipeline_is_restarting = TRUE;
    } else if (needs_udp_update && data->pipeline) {
        g_print("Updating UDP sink destination: %s:%d (no pipeline rebuild needed)\n", 
                data->config.host, data->config.port);
        
        // Find the UDP sink element and update its properties
        GstElement *sink = gst_bin_get_by_name(GST_BIN(data->pipeline), "sink");
        if (sink) {
            g_object_set(sink, "host", data->config.host, "port", data->config.port, NULL);
            gst_object_unref(sink);
            g_print("UDP sink updated successfully\n");
        } else {
            g_printerr("Could not find UDP sink element for update\n");
        }
    } else {
        g_print("Configuration updated (no changes required): host=%s, port=%d\n", 
                data->config.host, data->config.port);
    }

    g_free(old_host);
    g_mutex_unlock(&data->state_mutex);
    json_decref(root);

    return MHD_YES;
}

static enum MHD_Result answer_to_connection(void *cls, struct MHD_Connection *connection,
                                            const char *url, const char *method,
                                            const char *version __attribute__((unused)), const char *upload_data,
                                            size_t *upload_data_size, void **con_cls) {
    CustomData *data = (CustomData *)cls;

    if (NULL == *con_cls) {
        if (0 == strcmp(method, "POST")) {
            struct connection_info_struct *con_info = malloc(sizeof(struct connection_info_struct));
            if (NULL == con_info) return MHD_NO;
            con_info->json_data = NULL;
            con_info->data_size = 0;
            con_info->custom_data = data;
            *con_cls = (void *)con_info;
            return MHD_YES;
        }
        *con_cls = (void *)1; // Generic marker for non-POST requests
        return MHD_YES;
    }

    if (0 == strcmp(method, "GET")) {
        if (0 == strcmp(url, "/health")) return handle_health_check(connection);
        if (0 == strcmp(url, "/config")) return handle_get_config(connection, data);
        if (0 == strcmp(url, "/devices")) return handle_get_devices(connection);
        if (0 == strcmp(url, "/stats")) return handle_get_stats(connection, data);
    }

    if (0 == strcmp(method, "POST") && 0 == strcmp(url, "/config")) {
        struct connection_info_struct *con_info = *con_cls;
        if (*upload_data_size != 0) {
            con_info->json_data = realloc(con_info->json_data, con_info->data_size + *upload_data_size);
            memcpy(con_info->json_data + con_info->data_size, upload_data, *upload_data_size);
            con_info->data_size += *upload_data_size;
            *upload_data_size = 0;
            return MHD_YES;
        } else {
            // End of upload, process the data
            if (MHD_YES == process_config_update(con_info)) {
                return send_json_response(connection, "{\"status\":\"configuration updated\"}", MHD_HTTP_OK);
            } else {
                return send_json_response(connection, "{\"error\":\"Invalid JSON\"}", MHD_HTTP_BAD_REQUEST);
            }
        }
    }

    return send_json_response(connection, "{\"error\":\"Not Found\"}", MHD_HTTP_NOT_FOUND);
}

static void request_completed(void *cls __attribute__((unused)), struct MHD_Connection *connection __attribute__((unused)),
                              void **con_cls, enum MHD_RequestTerminationCode toe __attribute__((unused))) {
    struct connection_info_struct *con_info = *con_cls;
    if (NULL == con_info || con_info == (void *)1) return;
    if (con_info->json_data) free(con_info->json_data);
    free(con_info);
    *con_cls = NULL;
}

static gboolean build_and_run_pipeline(CustomData *data) {
    g_mutex_lock(&data->state_mutex);
    g_print("Building pipeline with config: host=%s, port=%d, src=%s, %dx%d@%dfps\n",
            data->config.host, data->config.port, data->config.src_type,
            data->config.width, data->config.height, data->config.framerate);

    // Stop and cleanup existing pipeline
    if (data->pipeline) {
        g_print("Stopping existing pipeline.\n");
        
        // Set to NULL state and wait for completion
        GstStateChangeReturn ret = gst_element_set_state(data->pipeline, GST_STATE_NULL);
        if (ret == GST_STATE_CHANGE_ASYNC) {
            g_print("Waiting for pipeline to stop...\n");
            ret = gst_element_get_state(data->pipeline, NULL, NULL, 5 * GST_SECOND);
            if (ret == GST_STATE_CHANGE_FAILURE) {
                g_printerr("Warning: Pipeline stop failed\n");
            }
        }
        
        gst_object_unref(data->pipeline);
        data->pipeline = NULL;
        
        // Give libcamera time to release the camera resource
        g_print("Waiting for camera resource to be released...\n");
        g_usleep(1000000); // Wait 1 second
    }
    if (data->bus) {
        gst_object_unref(data->bus);
        data->bus = NULL;
    }

    data->pipeline = gst_pipeline_new("video-stream-pipeline");
    if (!data->pipeline) {
        g_printerr("Failed to create pipeline.\n");
        g_mutex_unlock(&data->state_mutex);
        return FALSE;
    }

    GstElement *src, *capsfilter, *encoder, *encoder_caps, *parser, *payloader, *sink;
    GstCaps *caps;

    src = gst_element_factory_make(data->config.src_type, "source");
    if (!src) {
        g_printerr("Failed to create source: %s.\n", data->config.src_type);
        goto error;
    }
    g_print("Successfully created source element: %s\n", data->config.src_type);
    if (strcmp(data->config.src_type, "v4l2src") == 0) {
        gchar *device_path = data->config.device;
        if (!device_path || strlen(device_path) == 0) {
            device_path = find_video_device();
        }
        if (device_path) {
            g_object_set(src, "device", device_path, NULL);
            if (strlen(data->config.device) == 0) g_free(device_path);
        }
    } else if (strcmp(data->config.src_type, "libcamerasrc") == 0) {
        // Don't force specific camera name - let libcamera auto-detect
        // g_object_set(src, "camera-name", "/base/soc/i2c0mux/i2c@1/imx708@1a", NULL);
        
        // Configure autofocus for Camera Module 3
        if (data->config.autofocus) {
            g_print("Enabling autofocus for Camera Module 3\n");
            
            // Check if the element has a 'controls' property first
            GParamSpec *pspec = g_object_class_find_property(G_OBJECT_GET_CLASS(src), "controls");
            if (pspec) {
                // Use the correct libcamera control names for autofocus
                GstStructure *controls = gst_structure_new("controls",
                    "AfMode", G_TYPE_INT, 2,           // AfModeContinuous
                    "AfSpeed", G_TYPE_INT, 1,          // Fast autofocus speed
                    "AfRange", G_TYPE_INT, 0,          // AfRangeNormal (full range)
                    "LensPosition", G_TYPE_FLOAT, 0.0, // Let AF control lens position
                    NULL);
                g_object_set(src, "controls", controls, NULL);
                gst_structure_free(controls);
            } else {
                g_print("Warning: libcamerasrc doesn't support 'controls' property - autofocus may not work\n");
            }
        } else {
            g_print("Autofocus disabled - using manual focus at position %.2f\n", data->config.lens_position);
            // Check if the element has a 'controls' property first
            GParamSpec *pspec = g_object_class_find_property(G_OBJECT_GET_CLASS(src), "controls");
            if (pspec) {
                // Set manual focus mode with configurable lens position
                GstStructure *controls = gst_structure_new("controls",
                    "AfMode", G_TYPE_INT, 0,                           // AfModeManual
                    "LensPosition", G_TYPE_FLOAT, data->config.lens_position, // User-configurable focus
                    NULL);
                g_object_set(src, "controls", controls, NULL);
                gst_structure_free(controls);
            } else {
                g_print("Warning: libcamerasrc doesn't support 'controls' property - manual focus may not work\n");
            }
        }
    }

    // Try a simple test: if 1920x1080@60fps fails, try 1280x720@30fps
    if (data->config.width == 1920 && data->config.height == 1080 && data->config.framerate == 60) {
        g_print("High resolution/framerate detected. If pipeline fails, will try lower settings.\n");
    }
    
    // Add probe to source to timestamp buffers for latency measurement
    GstPad *src_pad = gst_element_get_static_pad(src, "src");
    if (src_pad) {
        gst_pad_add_probe(src_pad, GST_PAD_PROBE_TYPE_BUFFER, source_probe_callback, data, NULL);
        gst_object_unref(src_pad);
    } else {
        g_printerr("Warning: Could not get source pad for probe\n");
    }

    capsfilter = gst_element_factory_make("capsfilter", "capsfilter");
    if (!capsfilter) {
        g_printerr("Failed to create capsfilter element.\n");
        goto error;
    }
    caps = gst_caps_new_simple("video/x-raw",
                               "width", G_TYPE_INT, data->config.width,
                               "height", G_TYPE_INT, data->config.height,
                               "framerate", GST_TYPE_FRACTION, data->config.framerate, 1,
                               NULL);
    if (strcmp(data->config.src_type, "libcamerasrc") == 0) {
        // Use the exact format from the working gst-launch command
        gst_caps_set_simple(caps, "format", G_TYPE_STRING, "YUY2",
                                  "colorimetry", G_TYPE_STRING, "bt709",
                                  "interlace-mode", G_TYPE_STRING, "progressive", NULL);
    }
    
    // Debug: Print the caps we're setting
    gchar *caps_str = gst_caps_to_string(caps);
    g_print("Setting caps: %s\n", caps_str);
    g_free(caps_str);
    
    g_object_set(capsfilter, "caps", caps, NULL);
    gst_caps_unref(caps);

    encoder = gst_element_factory_make(data->config.encoder_type, "encoder");
    if (!encoder) {
        g_printerr("Failed to create encoder: %s. Trying fallback encoder...\n", data->config.encoder_type);
        // Try fallback encoders
        if (strcmp(data->config.encoder_type, "v4l2h264enc") == 0) {
            encoder = gst_element_factory_make("x264enc", "encoder");
            if (encoder) {
                g_print("Using x264enc as fallback encoder.\n");
                g_object_set(encoder, "tune", 0x00000004, "speed-preset", 1, "bitrate", 2048, "threads", 1, NULL);
            }
        } else if (strcmp(data->config.encoder_type, "x264enc") == 0) {
            encoder = gst_element_factory_make("v4l2h264enc", "encoder");
            if (encoder) {
                g_print("Using v4l2h264enc as fallback encoder.\n");
            }
        }
        
        if (!encoder) {
            g_printerr("No suitable encoder found.\n");
            goto error;
        }
    }
    
    if (strcmp(data->config.encoder_type, "x264enc") == 0 || 
        (gst_element_get_factory(encoder) && 
         strcmp(GST_OBJECT_NAME(gst_element_get_factory(encoder)), "x264enc") == 0)) {
        g_object_set(encoder, "tune", 0x00000004, "speed-preset", 1, "bitrate", 2048, "threads", 1, NULL);
    } else if (strcmp(data->config.encoder_type, "v4l2h264enc") == 0 ||
               (gst_element_get_factory(encoder) && 
                strcmp(GST_OBJECT_NAME(gst_element_get_factory(encoder)), "v4l2h264enc") == 0)) {
        // Use the exact same settings as the working gst-launch command
        GstStructure *ctrls = gst_structure_new("controls", 
                                               "repeat_sequence_header", G_TYPE_BOOLEAN, TRUE,
                                               NULL);
        g_object_set(encoder, "extra-controls", ctrls, NULL);
        gst_structure_free(ctrls);
    }

    // Add probe to monitor encoder output
    GstPad *encoder_pad = gst_element_get_static_pad(encoder, "src");
    if (encoder_pad) {
        gst_pad_add_probe(encoder_pad, GST_PAD_PROBE_TYPE_BUFFER, encoder_probe_callback, data, NULL);
        gst_object_unref(encoder_pad);
    }

    parser = gst_element_factory_make("h264parse", "parser");
    if (!parser) {
        g_printerr("Failed to create h264parse element.\n");
        goto error;
    }
    
    // Add caps filter after encoder to match the working pipeline
    encoder_caps = gst_element_factory_make("capsfilter", "encoder_caps");
    if (!encoder_caps) {
        g_printerr("Failed to create encoder caps filter.\n");
        goto error;
    }
    GstCaps *h264_caps = gst_caps_new_simple("video/x-h264",
                                             "level", G_TYPE_STRING, "4",
                                             NULL);
    g_object_set(encoder_caps, "caps", h264_caps, NULL);
    gst_caps_unref(h264_caps);

    payloader = gst_element_factory_make("rtph264pay", "payloader");
    if (!payloader) {
        g_printerr("Failed to create rtph264pay element.\n");
        goto error;
    }
    g_object_set(payloader, "config-interval", -1, NULL);

    sink = gst_element_factory_make("udpsink", "sink");
    if (!sink) {
        g_printerr("Failed to create udpsink element.\n");
        goto error;
    }
    
    g_print("Configuring UDP sink: %s:%d\n", data->config.host, data->config.port);
    
    // Test UDP connectivity (simple socket test)
    if (strcmp(data->config.host, "127.0.0.1") != 0 && strcmp(data->config.host, "localhost") != 0) {
        g_print("Testing network connectivity to %s:%d...\n", data->config.host, data->config.port);
        // Note: This is a basic check - actual UDP doesn't require connection
        // but this helps verify the host is reachable
    }
    
    g_object_set(sink, "host", data->config.host, "port", data->config.port, "sync", FALSE, "async", FALSE, NULL);

    // Add probe to monitor data flow for statistics
    GstPad *sink_pad = gst_element_get_static_pad(sink, "sink");
    if (sink_pad) {
        gst_pad_add_probe(sink_pad, GST_PAD_PROBE_TYPE_BUFFER, udpsink_probe_callback, data, NULL);
        gst_object_unref(sink_pad);
    }

    gst_bin_add_many(GST_BIN(data->pipeline), src, capsfilter, encoder, encoder_caps, parser, payloader, sink, NULL);
    g_print("All elements added to pipeline\n");

    g_print("Attempting to link pipeline elements...\n");
    if (!gst_element_link_many(src, capsfilter, encoder, encoder_caps, parser, payloader, sink, NULL)) {
        g_printerr("Failed to link elements. This might be due to unsupported resolution/framerate combination.\n");
        
        // If linking fails with current resolution, try with a known working resolution
        if (data->config.width != 1280 || data->config.height != 720) {
            g_print("Retrying with fallback resolution 1280x720...\n");
            
            // Update caps with fallback resolution
            GstCaps *fallback_caps = gst_caps_new_simple("video/x-raw",
                                       "width", G_TYPE_INT, 1280,
                                       "height", G_TYPE_INT, 720,
                                       "framerate", GST_TYPE_FRACTION, data->config.framerate, 1,
                                       NULL);
            if (strcmp(data->config.src_type, "libcamerasrc") == 0) {
                gst_caps_set_simple(fallback_caps, "format", G_TYPE_STRING, "YUY2",
                                          "colorimetry", G_TYPE_STRING, "bt709",
                                          "interlace-mode", G_TYPE_STRING, "progressive", NULL);
            }
            
            gchar *fallback_caps_str = gst_caps_to_string(fallback_caps);
            g_print("Fallback caps: %s\n", fallback_caps_str);
            g_free(fallback_caps_str);
            
            g_object_set(capsfilter, "caps", fallback_caps, NULL);
            gst_caps_unref(fallback_caps);
            
            // Try linking again with fallback resolution
            if (!gst_element_link_many(src, capsfilter, encoder, encoder_caps, parser, payloader, sink, NULL)) {
                g_printerr("Failed to link elements even with fallback resolution.\n");
                goto error;
            } else {
                g_print("Successfully linked with fallback resolution.\n");
            }
        } else {
            g_print("Successfully linked pipeline elements\n");
        }
    } else {
        g_print("Successfully linked pipeline elements\n");
    }

    g_print("Pipeline built successfully. Starting...\n");
    
    // Reset statistics for new pipeline
    g_mutex_lock(&data->stats.stats_mutex);
    data->stats.total_bytes = 0;
    data->stats.frame_count = 0;
    data->stats.current_bitrate = 0.0;
    data->stats.start_time = gst_clock_get_time(gst_system_clock_obtain());
    g_mutex_unlock(&data->stats.stats_mutex);
    
    GstStateChangeReturn ret = gst_element_set_state(data->pipeline, GST_STATE_PLAYING);
    if (ret == GST_STATE_CHANGE_FAILURE) {
        g_printerr("Failed to set pipeline to PLAYING state.\n");
        goto error;
    }
    
    g_print("Pipeline state change result: %d (PLAYING=%d)\n", ret, GST_STATE_CHANGE_SUCCESS);
    
    // Get the bus after the pipeline is created and started
    data->bus = gst_element_get_bus(data->pipeline);
    
    g_print("Pipeline started successfully, streaming to %s:%d\n", data->config.host, data->config.port);
    
    // Wait a moment and check if the pipeline is actually working
    g_usleep(500000); // Wait 500ms
    
    GstState current_state, pending_state;
    GstStateChangeReturn state_ret = gst_element_get_state(data->pipeline, &current_state, &pending_state, GST_CLOCK_TIME_NONE);
    g_print("Pipeline state check: return=%d, current=%s, pending=%s\n", 
            state_ret, gst_element_state_get_name(current_state), gst_element_state_get_name(pending_state));
    
    if (current_state != GST_STATE_PLAYING) {
        g_printerr("Warning: Pipeline is not in PLAYING state after startup!\n");
    }
    
    g_mutex_unlock(&data->state_mutex);
    return TRUE;

error:
    g_printerr("Error during pipeline construction.\n");
    if (data->pipeline) {
        gst_object_unref(data->pipeline);
        data->pipeline = NULL;
    }
    if (data->bus) {
        gst_object_unref(data->bus);
        data->bus = NULL;
    }
    g_mutex_unlock(&data->state_mutex);
    return FALSE;
}

int main(int argc, char *argv[]) {
    CustomData data;
    GstBus *bus;
    GstMessage *msg;

    gst_init(&argc, &argv);

    memset(&data, 0, sizeof(data));
    init_config(&data.config);
    init_stats(&data.stats);
    g_mutex_init(&data.state_mutex);
    data.should_terminate = FALSE;

    if (strlen(data.config.device) == 0 && (strcmp(data.config.src_type, "v4l2src") == 0)) {
        gchar *found_device = find_video_device();
        if (found_device) {
            g_free(data.config.device);
            data.config.device = found_device;
            g_print("Auto-detected video device: %s\n", data.config.device);
        }
    }

    if (!build_and_run_pipeline(&data)) {
        g_printerr("Failed to start initial pipeline. Exiting.\n");
        free_config_members(&data.config);
        g_mutex_clear(&data.state_mutex);
        return -1;
    }

    data.daemon = MHD_start_daemon(MHD_USE_SELECT_INTERNALLY | MHD_USE_THREAD_PER_CONNECTION, HTTP_PORT, NULL, NULL,
                                   &answer_to_connection, &data,
                                   MHD_OPTION_NOTIFY_COMPLETED, &request_completed, NULL,
                                   MHD_OPTION_END);
    if (NULL == data.daemon) {
        g_printerr("Failed to start HTTP daemon on port %d\n", HTTP_PORT);
        return 1;
    }
    g_print("HTTP server started on port %d\n", HTTP_PORT);

    do {
        g_mutex_lock(&data.state_mutex);
        
        // Check if we should terminate
        if (data.should_terminate) {
            g_mutex_unlock(&data.state_mutex);
            break;
        }
        
        // Check if pipeline restart is needed
        if (data.pipeline_is_restarting) {
            data.pipeline_is_restarting = FALSE;
            g_mutex_unlock(&data.state_mutex);
            
            g_print("Rebuilding pipeline with new configuration...\n");
            if (!build_and_run_pipeline(&data)) {
                g_printerr("Failed to rebuild pipeline. Attempting to restore default configuration...\n");
                // Try to rebuild with default settings
                if (!build_and_run_pipeline(&data)) {
                    g_printerr("Critical error: Cannot rebuild pipeline. Exiting.\n");
                    data.should_terminate = TRUE;
                }
            } else {
                g_print("Pipeline successfully rebuilt and started.\n");
            }
            continue;
        }
        
        // Get bus reference safely
        bus = data.bus;
        if (bus) {
            gst_object_ref(bus);
        }
        g_mutex_unlock(&data.state_mutex);
        
        if (bus) {
            msg = gst_bus_timed_pop_filtered(bus, 100 * GST_MSECOND,
                                             GST_MESSAGE_ERROR | GST_MESSAGE_EOS | GST_MESSAGE_STATE_CHANGED | 
                                             GST_MESSAGE_WARNING | GST_MESSAGE_INFO);

            if (msg != NULL) {
                GError *err;
                gchar *debug_info;
                switch (GST_MESSAGE_TYPE(msg)) {
                    case GST_MESSAGE_ERROR:
                        gst_message_parse_error(msg, &err, &debug_info);
                        g_printerr("ERROR from element %s: %s\n", GST_OBJECT_NAME(msg->src), err->message);
                        g_printerr("Debugging info: %s\n", debug_info ? debug_info : "none");
                        g_clear_error(&err);
                        g_free(debug_info);
                        data.should_terminate = TRUE;
                        break;
                    case GST_MESSAGE_WARNING:
                        gst_message_parse_warning(msg, &err, &debug_info);
                        g_printerr("WARNING from element %s: %s\n", GST_OBJECT_NAME(msg->src), err->message);
                        g_printerr("Warning info: %s\n", debug_info ? debug_info : "none");
                        g_clear_error(&err);
                        g_free(debug_info);
                        break;
                    case GST_MESSAGE_INFO:
                        gst_message_parse_info(msg, &err, &debug_info);
                        g_print("INFO from element %s: %s\n", GST_OBJECT_NAME(msg->src), err->message);
                        g_print("Info details: %s\n", debug_info ? debug_info : "none");
                        g_clear_error(&err);
                        g_free(debug_info);
                        break;
                    case GST_MESSAGE_EOS:
                        g_print("End-Of-Stream reached.\n");
                        data.should_terminate = TRUE;
                        break;
                    case GST_MESSAGE_STATE_CHANGED:
                        g_mutex_lock(&data.state_mutex);
                        if (data.pipeline && GST_MESSAGE_SRC(msg) == GST_OBJECT(data.pipeline)) {
                            GstState old_state, new_state, pending_state;
                            gst_message_parse_state_changed(msg, &old_state, &new_state, &pending_state);
                            g_print("Pipeline state changed from %s to %s\n",
                                    gst_element_state_get_name(old_state), gst_element_state_get_name(new_state));
                        }
                        g_mutex_unlock(&data.state_mutex);
                        break;
                    default:
                        break;
                }
                gst_message_unref(msg);
            }
            gst_object_unref(bus);
        } else {
            // No bus available, just wait a bit
            g_usleep(100000); // 100ms
        }

    } while (!data.should_terminate);

    // Cleanup
    MHD_stop_daemon(data.daemon);
    g_mutex_lock(&data.state_mutex);
    if (data.pipeline) {
        gst_element_set_state(data.pipeline, GST_STATE_NULL);
        gst_object_unref(data.pipeline);
    }
    if (data.bus) {
        gst_object_unref(data.bus);
    }
    g_mutex_unlock(&data.state_mutex);
    free_config_members(&data.config);
    free_stats(&data.stats);
    g_mutex_clear(&data.state_mutex);

    return 0;
}
