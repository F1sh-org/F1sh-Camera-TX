#include <gst/gst.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <microhttpd.h>
#include <jansson.h>

#ifdef __APPLE__
#include <TargetConditionals.h>
#endif

#define HTTP_PORT 8888

// Default configuration
#define DEFAULT_HOST "127.0.0.1"
#define DEFAULT_PORT 5000
#define DEFAULT_WIDTH 1280
#define DEFAULT_HEIGHT 720
#define DEFAULT_FRAMERATE 30

// Application configuration
typedef struct {
    gchar *host;
    gint port;
    gchar *camera_name;
    gchar *encoder_type;
    gint width;
    gint height;
    gint framerate;
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

struct connection_info_struct {
    char *json_data;
    size_t data_size;
    CustomData *custom_data;
};

// Function declarations
static gboolean build_and_run_pipeline(CustomData *data);
static void init_config(AppConfig *config);
static void free_config_members(AppConfig *config);
static void init_stats(StreamStats *stats);
static void free_stats(StreamStats *stats);

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
        
        // Print debug info every 60 frames (about every 1 second at 60fps)
        if (data->stats.frame_count % 60 == 0) {
            g_print("Streaming: frame %llu, size %zu bytes, total %llu bytes\n", 
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
    config->camera_name = g_strdup(""); // Auto-detect
    config->encoder_type = g_strdup("v4l2h264enc");
    config->width = DEFAULT_WIDTH;
    config->height = DEFAULT_HEIGHT;
    config->framerate = DEFAULT_FRAMERATE;
}

void free_config_members(AppConfig *config) {
    g_free(config->host);
    g_free(config->camera_name);
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

// Get available libcamera cameras
static json_t* get_available_cameras() {
    json_t *cameras = json_array();
    
    // Create a temporary libcamerasrc element to query available cameras
    GstElement *src = gst_element_factory_make("libcamerasrc", "temp_src");
    if (src) {
        // Get the camera-name property to see available cameras
        GParamSpec *pspec = g_object_class_find_property(G_OBJECT_GET_CLASS(src), "camera-name");
        if (pspec && G_IS_PARAM_SPEC_STRING(pspec)) {
            // Try to enumerate cameras by testing different camera names
            // This is a simplified approach - in practice, libcamera provides better APIs
            const gchar *common_cameras[] = {
                "/base/soc/i2c0mux/i2c@1/imx708@1a",  // Pi Camera Module 3
                "/base/soc/i2c0mux/i2c@1/imx219@10",  // Pi Camera Module v2
                "/base/soc/i2c0mux/i2c@1/ov5647@36",  // Pi Camera Module v1
                NULL
            };
            
            for (int i = 0; common_cameras[i]; i++) {
                json_array_append_new(cameras, json_string(common_cameras[i]));
            }
        }
        gst_object_unref(src);
    }
    
    // If no specific cameras found, add a generic entry
    if (json_array_size(cameras) == 0) {
        json_array_append_new(cameras, json_string("auto-detect"));
    }
    
    return cameras;
}

// Get available encoders
static json_t* get_available_encoders() {
    json_t *encoders = json_array();
    
    const gchar *encoder_list[] = {
        "v4l2h264enc",
        "x264enc",
        "nvh264enc",      // NVIDIA hardware encoder
        "vaapih264enc",   // Intel VAAPI encoder
        "omxh264enc",     // OpenMAX encoder
        NULL
    };
    
    for (int i = 0; encoder_list[i]; i++) {
        GstElementFactory *factory = gst_element_factory_find(encoder_list[i]);
        if (factory) {
            json_array_append_new(encoders, json_string(encoder_list[i]));
            gst_object_unref(factory);
        }
    }
    
    return encoders;
}

// Get supported resolutions for a camera
static json_t* get_camera_resolutions(const gchar *camera_name) {
    json_t *resolutions = json_array();
    
    // Common resolutions supported by most Pi cameras
    const struct {
        int width;
        int height;
        int max_framerate;
    } common_resolutions[] = {
        {640, 480, 90},
        {1280, 720, 60},
        {1920, 1080, 30},
        {2304, 1296, 25},  // Pi Camera Module 3 native
        {4608, 2592, 10},  // Pi Camera Module 3 full resolution
        {0, 0, 0}  // End marker
    };
    
    for (int i = 0; common_resolutions[i].width > 0; i++) {
        json_t *res = json_object();
        json_object_set_new(res, "width", json_integer(common_resolutions[i].width));
        json_object_set_new(res, "height", json_integer(common_resolutions[i].height));
        json_object_set_new(res, "max_framerate", json_integer(common_resolutions[i].max_framerate));
        json_array_append_new(resolutions, res);
    }
    
    return resolutions;
}

// Send JSON response
static enum MHD_Result send_json_response(struct MHD_Connection *connection, const char *json_str, int status_code) {
    struct MHD_Response *response = MHD_create_response_from_buffer(strlen(json_str), 
                                                                   (void*)json_str, 
                                                                   MHD_RESPMEM_MUST_COPY);
    MHD_add_response_header(response, "Content-Type", "application/json");
    MHD_add_response_header(response, "Access-Control-Allow-Origin", "*");
    
    enum MHD_Result ret = MHD_queue_response(connection, status_code, response);
    MHD_destroy_response(response);
    
    return ret;
}

// Handle health check
static enum MHD_Result handle_health(struct MHD_Connection *connection) {
    return send_json_response(connection, "{\"status\":\"healthy\",\"service\":\"F1sh-Camera-TX\"}", MHD_HTTP_OK);
}

// Handle statistics
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

// Handle GET requests
static enum MHD_Result handle_get_request(struct MHD_Connection *connection, const char *url) {
    if (strcmp(url, "/get") == 0) {
        // Return available cameras and encoders
        json_t *root = json_object();
        json_object_set_new(root, "cameras", get_available_cameras());
        json_object_set_new(root, "encoders", get_available_encoders());
        
        char *json_str = json_dumps(root, JSON_INDENT(2));
        json_decref(root);
        
        enum MHD_Result ret = send_json_response(connection, json_str, MHD_HTTP_OK);
        free(json_str);
        return ret;
    } else if (strncmp(url, "/get/", 5) == 0) {
        // Return camera-specific information
        const char *camera_name = url + 5;
        
        json_t *root = json_object();
        json_object_set_new(root, "camera", json_string(camera_name));
        json_object_set_new(root, "supported_resolutions", get_camera_resolutions(camera_name));
        
        char *json_str = json_dumps(root, JSON_INDENT(2));
        json_decref(root);
        
        enum MHD_Result ret = send_json_response(connection, json_str, MHD_HTTP_OK);
        free(json_str);
        return ret;
    }
    
    return send_json_response(connection, "{\"error\":\"Not Found\"}", MHD_HTTP_NOT_FOUND);
}

// Process configuration update
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
    
    value = json_object_get(root, "camera");
    if (json_is_string(value)) {
        str_val = json_string_value(value);
        if (strcmp(data->config.camera_name, str_val) != 0) {
            g_free(data->config.camera_name);
            data->config.camera_name = g_strdup(str_val);
            needs_pipeline_rebuild = TRUE;
        }
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
        if (new_width >= 320 && new_width <= 4608 && new_width != data->config.width) {
            data->config.width = new_width;
            needs_pipeline_rebuild = TRUE;
        } else if (new_width < 320 || new_width > 4608) {
            g_print("Warning: Invalid width %d, keeping current value %d\n", new_width, data->config.width);
        }
    }
    
    value = json_object_get(root, "height");
    if (json_is_integer(value)) {
        int new_height = json_integer_value(value);
        if (new_height >= 240 && new_height <= 2592 && new_height != data->config.height) {
            data->config.height = new_height;
            needs_pipeline_rebuild = TRUE;
        } else if (new_height < 240 || new_height > 2592) {
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

    // Decide what kind of update to perform
    if (needs_pipeline_rebuild) {
        g_print("Configuration change requires pipeline rebuild: host=%s, port=%d, camera=%s, encoder=%s, %dx%d@%dfps\n", 
                data->config.host, data->config.port, data->config.camera_name, data->config.encoder_type,
                data->config.width, data->config.height, data->config.framerate);
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
        *con_cls = (void *)1; // Mark as handled for GET requests
    }

    if (0 == strcmp(method, "GET")) {
        if (0 == strcmp(url, "/health")) return handle_health(connection);
        if (0 == strcmp(url, "/stats")) return handle_get_stats(connection, data);
        if (0 == strncmp(url, "/get", 4)) return handle_get_request(connection, url);
    } else if (0 == strcmp(method, "POST") && 0 == strcmp(url, "/config")) {
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
    g_print("Building pipeline with config: host=%s, port=%d, camera=%s, encoder=%s, %dx%d@%dfps\n",
            data->config.host, data->config.port, data->config.camera_name, data->config.encoder_type,
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

    GstElement *src, *capsfilter, *convert, *encoder, *parser, *payloader, *sink;
    GstCaps *caps;

    src = gst_element_factory_make("libcamerasrc", "source");
    if (!src) {
        g_printerr("Failed to create libcamerasrc.\n");
        goto error;
    }
    g_print("Successfully created libcamerasrc element\n");
    
    // Set camera name if specified
    if (strlen(data->config.camera_name) > 0 && strcmp(data->config.camera_name, "auto-detect") != 0) {
        g_object_set(src, "camera-name", data->config.camera_name, NULL);
        g_print("Using camera: %s\n", data->config.camera_name);
    } else {
        g_print("Using auto-detected camera\n");
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
    
    gchar *caps_str = gst_caps_to_string(caps);
    g_print("Setting caps: %s\n", caps_str);
    g_free(caps_str);
    
    g_object_set(capsfilter, "caps", caps, NULL);
    gst_caps_unref(caps);

    convert = gst_element_factory_make("videoconvert", "convert");
    if (!convert) {
        g_printerr("Failed to create videoconvert element.\n");
        goto error;
    }

    // Try encoders in order of preference with better error handling
    const gchar *encoder_fallbacks[] = {
        data->config.encoder_type,  // First try the requested encoder
        "v4l2h264enc",             // Hardware encoder (Pi default)
        "omxh264enc",              // OpenMAX encoder (Pi fallback)
        "x264enc",                 // Software fallback
        "nvh264enc",               // NVIDIA if available
        "vaapih264enc",            // Intel VAAPI if available
        NULL
    };
    
    encoder = NULL;
    gchar *actual_encoder_name = NULL;
    
    for (int i = 0; encoder_fallbacks[i] && !encoder; i++) {
        // Skip if we already tried this encoder
        if (i > 0 && strcmp(encoder_fallbacks[i], data->config.encoder_type) == 0) {
            continue;
        }
        
        g_print("Trying encoder: %s\n", encoder_fallbacks[i]);
        encoder = gst_element_factory_make(encoder_fallbacks[i], "encoder");
        if (encoder) {
            actual_encoder_name = g_strdup(encoder_fallbacks[i]);
            g_print("Successfully created encoder: %s\n", actual_encoder_name);
            break;
        } else {
            g_print("Encoder %s not available\n", encoder_fallbacks[i]);
        }
    }
    
    if (!encoder) {
        g_printerr("No suitable encoder found after trying all fallbacks.\n");
        goto error;
    }
    
    // Configure encoder settings based on the actual encoder being used
    if (strcmp(actual_encoder_name, "x264enc") == 0) {
        g_print("Configuring x264enc encoder\n");
        g_object_set(encoder, 
                     "tune", 0x00000004,  // zerolatency
                     "speed-preset", 1,   // superfast
                     "bitrate", 2048,     // 2Mbps
                     "threads", 1,        // Single thread for low latency
                     "key-int-max", 30,   // GOP size
                     NULL);
    } else if (strcmp(actual_encoder_name, "v4l2h264enc") == 0) {
        g_print("Configuring v4l2h264enc encoder with minimal settings\n");
        // Use absolutely minimal settings - don't set any extra controls that might fail
        // Let the encoder use its defaults
        (void)encoder; // Just acknowledge we have the encoder, don't configure it
    } else if (strcmp(actual_encoder_name, "omxh264enc") == 0) {
        g_print("Configuring omxh264enc encoder\n");
        g_object_set(encoder,
                     "target-bitrate", 2048000,  // 2Mbps in bits/sec
                     "control-rate", 2,          // variable bitrate
                     NULL);
    } else if (strcmp(actual_encoder_name, "nvh264enc") == 0) {
        g_print("Configuring nvh264enc encoder\n");
        g_object_set(encoder,
                     "bitrate", 2048,
                     "gop-size", 30,
                     "preset", 1,  // low-latency-hq
                     NULL);
    } else if (strcmp(actual_encoder_name, "vaapih264enc") == 0) {
        g_print("Configuring vaapih264enc encoder\n");
        g_object_set(encoder,
                     "bitrate", 2048,
                     "keyframe-period", 30,
                     NULL);
    }
    
    g_free(actual_encoder_name);

    parser = gst_element_factory_make("h264parse", "parser");
    if (!parser) {
        g_printerr("Failed to create h264parse element.\n");
        goto error;
    }

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
    g_object_set(sink, "host", data->config.host, "port", data->config.port, "sync", FALSE, "async", FALSE, NULL);

    // Add probe to monitor data flow for statistics
    GstPad *sink_pad = gst_element_get_static_pad(sink, "sink");
    if (sink_pad) {
        gst_pad_add_probe(sink_pad, GST_PAD_PROBE_TYPE_BUFFER, udpsink_probe_callback, data, NULL);
        gst_object_unref(sink_pad);
    }

    gst_bin_add_many(GST_BIN(data->pipeline), src, capsfilter, convert, encoder, parser, payloader, sink, NULL);
    g_print("All elements added to pipeline\n");

    g_print("Attempting to link pipeline elements...\n");
    if (!gst_element_link_many(src, capsfilter, convert, encoder, parser, payloader, sink, NULL)) {
        g_printerr("Failed to link elements.\n");
        goto error;
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

    if (!build_and_run_pipeline(&data)) {
        g_printerr("Failed to start initial pipeline. Exiting.\n");
        free_config_members(&data.config);
        g_mutex_clear(&data.state_mutex);
        return -1;
    }

    data.daemon = MHD_start_daemon(MHD_USE_INTERNAL_POLLING_THREAD, HTTP_PORT, NULL, NULL,
                                   &answer_to_connection, &data,
                                   MHD_OPTION_NOTIFY_COMPLETED, request_completed, NULL,
                                   MHD_OPTION_END);
    if (NULL == data.daemon) {
        g_printerr("Failed to start HTTP server.\n");
        return -1;
    }

    g_print("HTTP server started on port %d\n", HTTP_PORT);
    g_print("Available endpoints:\n");
    g_print("  GET  /health - Health check\n");
    g_print("  GET  /stats - Stream statistics\n");
    g_print("  GET  /get - List available cameras and encoders\n");
    g_print("  GET  /get/[camera_name] - Get camera-specific information\n");
    g_print("  POST /config - Update configuration\n");

    do {
        g_mutex_lock(&data.state_mutex);
        
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
                g_printerr("Failed to rebuild pipeline.\n");
                data.should_terminate = TRUE;
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
                        
                        // Log encoder errors but don't auto-fallback to avoid infinite loops
                        if (strstr(GST_OBJECT_NAME(msg->src), "encoder")) {
                            g_printerr("Encoder error detected. Consider using a different encoder via /config API.\n");
                            g_printerr("Try: curl -X POST http://localhost:8888/config -d '{\"encoder\":\"x264enc\"}'\n");
                        }
                        
                        data.should_terminate = TRUE;
                        g_clear_error(&err);
                        g_free(debug_info);
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
