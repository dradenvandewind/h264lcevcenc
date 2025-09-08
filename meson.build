project('gst-dual-encoder', 'c',
  version: '1.0.0',
  default_options: ['c_std=c11', 'buildtype=release']
)

# Compiler flags
add_project_arguments(['-Wall', '-Wextra', '-fPIC', '-O2'], language: 'c')

# Dependencies
gst_deps = [
  dependency('gstreamer-1.0'),
  dependency('gstreamer-video-1.0'),
  dependency('gstreamer-base-1.0')
]

# x264 dependency (using cc.find_library for better cross-platform support)
x264_dep = dependency('x264', required: true)
if not x264_dep.found()
  # Fallback: try to find it manually
  x264_dep = declare_dependency(
    compile_args: ['-I/usr/local/include'],
    link_args: ['-lx264']
  )
endif

# xeve dependency - multiple possible include paths
xeve_include_dirs = [
  '/usr/include/xeve',
  '/usr/local/include/xeve', 
  '/usr/local/include/'
]

xeve_found = false
xeve_compile_args = []
xeve_link_args = ['-lxeve']

# Check for xeve headers
foreach dir : xeve_include_dirs
  if not xeve_found and run_command('[', '-f', dir / 'xeve.h', ']', check: false).returncode() == 0
    xeve_compile_args += ['-I' + dir]
    xeve_found = true
  endif
endforeach

if not xeve_found
  error('xeve headers not found in any of the expected locations')
endif

xeve_dep = declare_dependency(
  compile_args: xeve_compile_args,
  link_args: xeve_link_args
)

# Combine all dependencies
dependencies = gst_deps + [x264_dep, xeve_dep]

# Plugin installation directory
gst_plugin_dir = get_option('gst_plugin_dir')
if gst_plugin_dir == ''
  gst_dep = dependency('gstreamer-1.0')
  gst_plugin_dir = gst_dep.get_variable(pkgconfig: 'pluginsdir', default_value: '')
  if gst_plugin_dir == ''
    error('Could not determine GStreamer plugin directory')
  endif
endif

# Source files
sources = ['gstdualencoder.c']

# Build the plugin
shared_library('gstdualencoder',
  sources,
  dependencies: dependencies,
  install: true,
  install_dir: gst_plugin_dir,
  name_prefix: 'lib'
)

# Custom target to check dependencies (optional)
run_target('check-deps',
  command: [
    'sh', '-c', '''
    echo "Checking dependencies..."
    pkg-config --exists gstreamer-1.0 gstreamer-video-1.0 gstreamer-base-1.0 || \
      (echo "Error: GStreamer not found" && exit 1)
    echo "GStreamer: OK"
    
    [ -f /usr/include/x264.h ] || [ -f /usr/local/include/x264.h ] || \
      (echo "Error: x264 headers not found" && exit 1)
    echo "x264: OK"
    
    xeve_found=false
    for dir in /usr/include/xeve /usr/local/include/xeve /usr/local/include/; do
      [ -f "$dir/xeve.h" ] && xeve_found=true && break
    done
    $xeve_found || (echo "Error: xeve headers not found" && exit 1)
    echo "xeve: OK"
    echo "All dependencies are installed!"
    '''
  ]
)
