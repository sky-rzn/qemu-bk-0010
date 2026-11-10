k1801vm1_ss = ss.source_set()
k1801vm1_ss.add(files(
  'cpu.c',
  'disas.c',
  'helper.c',
  'translate.c',
))

k1801vm1_common_system_ss = ss.source_set()
k1801vm1_common_system_ss.add(files('machine.c'))

target_arch += {'k1801vm1': k1801vm1_ss}
target_common_system_arch += {'k1801vm1': k1801vm1_common_system_ss}
