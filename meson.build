project('radfu', 'c',
  version : '0.0.1',
  license : 'AGPL-3.0',
  default_options : ['c_std=c11', 'warning_level=3'])

# Get version from git describe (tag or sha), fallback to project version
git = find_program('git', required : false)
if git.found()
  git_describe = run_command(git, 'describe', '--tags', '--always', check : false)
  if git_describe.returncode() == 0
    version = git_describe.stdout().strip()
  else
    version = meson.project_version()
  endif
else
  version = meson.project_version()
endif

# Pass version to source code
add_project_arguments('-DVERSION="' + version + '"', language : 'c')

src = files(
  'src/main.c',
  'src/radfu.c',
  'src/rapacker.c',
  'src/raosis.c',
  'src/progress.c',
  'src/compat.c',
)

# Platform-specific source files
if host_machine.system() == 'windows'
  src += files('src/port_windows.c')
  src += files('src/raconnect_windows.c')
  src += files('src/getopt.c')
  platform_src = files('src/port_windows.c', 'src/raconnect_windows.c', 'src/getopt.c', 'src/compat.c')
elif host_machine.system() == 'darwin'
  src += files('src/port_macos.c')
  src += files('src/raconnect.c')
  platform_src = files('src/port_macos.c', 'src/raconnect.c', 'src/compat.c')
else
  src += files('src/port_linux.c')
  src += files('src/raconnect.c')
  platform_src = files('src/port_linux.c', 'src/raconnect.c', 'src/compat.c')
endif

# Platform-specific dependencies
cc = meson.get_compiler('c')
deps = []
link_args = []

if host_machine.system() == 'windows'
  # Windows requires SetupAPI for USB device detection
  deps += cc.find_library('setupapi')
elif host_machine.system() == 'darwin'
  # macOS requires IOKit and CoreFoundation for USB device detection
  deps += dependency('appleframeworks', modules : ['IOKit', 'CoreFoundation'])
elif get_option('static')
  link_args += '-static'
endif

radfu_exe = executable('radfu', src,
  dependencies : deps,
  link_args : link_args,
  install : true)

# Man page generation with help2man
help2man = find_program('help2man', required : false)
if help2man.found()
  custom_target('radfu.1',
    output : 'radfu.1',
    input : 'radfu.h2m',
    command : [help2man,
      '--include=@INPUT@',
      '--section=1',
      '--no-info',
      '--output=@OUTPUT@',
      radfu_exe],
    depends : radfu_exe,
    install : true,
    install_dir : get_option('mandir') / 'man1')
endif

# Tests
cmocka = dependency('cmocka', required : get_option('tests'))
if cmocka.found()
  test_rapacker = executable('test_rapacker',
    'tests/test_rapacker.c',
    'src/rapacker.c',
    dependencies : cmocka)
  test('rapacker', test_rapacker)

  test_radfu = executable('test_radfu',
    'tests/test_radfu.c',
    'src/radfu.c',
    'src/rapacker.c',
    'src/progress.c',
    platform_src,
    c_args : ['-DTESTING'],
    dependencies : [cmocka] + deps)
  test('radfu', test_radfu)

  test_protocol = executable('test_protocol',
    'tests/test_protocol.c',
    'tests/mock/ramock.c',
    'src/rapacker.c',
    platform_src,
    dependencies : [cmocka] + deps)
  test('protocol', test_protocol)

  test_raosis = executable('test_raosis',
    'tests/test_raosis.c',
    'src/raosis.c',
    'src/rapacker.c',
    platform_src,
    dependencies : [cmocka] + deps)
  test('raosis', test_raosis)
endif
