// gRPC service implementation with C wrapper for F1sh Camera TX
#include <grpcpp/grpcpp.h>
#include <grpcpp/health_check_service_interface.h>

// Server reflection support (optional - for grpcurl compatibility)
#ifdef HAVE_GRPC_REFLECTION
#include <grpcpp/ext/proto_server_reflection_plugin.h>
#endif

#include "f1sh_camera.grpc.pb.h"
#include "f1sh_camera.pb.h"

extern "C" {
#include "grpc_wrapper.h"
}

using grpc::Server;
using grpc::ServerBuilder;
using grpc::ServerContext;
using grpc::Status;

using f1sh_camera::F1shCameraService;
using f1sh_camera::HealthRequest;
using f1sh_camera::HealthResponse;
using f1sh_camera::GetStatsRequest;
using f1sh_camera::GetStatsResponse;
using f1sh_camera::GetConfigRequest;
using f1sh_camera::GetConfigResponse;
using f1sh_camera::UpdateConfigRequest;
using f1sh_camera::UpdateConfigResponse;
using f1sh_camera::SwapResolutionRequest;
using f1sh_camera::SwapResolutionResponse;
using f1sh_camera::UpdateHostRequest;
using f1sh_camera::UpdateHostResponse;
using f1sh_camera::GetAvailableDevicesRequest;
using f1sh_camera::GetAvailableDevicesResponse;

// gRPC service implementation
class F1shCameraServiceImpl final : public F1shCameraService::Service {
public:
    F1shCameraServiceImpl(const grpc_callbacks* callbacks) : callbacks_(*callbacks) {}

    Status Health(ServerContext* context, const HealthRequest* request,
                  HealthResponse* response) override {
        char* status = nullptr;
        callbacks_.health_callback(callbacks_.user_data, &status);
        if (status) {
            response->set_status(status);
            free(status);
        } else {
            response->set_status("healthy");
        }
        return Status::OK;
    }

    Status GetStats(ServerContext* context, const GetStatsRequest* request,
                    GetStatsResponse* response) override {
        uint64_t total_bytes = 0, frame_count = 0;
        double bitrate = 0.0;

        callbacks_.get_stats_callback(callbacks_.user_data, &total_bytes, &frame_count, &bitrate);

        auto* stats = response->mutable_stats();
        stats->set_total_bytes(total_bytes);
        stats->set_frame_count(frame_count);
        stats->set_current_bitrate(bitrate);

        return Status::OK;
    }

    Status GetConfig(ServerContext* context, const GetConfigRequest* request,
                     GetConfigResponse* response) override {
        grpc_config_t config = {0};
        callbacks_.get_config_callback(callbacks_.user_data, &config);

        auto* cfg = response->mutable_config();
        if (config.host) cfg->set_host(config.host);
        cfg->set_port(config.port);
        if (config.camera_name) cfg->set_camera_name(config.camera_name);
        if (config.encoder_type) cfg->set_encoder_type(config.encoder_type);
        cfg->set_width(config.width);
        cfg->set_height(config.height);
        cfg->set_framerate(config.framerate);

        // Free allocated strings
        free(config.host);
        free(config.camera_name);
        free(config.encoder_type);

        return Status::OK;
    }

    Status UpdateConfig(ServerContext* context, const UpdateConfigRequest* request,
                        UpdateConfigResponse* response) override {
        grpc_config_update_t update = {0};

        if (request->has_host()) {
            update.host = strdup(request->host().c_str());
            update.has_host = 1;
        }
        if (request->has_port()) {
            update.port = request->port();
            update.has_port = 1;
        }
        if (request->has_camera_name()) {
            update.camera_name = strdup(request->camera_name().c_str());
            update.has_camera_name = 1;
        }
        if (request->has_encoder_type()) {
            update.encoder_type = strdup(request->encoder_type().c_str());
            update.has_encoder_type = 1;
        }
        if (request->has_width()) {
            update.width = request->width();
            update.has_width = 1;
        }
        if (request->has_height()) {
            update.height = request->height();
            update.has_height = 1;
        }
        if (request->has_framerate()) {
            update.framerate = request->framerate();
            update.has_framerate = 1;
        }

        grpc_config_t new_config = {0};
        char* error_msg = nullptr;
        int success = callbacks_.update_config_callback(callbacks_.user_data, &update, &new_config, &error_msg);

        response->set_success(success != 0);
        if (error_msg) {
            response->set_message(error_msg);
            free(error_msg);
        } else {
            response->set_message(success ? "Configuration updated successfully" : "Failed to update configuration");
        }

        if (success) {
            auto* cfg = response->mutable_config();
            if (new_config.host) cfg->set_host(new_config.host);
            cfg->set_port(new_config.port);
            if (new_config.camera_name) cfg->set_camera_name(new_config.camera_name);
            if (new_config.encoder_type) cfg->set_encoder_type(new_config.encoder_type);
            cfg->set_width(new_config.width);
            cfg->set_height(new_config.height);
            cfg->set_framerate(new_config.framerate);
        }

        // Cleanup
        free((void*)update.host);
        free((void*)update.camera_name);
        free((void*)update.encoder_type);
        free(new_config.host);
        free(new_config.camera_name);
        free(new_config.encoder_type);

        return Status::OK;
    }

