project('radfu', 'c',
  version : '0.0.1',
  license : 'AGPL-3.0',
  default_options : ['c_std=c11', 'warning_level=3'])

src = files(
  'src/main.c',
  'src/radfu.c',
  'src/raconnect.c',
  'src/rapacker.c',
  'src/progress.c',
)

# Platform-specific source files
if host_machine.system() == 'darwin'
  src += files('src/port_macos.c')
else
  src += files('src/port_linux.c')
endif

# Platform-specific dependencies
deps = []
link_args = []

if host_machine.system() == 'darwin'
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
endif
