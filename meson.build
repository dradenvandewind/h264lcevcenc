project('gst-h264-lcevc', 'c',
  version : '1.0.0',
  meson_version : '>= 1.4.0',
  default_options : [ 'warning_level=1',
                      'buildtype=debugoptimized' ])

# Generate config.h with necessary definitions for GStreamer plugin macros
conf = configuration_data()
conf.set_quoted('PACKAGE', meson.project_name())
conf.set_quoted('PACKAGE_NAME', meson.project_name())
conf.set_quoted('PACKAGE_STRING', meson.project_name() + ' ' + meson.project_version())
conf.set_quoted('PACKAGE_TARNAME', meson.project_name())
conf.set_quoted('PACKAGE_VERSION', meson.project_version())
conf.set_quoted('VERSION', meson.project_version())
conf.set_quoted('GST_PACKAGE_NAME', meson.project_name())
conf.set_quoted('GST_PACKAGE_ORIGIN', 'Unknown')
conf.set_quoted('GETTEXT_PACKAGE', meson.project_name())

configure_file(output: 'config.h', configuration: conf)
configinc = include_directories('.')

# Get compiler
cc = meson.get_compiler('c')

# Get basic GStreamer dependencies
gst_dep = dependency('gstreamer-1.0', required: true)
gstbase_dep = dependency('gstreamer-base-1.0', required: true)
gstvideo_dep = dependency('gstreamer-video-1.0', required: true)
gstpbutils_dep = dependency('gstreamer-pbutils-1.0', required: true)

# Get options (defined in meson_options.txt)
gpl_option = get_option('gpl')
x264_option = get_option('x264')

# Handle string-based options (combo options return string values)
gpl_allowed = false
if gpl_option == 'enabled' or gpl_option == 'auto'
  gpl_allowed = true
endif

build_x264 = false
if x264_option == 'enabled' or (x264_option == 'auto' and gpl_allowed)
  build_x264 = true
endif

# Check if x264 is explicitly enabled but GPL is disabled
if x264_option == 'enabled' and not gpl_allowed
  error('''Plugin x264 explicitly required via options but GPL-licensed plugins disabled via options.
Pass option -Dgpl=enabled to Meson to allow GPL-licensed plugins to be built.''')
endif

# Plugin source files
h264lcevc_sources = [
  'gsth264lcevcenc.c',
  'gstencoderbitrateprofilemanager.c',
]

h264lcevc_headers = [
  'gsth264lcevcenc.h',
  'gstencoderbitrateprofilemanager.h',
]

# XEVE library search paths
xeve_lib_dirs = [
  '/usr/local/lib',
  '/usr/lib/x86_64-linux-gnu', 
  '/usr/lib',
  '/usr/lib64',
]

xeve_inc_dirs = [
  '/usr/local/include',
  '/usr/include',
  '/usr/local/include/xeve',
]

# Find XEVE library
xeve_dep = cc.find_library('xeve',
  required : true,
  dirs : xeve_lib_dirs)

# Configure includes for XEVE
xeve_inc = include_directories(xeve_inc_dirs)

# Math library
mathlib = cc.find_library('m', required: true)

# Check for XEVE headers
if not cc.has_header('xeve.h')
  error('xeve.h header not found. Please install libxeve-dev or check installation path')
endif

# Base dependencies for the plugin
base_deps = [gstbase_dep, gstvideo_dep, gstpbutils_dep, xeve_dep, mathlib]
plugin_args = ['-DHAVE_CONFIG_H']

# Optional x264 support
x264_dep = dependency('', required: false)  # Initialize empty dependency
if build_x264
  x264_dep = dependency('x264', version : '>=0.156', required : true)
  if x264_dep.found()
    base_deps += [x264_dep]
    plugin_args += ['-DHAVE_X264=1']
    
    # Handle additional x264 libraries if specified
    x264_libraries = get_option('x264_libraries')
    if x264_libraries != '' and x264_libraries != 'auto'
      plugin_args += ['-DHAVE_X264_ADDITIONAL_LIBRARIES="@0@"'.format(x264_libraries)]
      gmodule_dep = dependency('gmodule-2.0', required: true)
      base_deps += [gmodule_dep]
    endif
  endif
endif

# Build the plugin library
gsth264lcevc = library('gsth264lcevc',
  h264lcevc_sources,
  c_args : plugin_args,
  include_directories : [configinc, xeve_inc],
  dependencies : base_deps,
  install : true,
  install_dir : join_paths(get_option('libdir'), 'gstreamer-1.0'),
  name_prefix : '',  # Don't add 'lib' prefix for GStreamer plugins
)

# Install preset file if it exists
preset_file = 'GstX264Enc.prs'
# Use run_command to check if file exists (fs module not available in older Meson)
file_check = run_command('[', '-f', preset_file, ']', check: false)
if file_check.returncode() == 0
  install_data(
    sources: preset_file, 
    install_dir: join_paths(get_option('datadir'), 'gstreamer-1.0', 'presets')
  )
  
  # Set up development environment for presets
  env = environment()
  env.prepend('GST_PRESET_PATH', meson.current_source_dir())
  meson.add_devenv(env)
endif

# Plugin registration information
plugin_summary = {
  'XEVE support': xeve_dep.found(),
  'x264 support': x264_dep.found(),
  'GPL allowed': gpl_allowed,
}

summary(plugin_summary, section: 'Plugin Features')