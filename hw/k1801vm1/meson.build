k1801vm1_ss = ss.source_set()
k1801vm1_ss.add(when: 'CONFIG_BK0010', if_true: files(
  'bk0010.c',
  'display.c',
  'keyboard.c',
  'sysregs.c',
))

hw_arch += {'k1801vm1': k1801vm1_ss}
