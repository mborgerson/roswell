/* Full linker script for the SARCH=xbox kernel image (replaces both ld's
 * builtin pei-i386 script and the init-section.lds INSERT script).
 *
 * Derived from `i686-w64-mingw32-ld --verbose` with these changes, each
 * worth one resident page of section-alignment padding:
 *   - .xdata (PSEH scope tables, ~850 bytes) folds into .rdata
 *   - .edata (ld's synthesized export table, ~2 KB for the NONAME ordinal
 *     exports) folds into .rdata; the export DataDirectory tracks the
 *     placement, and pefixup/fix-edata read it from the directory
 *   - the .idata rule is dropped: with zero imports it emitted only its
 *     own 20-byte null-descriptor literal, costing a page
 *   - .eh_frame is discarded (no unwinding in kernel mode; residue comes
 *     from prebuilt libgcc objects)
 *   - PAGE*, INIT (+ INITDATA), and .rsrc are explicit output sections at
 *     the image tail so MiFreeInitializationCode can reclaim INIT/.rsrc
 *     and the PAGE orphans don't burn a page of padding each
 */
SECTIONS
{
  . = SIZEOF_HEADERS;
  . = ALIGN(__section_alignment__);
  .text  __image_base__ + ( __section_alignment__ < 0x1000 ? . : __section_alignment__ ) :
  {
    KEEP (*(SORT_NONE(.init)))
    *(.text)
    *(SORT(.text$*))
    *(.text.*)
    *(.gnu.linkonce.t.*)
    *(.glue_7t)
    *(.glue_7)
    KEEP (*(SORT_NONE(.fini)))
    *(.gcc_exc)
    PROVIDE (etext = .);
    PROVIDE (_etext = .);
    KEEP (*(.gcc_except_table))
  }
  .data ALIGN(__section_alignment__) :
  {
    __data_start__ = . ;
    *(.data)
    *(.data2)
    *(SORT(.data$*))
    KEEP(*(.jcr))
    __data_end__ = . ;
    *(.data_cygwin_nocopy)
  }
  .rdata BLOCK(__section_alignment__) :
  {
    *(.rdata)
    *(SORT(.rdata$*))
    . = ALIGN(4);
    __rt_psrelocs_start = .;
    KEEP(*(.rdata_runtime_pseudo_reloc))
    __rt_psrelocs_end = .;
    ___CTOR_LIST__ = .;
    __CTOR_LIST__ = .;
    LONG (-1);
    KEEP(*(.ctors));
    KEEP(*(.ctor));
    KEEP(*(SORT_BY_NAME(.ctors.*)));
    LONG (0);
    ___DTOR_LIST__ = .;
    __DTOR_LIST__ = .;
    LONG (-1);
    KEEP(*(.dtors));
    KEEP(*(.dtor));
    KEEP(*(SORT_BY_NAME(.dtors.*)));
    LONG (0);
    *(.xdata)
    *(SORT(.xdata$*))
    *(.edata)
  }
  ___RUNTIME_PSEUDO_RELOC_LIST_END__ = __rt_psrelocs_end;
  __RUNTIME_PSEUDO_RELOC_LIST_END__ = __rt_psrelocs_end;
  ___RUNTIME_PSEUDO_RELOC_LIST__ = __rt_psrelocs_start;
  __RUNTIME_PSEUDO_RELOC_LIST__ = __rt_psrelocs_start;
  .bss BLOCK(__section_alignment__) :
  {
    __bss_start__ = . ;
    /* Largest-alignment chunks first: the page-aligned boot/double-fault
       stacks otherwise punch ~4 KB alignment holes mid-section. */
    *(SORT_BY_ALIGNMENT(.bss))
    *(COMMON)
    __bss_end__ = . ;
  }
  PAGE BLOCK(__section_alignment__) : ALIGN(__file_alignment__)
  {
    *(PAGE*)
  }
  INIT BLOCK(__section_alignment__) : ALIGN(__file_alignment__)
  {
    __init_start__ = . ;
    *(INIT)
    *(INIT$*)
    *(INITDATA)
    *(INITDATA_RW)
    __init_end__ = . ;
  }
  .rsrc BLOCK(__section_alignment__) : SUBALIGN(4)
  {
    __rsrc_start__ = . ;
    KEEP (*(.rsrc))
    KEEP (*(.rsrc$*))
    __rsrc_end__ = . ;
  }
  .reloc BLOCK(__section_alignment__) :
  {
    *(.reloc)
  }
  .endjunk BLOCK(__section_alignment__) :
  {
    PROVIDE (end = .);
    PROVIDE ( _end = .);
    __end__ = .;
  }
  /DISCARD/ :
  {
    *(.debug$S)
    *(.debug$T)
    *(.debug$F)
    *(.drectve)
    *(.note.GNU-stack)
    *(.gnu.lto_*)
    *(.gnu_object_only)
    *(.eh_frame*)
    *(.pdata*)
  }
  .debug_aranges BLOCK(__section_alignment__) (NOLOAD) :
  {
    *(.debug_aranges)
  }
  .debug_pubnames BLOCK(__section_alignment__) (NOLOAD) :
  {
    *(.debug_pubnames)
  }
  .debug_info BLOCK(__section_alignment__) (NOLOAD) :
  {
    *(.debug_info .gnu.linkonce.wi.*)
  }
  .debug_abbrev BLOCK(__section_alignment__) (NOLOAD) :
  {
    *(.debug_abbrev)
  }
  .debug_line BLOCK(__section_alignment__) (NOLOAD) :
  {
    *(.debug_line)
  }
  .debug_frame BLOCK(__section_alignment__) (NOLOAD) :
  {
    *(.debug_frame*)
  }
  .debug_str BLOCK(__section_alignment__) (NOLOAD) :
  {
    *(.debug_str)
  }
  .debug_line_str BLOCK(__section_alignment__) (NOLOAD) :
  {
    *(.debug_line_str)
  }
  .debug_loclists BLOCK(__section_alignment__) (NOLOAD) :
  {
    *(.debug_loclists)
  }
  .debug_rnglists BLOCK(__section_alignment__) (NOLOAD) :
  {
    *(.debug_rnglists)
  }
  .debug_macinfo BLOCK(__section_alignment__) (NOLOAD) :
  {
    *(.debug_macinfo)
  }
  .debug_loc BLOCK(__section_alignment__) (NOLOAD) :
  {
    *(.debug_loc)
  }
  .debug_ranges BLOCK(__section_alignment__) (NOLOAD) :
  {
    *(.debug_ranges)
  }
}
