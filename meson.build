project(
  'F1sh-Camera-TX',
  'c',
  meson_version : '>= 1.3.0',
  version : '0.1',
  default_options : ['warning_level=3'],
)

dependencies = [
  dependency('gstreamer-1.0'),
  dependency('gstreamer-app-1.0'),
  dependency('gstreamer-video-1.0'),
  dependency('libmicrohttpd'),
  dependency('jansson'),
]

exe = executable(
  'F1sh-Camera-TX',
  'f1sh_camera_tx_new.c',
  dependencies : dependencies,
  install : true,
)

test('basic', exe)
