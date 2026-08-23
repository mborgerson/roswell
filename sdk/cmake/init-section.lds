/* Make sure the INIT & .rsrc sections are at the end of the module so we can reclaim the space */
SECTIONS
{
  /* One output section for every PAGE* orphan (PAGE, PAGECONS, PAGEDATA,
     PAGELK, ...).  Nothing pages in this kernel, so keeping them as
     separate output sections costs a page of alignment padding each and
     buys nothing. */
  PAGE BLOCK(__section_alignment__) : ALIGN(__file_alignment__)
  {
    *(PAGE*)
  }
  INIT BLOCK(__section_alignment__) : ALIGN(__file_alignment__)
  {
    __init_start__ = . ;
    *(INIT)
    *(INIT$*)
    /* INITDATA is freed with INIT; as its own output section it burned a
       page of padding for 4 bytes of content. */
    *(INITDATA)
    __init_end__ = . ;
  }
  .rsrc BLOCK(__section_alignment__) : ALIGN(__file_alignment__)
  {
    __rsrc_start__ = . ;
    *(.rsrc)
    __rsrc_end__ = . ;
  }
  /* No unwinding in kernel mode: C is built with
     -fno-asynchronous-unwind-tables; the residue comes from prebuilt
     libgcc objects (64-bit division helpers).  Drop it at link time so
     the VA is never allocated. */
  /DISCARD/ : { *(.eh_frame) }
}
INSERT BEFORE .reloc; /* .reloc is always at the end */
