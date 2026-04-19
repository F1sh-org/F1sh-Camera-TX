project(
  'F1sh-Camera-TX',
  'c', 'cpp',
  meson_version : '>= 1.3.0',
  version : '0.1',
  default_options : ['warning_level=3', 'cpp_std=c++14'],
)

# Protobuf compiler
protoc = find_program('protoc', required: true)
grpc_cpp_plugin = find_program('grpc_cpp_plugin', required: true)

# Generate protobuf and gRPC files
protobuf_gen = generator(protoc,
  output: ['@BASENAME@.pb.cc', '@BASENAME@.pb.h'],
  arguments: ['--proto_path=@CURRENT_SOURCE_DIR@',
              '--cpp_out=@BUILD_DIR@',
              '@INPUT@'])

grpc_gen = custom_target('grpc_gen',
  input: 'f1sh_camera.proto',
  output: ['f1sh_camera.grpc.pb.cc', 'f1sh_camera.grpc.pb.h'],
  command: [protoc,
            '--proto_path=@CURRENT_SOURCE_DIR@',
            '--grpc_out=@OUTDIR@',
            '--plugin=protoc-gen-grpc=' + grpc_cpp_plugin.full_path(),
            '@INPUT@'])

proto_src = protobuf_gen.process('f1sh_camera.proto')
grpc_src = grpc_gen

# Core dependencies (required)
dependencies = [
  dependency('gstreamer-1.0'),
  dependency('gstreamer-app-1.0'),
  dependency('gstreamer-video-1.0'),
  dependency('grpc++'),
  dependency('protobuf'),
  dependency('jansson'),
]

# Find grpc++ reflection library (needed for grpcurl service discovery)
grpc_reflection_dep = dependency('grpc++_reflection', required: false)
if not grpc_reflection_dep.found()
  # Try pkg-config alternative name
  grpc_reflection_dep = meson.get_compiler('cpp').find_library('grpc++_reflection', required: false)
endif

# Compile definitions
compile_args = []

if grpc_reflection_dep.found()
  dependencies += grpc_reflection_dep
  compile_args += '-DHAVE_GRPC_REFLECTION=1'
  message('grpc++_reflection found - service reflection enabled')
else
  message('grpc++_reflection not found - service reflection disabled')
endif

# Optional: Avahi for mDNS service discovery (Linux only)
avahi_client_dep = dependency('avahi-client', required: false)
avahi_glib_dep = dependency('avahi-glib', required: false)

if avahi_client_dep.found() and avahi_glib_dep.found()
  dependencies += [avahi_client_dep, avahi_glib_dep]
  message('Avahi found - mDNS service advertisement will be enabled')
else
  message('Avahi not found - mDNS service advertisement will be disabled')
endif

exe = executable(
  'F1sh-Camera-TX',
  ['f1sh_camera_tx.c', 'grpc_server.cpp', proto_src, grpc_src],
  dependencies : dependencies,
  cpp_args : compile_args,
  install : true,
)

test('basic', exe)
