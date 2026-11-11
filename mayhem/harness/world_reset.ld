/* Linker fragment (-Wl,-T, augments the default script) that gathers every mutable global of the
   firmware library (libbatteryfw.a, built with -fdata-sections) into two contiguous ranges that
   world_reset.h snapshots and restores. It goes BEFORE .data/.bss so these patterns claim the input
   sections first; the sanitizer and libFuzzer runtimes are not in the library and stay untouched. */
/* Never written after startup (vtables and other relocated tables, the compiler's unnamed constants
   and switch-coverage tables): kept out of the snapshot. */
SECTIONS
{
  fwconst :
  {
    *libbatteryfw.a:*(.data.rel.ro .data.rel.ro.* .data..L__unnamed_* .data.__sancov_gen_*)
  }
}
INSERT BEFORE .data;
SECTIONS
{
  fwdata : ALIGN(32)
  {
    __fw_data_start = .;
    *libbatteryfw.a:*(.data .data.*)
    . = ALIGN(8);
    __fw_data_end = .;
  }
}
INSERT BEFORE .data;
SECTIONS
{
  fwbss : ALIGN(32)
  {
    __fw_bss_start = .;
    *libbatteryfw.a:*(.bss .bss.* COMMON)
    . = ALIGN(8);
    __fw_bss_end = .;
  }
}
INSERT BEFORE .bss;