    Status SwapResolution(ServerContext* context, const SwapResolutionRequest* request,
                          SwapResolutionResponse* response) override {
        grpc_config_t new_config = {0};
        char* error_msg = nullptr;
        int success = callbacks_.swap_resolution_callback(callbacks_.user_data, &new_config, &error_msg);

        response->set_success(success != 0);
        if (error_msg) {
            response->set_message(error_msg);
            free(error_msg);
        } else {
            response->set_message(success ? "Resolution swapped successfully" : "Failed to swap resolution");
        }

        if (success) {
            auto* cfg = response->mutable_config();
            if (new_config.host) cfg->set_host(new_config.host);
            cfg->set_port(new_config.port);
            if (new_config.camera_name) cfg->set_camera_name(new_config.camera_name);
            if (new_config.encoder_type) cfg->set_encoder_type(new_config.encoder_type);
            cfg->set_width(new_config.width);
            cfg->set_height(new_config.height);
            cfg->set_framerate(new_config.framerate);

            free(new_config.host);
            free(new_config.camera_name);
            free(new_config.encoder_type);
        }

        return Status::OK;
    }

    Status UpdateHost(ServerContext* context, const UpdateHostRequest* request,
                      UpdateHostResponse* response) override {
        char* error_msg = nullptr;
        int success = callbacks_.update_host_callback(callbacks_.user_data, request->host().c_str(), &error_msg);

        response->set_success(success != 0);
        if (error_msg) {
            response->set_message(error_msg);
            free(error_msg);
        } else {
            response->set_message(success ? "Host updated successfully" : "Failed to update host");
        }

        return Status::OK;
    }

    Status GetAvailableDevices(ServerContext* context, const GetAvailableDevicesRequest* request,
                               GetAvailableDevicesResponse* response) override {
        grpc_devices_t devices = {0};
        callbacks_.get_devices_callback(callbacks_.user_data, &devices);

        auto* devs = response->mutable_devices();

        // Add cameras
        for (int i = 0; i < devices.num_cameras; i++) {
            auto* cam = devs->add_cameras();
            if (devices.cameras[i].name) cam->set_name(devices.cameras[i].name);
            if (devices.cameras[i].path) cam->set_path(devices.cameras[i].path);
            free(devices.cameras[i].name);
            free(devices.cameras[i].path);
        }

        // Add encoders
        for (int i = 0; i < devices.num_encoders; i++) {
            auto* enc = devs->add_encoders();
            if (devices.encoders[i].name) enc->set_name(devices.encoders[i].name);
            enc->set_available(devices.encoders[i].available);
            free(devices.encoders[i].name);
        }

        free(devices.cameras);
        free(devices.encoders);

        return Status::OK;
    }

private:
    grpc_callbacks callbacks_;
};

// C wrapper implementation
struct f1sh_grpc_server {
    std::unique_ptr<Server> server;
    std::unique_ptr<F1shCameraServiceImpl> service;
};

extern "C" f1sh_grpc_server_t* f1sh_grpc_server_start(const char* address, const grpc_callbacks* callbacks) {
    f1sh_grpc_server_t* srv = new f1sh_grpc_server_t();

    srv->service = std::make_unique<F1shCameraServiceImpl>(callbacks);

    grpc::EnableDefaultHealthCheckService(true);

#ifdef HAVE_GRPC_REFLECTION
    // Enable server reflection for grpcurl/service discovery
    grpc::reflection::InitProtoReflectionServerBuilderPlugin();
#endif

    ServerBuilder builder;
    builder.AddListeningPort(address, grpc::InsecureServerCredentials());
    builder.RegisterService(srv->service.get());

    srv->server = builder.BuildAndStart();

    if (!srv->server) {
        delete srv;
        return nullptr;
    }

    return srv;
}

extern "C" void f1sh_grpc_server_stop(f1sh_grpc_server_t* server) {
    if (server) {
        if (server->server) {
            server->server->Shutdown();
        }
        delete server;
    }
}

extern "C" void f1sh_grpc_server_wait(f1sh_grpc_server_t* server) {
    if (server && server->server) {
        server->server->Wait();
    }
}
