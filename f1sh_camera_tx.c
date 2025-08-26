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
#define DEFAULT_AUTOFOCUS TRUE

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
    gdouble actual_framerate;       // fps
    gdouble buffer_fullness;        // percentage (0-100)
    GstClockTime last_timestamp;
    GstClockTime start_time;
    GstClockTime last_frame_time;
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

// Probe callback to monitor data flow
static GstPadProbeReturn
udpsink_probe_callback (GstPad *pad __attribute__((unused)), GstPadProbeInfo *info, gpointer user_data)
{
    CustomData *data = (CustomData *)user_data;
    GstBuffer *buffer = GST_PAD_PROBE_INFO_BUFFER(info);
    
    if (buffer) {
        g_mutex_lock(&data->stats.stats_mutex);
        
        // Update byte count
        gsize buffer_size = gst_buffer_get_size(buffer);
        data->stats.total_bytes += buffer_size;
        data->stats.frame_count++;
        
        // Calculate average framerate over the entire streaming session
        GstClockTime current_time = g_get_monotonic_time(); // microseconds
        
        if (data->stats.last_frame_time == 0) {
            // First frame - just record the time
            data->stats.last_frame_time = current_time;
        } else if (data->stats.frame_count > 10) { // Only calculate after we have some frames
            // Calculate average FPS since streaming started
            GstClockTime total_time_us = current_time - data->stats.last_frame_time;
            if (total_time_us > 0) {
                // Average FPS = (frames - 1) / time_elapsed_seconds
                gdouble time_elapsed_seconds = (gdouble)total_time_us / 1000000.0;
                data->stats.actual_framerate = (gdouble)(data->stats.frame_count - 1) / time_elapsed_seconds;
                
                // Calculate buffer performance as ratio of actual vs target framerate
                gdouble fps_ratio = data->stats.actual_framerate / data->config.framerate;
                data->stats.buffer_fullness = CLAMP(fps_ratio * 100.0, 0.0, 150.0); // Cap at 150%
            }
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
    stats->actual_framerate = 0.0;
    stats->buffer_fullness = 0.0;
    stats->last_timestamp = 0;
    stats->start_time = gst_clock_get_time(gst_system_clock_obtain());
    stats->last_frame_time = 0;
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
    json_object_set_new(root, "actual_framerate", json_real(data->stats.actual_framerate));
    json_object_set_new(root, "buffer_fullness_percent", json_real(data->stats.buffer_fullness));
    json_object_set_new(root, "elapsed_time_seconds", json_real(elapsed_seconds));
    
    // Performance metrics
    gdouble target_fps = data->config.framerate;
    gdouble fps_efficiency = 0.0;
    if (target_fps > 0 && data->stats.actual_framerate > 0) {
        fps_efficiency = (data->stats.actual_framerate / target_fps) * 100.0;
        // Clamp efficiency to reasonable bounds
        fps_efficiency = CLAMP(fps_efficiency, 0.0, 200.0);
    }
    json_object_set_new(root, "target_framerate", json_real(target_fps));
    json_object_set_new(root, "framerate_efficiency_percent", json_real(fps_efficiency));
    
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

    value = json_object_get(root, "host");
    if (json_is_string(value)) {
        str_val = json_string_value(value);
        g_free(data->config.host);
        data->config.host = g_strdup(str_val);
    }
    
    value = json_object_get(root, "port");
    if (json_is_integer(value)) {
        data->config.port = json_integer_value(value);
    }
    
    value = json_object_get(root, "src");
    if (json_is_string(value)) {
        str_val = json_string_value(value);
        g_free(data->config.src_type);
        data->config.src_type = g_strdup(str_val);
    }
    
    value = json_object_get(root, "device");
    if (json_is_string(value)) {
        str_val = json_string_value(value);
        g_free(data->config.device);
        data->config.device = g_strdup(str_val);
    }
    
    value = json_object_get(root, "encoder");
    if (json_is_string(value)) {
        str_val = json_string_value(value);
        g_free(data->config.encoder_type);
        data->config.encoder_type = g_strdup(str_val);
    }
    
    value = json_object_get(root, "width");
    if (json_is_integer(value)) {
        data->config.width = json_integer_value(value);
    }
    
    value = json_object_get(root, "height");
    if (json_is_integer(value)) {
        data->config.height = json_integer_value(value);
    }
    
    value = json_object_get(root, "framerate");
    if (json_is_integer(value)) {
        data->config.framerate = json_integer_value(value);
    }

    value = json_object_get(root, "autofocus");
    if (json_is_boolean(value)) {
        data->config.autofocus = json_boolean_value(value);
    }

    value = json_object_get(root, "lens_position");
    if (json_is_real(value)) {
        data->config.lens_position = json_real_value(value);
    } else if (json_is_integer(value)) {
        data->config.lens_position = (gfloat)json_integer_value(value);
    }

    g_print("Configuration updated: host=%s, port=%d, src=%s, encoder=%s, autofocus=%s, lens_position=%.2f\n", 
            data->config.host, data->config.port, data->config.src_type, data->config.encoder_type,
            data->config.autofocus ? "enabled" : "disabled", data->config.lens_position);
    
    data->pipeline_is_restarting = TRUE;

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
    g_print("Building pipeline...\n");

    // Stop and cleanup existing pipeline
    if (data->pipeline) {
        g_print("Stopping existing pipeline.\n");
        gst_element_set_state(data->pipeline, GST_STATE_NULL);
        gst_object_unref(data->pipeline);
        data->pipeline = NULL;
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
            g_print("Autofocus disabled - using manual focus at position %.2f\n", data->config.lens_position);
            // Set manual focus mode with configurable lens position
            GstStructure *controls = gst_structure_new("controls",
                "AfMode", G_TYPE_INT, 0,                           // AfModeManual
                "LensPosition", G_TYPE_FLOAT, data->config.lens_position, // User-configurable focus
                NULL);
            g_object_set(src, "controls", controls, NULL);
            gst_structure_free(controls);
        }
    }

    // Add probe to source to timestamp buffers for latency measurement
    GstPad *src_pad = gst_element_get_static_pad(src, "src");
    if (src_pad) {
        gst_pad_add_probe(src_pad, GST_PAD_PROBE_TYPE_BUFFER, source_probe_callback, data, NULL);
        gst_object_unref(src_pad);
    }

    capsfilter = gst_element_factory_make("capsfilter", "capsfilter");
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

    parser = gst_element_factory_make("h264parse", "parser");
    
    // Add caps filter after encoder to match the working pipeline
    encoder_caps = gst_element_factory_make("capsfilter", "encoder_caps");
    GstCaps *h264_caps = gst_caps_new_simple("video/x-h264",
                                             "level", G_TYPE_STRING, "4",
                                             NULL);
    g_object_set(encoder_caps, "caps", h264_caps, NULL);
    gst_caps_unref(h264_caps);

    payloader = gst_element_factory_make("rtph264pay", "payloader");
    g_object_set(payloader, "config-interval", -1, NULL);

    sink = gst_element_factory_make("udpsink", "sink");
    g_object_set(sink, "host", data->config.host, "port", data->config.port, "sync", FALSE, "async", FALSE, NULL);

    // Add probe to monitor data flow for statistics
    GstPad *sink_pad = gst_element_get_static_pad(sink, "sink");
    if (sink_pad) {
        gst_pad_add_probe(sink_pad, GST_PAD_PROBE_TYPE_BUFFER, udpsink_probe_callback, data, NULL);
        gst_object_unref(sink_pad);
    }

    gst_bin_add_many(GST_BIN(data->pipeline), src, capsfilter, encoder, encoder_caps, parser, payloader, sink, NULL);

    if (!gst_element_link_many(src, capsfilter, encoder, encoder_caps, parser, payloader, sink, NULL)) {
        g_printerr("Failed to link elements.\n");
        goto error;
    }

    g_print("Pipeline built successfully. Starting...\n");
    
    // Reset statistics for new pipeline
    g_mutex_lock(&data->stats.stats_mutex);
    data->stats.total_bytes = 0;
    data->stats.frame_count = 0;
    data->stats.current_bitrate = 0.0;
    data->stats.actual_framerate = 0.0;
    data->stats.buffer_fullness = 0.0;
    data->stats.last_frame_time = 0; // Will be set on first frame
    data->stats.start_time = gst_clock_get_time(gst_system_clock_obtain());
    g_mutex_unlock(&data->stats.stats_mutex);
    
    gst_element_set_state(data->pipeline, GST_STATE_PLAYING);
    
    // Get the bus after the pipeline is created and started
    data->bus = gst_element_get_bus(data->pipeline);
    
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
            build_and_run_pipeline(&data);
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
                                             GST_MESSAGE_ERROR | GST_MESSAGE_EOS | GST_MESSAGE_STATE_CHANGED);

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
