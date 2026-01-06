project(
  'F1sh-Camera-TX',
  'c',
  meson_version : '>= 1.3.0',
  version : '0.1',
  default_options : ['warning_level=3'],
)

# Core dependencies (required)
dependencies = [
  dependency('gstreamer-1.0'),
  dependency('gstreamer-app-1.0'),
  dependency('gstreamer-video-1.0'),
  dependency('libmicrohttpd'),
  dependency('jansson'),
]

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
  'f1sh_camera_tx.c',
  dependencies : dependencies,
  install : true,
)

test('basic', exe)
