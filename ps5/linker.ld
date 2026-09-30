OUTPUT_FORMAT("elf64-x86-64", "elf64-x86-64", "elf64-x86-64")
OUTPUT_ARCH(i386:x86-64)
ENTRY(_start)

/* The payload executes from a PS5 JIT shared-memory alias mapped
 * PROT_READ|PROT_EXECUTE. That mapping is not writable, so unlike a normal
 * flat binary the image cannot simply fold .data and .bss in with .text --
 * the first write to any global would fault.
 *
 * Instead the writable data gets its own region, placed immediately after the
 * code in virtual address space. _start maps anonymous RW memory over that
 * range before touching a single global, which keeps every reference from the
 * code RIP-relative and in range. The initial contents of .data ride along
 * inside the read-only code image at __data_load and are copied across during
 * that same bootstrap; .bss is NOLOAD and never occupies a byte of the blob.
 */

PHDRS
{
    text PT_LOAD FLAGS(5);   /* R-X: the JIT mapping                        */
    data PT_LOAD FLAGS(6);   /* RW-: mapped by _start, not present in file  */
}

SECTIONS
{
    . = 0;

    .text : {
        *(.text._start)
        *(.text)
        *(.text.*)
        *(.rodata)
        *(.rodata.*)
        . = ALIGN(16);
    } :text

    /* Dynamic relocations, KEPT rather than discarded.
     *
     * -static-pie emits R_X86_64_RELATIVE entries for every initialised
     * pointer. LuaGB and LuaMD discarded these and hand-patched their cores to
     * build such tables at runtime instead -- workable for a Game Boy core with
     * two offenders, hopeless for PCSX-ReARMed, whose interpreter dispatch,
     * GTE function tables and plugin structs are all initialised pointer data.
     *
     * Keeping the section and applying it in _start is what a real loader does,
     * and it removes the entire class of bug. The section must live below
     * __data_load so it travels inside the blob. */
    . = ALIGN(8);
    .rela.dyn : {
        __rela_start = .;
        *(.rela.dyn)
        *(.rela.data*)
        *(.rela.rodata*)
        *(.rela.text*)
        *(.rela.got*)
        __rela_end = .;
    } :text
    . = ALIGN(16);

    /* Load address of .data: straight after the code, so `objcopy -O binary`
       emits one contiguous blob of code + initial data and nothing else. */
    __data_load = .;

    /* .data's runtime (VMA) address. This MUST be a fixed constant, not
     * derived from SIZEOF(.data): the VMA has to be known before the section
     * is emitted, because C code addresses every .data/.bss global by its VMA
     * (PC-relative) and the linker computes every R_X86_64_RELATIVE r_offset /
     * r_addend against it. If the VMA were the blob address, _start would copy
     * the image to the RW region but every global access and every relocation
     * would still target the read-only blob -- writes fault, reads see
     * unrelocated link-time offsets.
     *
     * AT(__data_load) keeps the LMA (and thus the blob image) right after the
     * code; the VMA is the RW region _start mmaps. The ASSERT proves the init
     * image still fits in the gap; if .data ever outgrows it, raise
     * __data_start (and the JIT reservation follows automatically via mklua). */
    __data_start = 0x100000;
    .data __data_start : AT(__data_load) {
        *(.data)
        *(.data.*)

        /* Even -static-pie leaves a couple of real GOT slots that it could not
           relax into `lea`, and each holds a link-time address that the
           discarded .rela.dyn was supposed to fix up. _start adds the load base
           to every non-zero entry between __got_start/__got_end; without that,
           the first call through one jumps into unmapped memory. */
        . = ALIGN(8);          /* loose bytes in .data would misalign the walk */
        __got_start = .;
        *(.got)
        *(.got.plt)
        . = ALIGN(8);
        __got_end = .;

        . = ALIGN(16);
        __data_end = .;
    } :data

    __data_image_end = __data_load + SIZEOF(.data);
    ASSERT(__data_image_end <= __data_start, ".data init image overruns __data_start")

    /* .bss follows the RW data; its VMA is already the runtime address. */
    . = __data_end;

    .bss (NOLOAD) : {
        __bss_start = .;
        *(.bss)
        *(.bss.*)
        *(COMMON)
        . = ALIGN(16);
        __bss_end = .;
    } :data

    /* Total span the Lua launcher must reserve: code + data + bss. */
    __image_end = .;

    /DISCARD/ : {
        *(.comment)
        *(.note.*)
        *(.eh_frame*)
        *(.interp)
        *(.dynsym)
        *(.dynstr)
        *(.dynamic)
        *(.gnu.hash)
        *(.gnu.version*)
        *(.rel.*)
        *(.plt)
        *(.plt.*)
    }
}
