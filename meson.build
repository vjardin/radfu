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

executable('radfu', src, install : true)

# Tests
cmocka = dependency('cmocka', required : get_option('tests'))
if cmocka.found()
  test_rapacker = executable('test_rapacker',
    'tests/test_rapacker.c',
    'src/rapacker.c',
    dependencies : cmocka)
  test('rapacker', test_rapacker)
endif
