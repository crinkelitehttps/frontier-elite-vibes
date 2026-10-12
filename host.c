/*
 * This is used by the C output mode of the 68k assembler.
 * Loads 68k executable into m68k memory and applies all the relocations.
 * Executable is in an Atari .prg format.
 */

#include "host.h"

#ifdef PART1

#ifdef M68K_DEBUG
int line_no;
#endif /* M68K_DEBUG */

/* m68000 state ----------------------------------------------------- */
s8 m68kram[MEM_SIZE];    // RAM for 68K
union Reg Regs[16];      // Register state (16 regs)
s32 N, nZ, V, C, X;      // CPU flags
s32 bN, bnZ, bV, bC, bX; // Backup flags
s32 rdest;               // Return address from interrupt
s32 exceptions_pending;
u32 exception_handlers[32];      // Handlers for exceptions
char *STRam;                     // Pointer to system RAM

#endif /* part1 */

#ifdef PART2

void SetReg(int reg, int val)
{
    Regs[reg]._s32 = val; // Set the 32-bit value of the specified register
}

int GetReg(int reg)
{
    return Regs[reg]._s32; // Return the 32-bit value of the specified register
}

/* Memory Read/Write Functions ----------------------------------- */

int MemReadLong(p68K pos)
{
#ifdef M68K_DEBUG
    BOUNDS_CHECK(pos, 4);
#endif
    return do_get_mem_long((u32 *)(m68kram + pos)); // Read a long (32-bit) from memory
}

short MemReadWord(p68K pos)
{
#ifdef M68K_DEBUG
    BOUNDS_CHECK(pos, 2);
#endif
    return do_get_mem_word((u16 *)(m68kram + pos)); // Read a word (16-bit) from memory
}

char MemReadByte(p68K pos)
{
#ifdef M68K_DEBUG
    BOUNDS_CHECK(pos, 1);
#endif
    return do_get_mem_byte((u8 *)(m68kram + pos)); // Read a byte (8-bit) from memory
}

void MemWriteByte(p68K pos, int val)
{
#ifdef M68K_DEBUG
    BOUNDS_CHECK(pos, 1);
#endif
    do_put_mem_byte((u8 *)(m68kram + pos), (u8)val); // Write a byte (8-bit) to memory
}

void MemWriteWord(p68K pos, int val)
{
#ifdef M68K_DEBUG
    BOUNDS_CHECK(pos, 2);
#endif
    do_put_mem_word((u16 *)(m68kram + pos), (u16)val); // Write a word (16-bit) to memory
}

void MemWriteLong(p68K pos, int val)
{
#ifdef M68K_DEBUG
    BOUNDS_CHECK(pos, 4);
#endif
    do_put_mem_long((u32 *)(m68kram + pos), (u32)val); // Write a long (32-bit) to memory
}

/* Flag Exception Handling -------------------------------------- */

/* Called from the timer thread. A flagged exception runs once however many
 * times it was flagged. (This used to be a count set here and decremented by
 * the game's thread, unlocked: a race could leave it below zero, and then
 * the VBL handler ran at every jump until the next tick - minutes of game
 * time in 20 ms.) */
void FlagException(int num)
{
    if (exception_handlers[num])
        EXCEPTIONS_PENDING_OR(1 << num);
}

/* The game's thread: the lowest pending exception with a handler, now no
 * longer pending, or -1 */
int TakeException(void)
{
    for (int i = 0; i < 32; i++)
    {
        if (!(EXCEPTIONS_PENDING_AND(~(1 << i)) & (1 << i)))
            continue;
        if (exception_handlers[i])
            return i;
    }
    return -1;
}

/* Bin Loader (In-place) ---------------------------------------- */
static s32 buf_pos;

static s32 get_fixup(s32 reloc, s32 code_end)
{
    s32 old_bufpos;
    s32 next;
    static s32 reloc_pos;

    old_bufpos = buf_pos;
    if (reloc == 0)
    {
        buf_pos = code_end;
        reloc = rdlong(buf_pos); // Read the relocation value
        buf_pos += 4;
        reloc_pos = buf_pos;
        buf_pos = old_bufpos;
        if (reloc == 0)
            return 0; // No relocation found
        else
            return reloc + 0x1c + LOAD_BASE; // Return the adjusted relocation address
    }
    else
    {
        buf_pos = reloc_pos;
    again:
        next = (u8)rdbyte(buf_pos); // Read the next byte for relocation fixup
        buf_pos++;
        if (next == 0)
        {
            buf_pos = old_bufpos;
            return 0; // End of relocation
        }
        else if (next == 1)
        {
            reloc += 254; // Adjust relocation by 254 if specified
            goto again;
        }
        else
        {
            reloc += next;
        }
        reloc_pos = buf_pos;
        buf_pos = old_bufpos;
        return reloc;
    }
}

/* FE2_USE_MODDED comes from CMake (GLF_MODDED_FE2); modded by default */
#ifndef FE2_USE_MODDED
#define FE2_USE_MODDED 1
#endif

#if FE2_USE_MODDED
#include "fe2_bin.h"
#define FE2_BIN fe2_modded_s_bin
#define FE2_BIN_LEN fe2_modded_s_bin_len
#else
#include "fe2_orig_bin.h"
#define FE2_BIN fe2_s_bin
#define FE2_BIN_LEN fe2_s_bin_len
#endif

void load_binfile(const char *bin_filename)
{
    s32 reloc, next, pos, code_end, i = 0;

    unsigned char *bin_data = FE2_BIN;
    unsigned int len = FE2_BIN_LEN;

    assert(len + LOAD_BASE < MEM_SIZE);         // Ensure we don't exceed memory bounds
    memcpy(m68kram + LOAD_BASE, bin_data, len); // Load binary into RAM

    buf_pos = LOAD_BASE + 2;
    code_end = LOAD_BASE + 0x1c + rdlong(buf_pos); // Set code end based on relocation info

    i = 0;
    reloc = get_fixup(0, code_end);
    while (reloc)
    {
        i++;
        pos = buf_pos;
        buf_pos = reloc;
        next = rdlong(buf_pos);
        next += LOAD_BASE;
        wrlong(buf_pos, next);
        if (next > code_end)
        {
            // log_printf(stderr, "Reloc 0x%x (0x%x) out of range..\n", next, reloc + LOAD_BASE);
            log_printf("Reloc 0x%x (0x%x) out of range..\n", next, reloc + LOAD_BASE);
        }
        buf_pos = pos;
        reloc = get_fixup(reloc, code_end);
    }

    log_printf("Loaded %s Binary from memory: 0x%x bytes (code end 0x%x), %d fixups; loaded at 0x%x end 0x%x\n", bin_filename, (int)len, code_end, i, LOAD_BASE, MEM_SIZE);
}

#ifdef M68K_DEBUG
void m68k_print_line_no()
{
    printf("Hello. At fe2_modded.s line %d.\n", line_no);
    fflush(stdout);
}
#endif /* M68K_DEBUG */

#endif /* part2 */