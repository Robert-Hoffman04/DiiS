#include "../inc.h"
#include "../tests/tests.h"

extern const char txt_buildtime[];

struct MenuEntry
{
    const char* text;
    void* ptr;
    int type; // 0: test, 1: submenu, 2: function that just prints some stuff (don't print OK afterwards, just wait for B button)
};

class Menu
{
public:
    const MenuEntry* entries;
    const char* title;
    const Menu* parent;

    int get_entry_count() const
    {
        int count = 0;
        const MenuEntry* p = entries;
        while ((p++)->text != nullptr) count++;
        return count;
    }

    void draw() const
    {
        // draw title, buttons
        clear_screen();
        draw_string(3, 0, title);

        for(int i = 0; i < get_entry_count(); i++)
        {
            draw_string(2, 2+i, entries[i].text);
        }

        // only in main menu: draw disclaimer + build time
        if (parent == nullptr)
        {
            draw_string(3, 21, " WIP  CONTACT: ROCKPOLISH");
            draw_string(3, 22, " MAY STILL CONTAIN BUGS  ");
            draw_string(3, 23, txt_buildtime);
        }
    }
};




extern const Menu menu_main, menu_armv4, menu_armv5, menu_ipc, menu_dsmath, menu_memory, menu_initialstate;

const MenuEntry menu_main_entries[] = {
    {"ARMv4", (void*)&menu_armv4, 1},
    {"ARMv5", (void*)&menu_armv5, 1},
    {"IPC", (void*)&menu_ipc, 1},
    {"DS MATH", (void*)&menu_dsmath, 1},
    {"MEMORY", (void*)&menu_memory, 1},
    {"INITIAL STATE", (void*)&menu_initialstate, 1},
    {nullptr, nullptr, 0}
};

const MenuEntry menu_armv4_entries[] = {
    {"CONDITION CODES", (void*)&run_tests_armv4_conditioncodes, 0},
    {nullptr, nullptr, 0}
};

const MenuEntry menu_armv5_entries[] = {
    {"CLZ", (void*)&run_tests_armv5_clz, 0},
    {"QADD, QSUB", (void*)&run_tests_armv5_qadd_qsub, 0},
    {"QDADD, QDSUB", (void*)&run_tests_armv5_qdadd_qdsub, 0},
    {"SMULxy", (void*)&run_tests_armv5_SMULxy, 0},
    {"SMLAxy", (void*)&run_tests_armv5_SMLAxy, 0},
    {"SMULWy", (void*)&run_tests_armv5_SMULWy, 0},
    {"SMLAWy", (void*)&run_tests_armv5_SMLAWy, 0},
    {"SMLALxy", (void*)&run_tests_armv5_SMLALxy, 0},
    {"BLX", (void*)&run_tests_armv5_blx, 0},
    {"LDR r15, POP {r15}, LDM {r15}", (void*)&run_tests_armv5_ldrpopr15, 0},
    {"LDM / STM", (void*)&run_tests_armv5_ldm_stm, 0},
    {nullptr, nullptr, 0}
};


const MenuEntry menu_ipc_entries[] = {
    {"IPCSYNC", (void*)&run_tests_ipcsync, 0},
    {"IPCFIFO", (void*)&run_tests_ipcfifo, 0},
    {"IPCFIFO IRQ", (void*)&run_tests_ipcfifo_irq, 0},
    {nullptr, nullptr, 0}
};

const MenuEntry menu_dsmath_entries[] = {
    {"SQRT 32", (void*)&run_tests_ds_maths_sqrt_32, 0},
    {"SQRT 64", (void*)&run_tests_ds_maths_sqrt_64, 0},
    {"DIV 32/32", (void*)&run_tests_ds_maths_div_32_32, 0},
    {"DIV 64/32", (void*)&run_tests_ds_maths_div_64_32, 0},
    {"DIV 64/64", (void*)&run_tests_ds_maths_div_64_64, 0},
    {nullptr, nullptr, 0}
};

const MenuEntry menu_memory_entries[] = {
    {"WRAM CNT", (void*)&run_tests_wramcnt, 0},
    {"VRAM CNT", (void*)&run_tests_vramcnt, 0},
    {"TCM", (void*)&run_tests_tcm, 0},
    {nullptr, nullptr, 0}
};

const MenuEntry menu_initialstate_entries[] = {
    {"IPC/IRQ/CPSR", (void*)&run_tests_initialstate_ipc_irq_cpsr, 2},
    {"CP15", (void*)&run_tests_initialstate_cp15, 2},
    {nullptr, nullptr, 0}
};

const Menu menu_main = {menu_main_entries, "ROCKWRESTLER", nullptr};
const Menu menu_armv4 = {menu_armv4_entries, "ARMv4", &menu_main};
const Menu menu_armv5 = {menu_armv5_entries, "ARMv5", &menu_main};
const Menu menu_ipc = {menu_ipc_entries, "IPC TESTS", &menu_main};
const Menu menu_dsmath = {menu_dsmath_entries, "DS MATH", &menu_main};
const Menu menu_memory = {menu_memory_entries, "MEMORY CONTROL", &menu_main};
const Menu menu_initialstate = {menu_initialstate_entries, "INITIAL STATE", &menu_main};

//---------------------------------------------------------------------------
// §19 autorun driver: headless, slot-2 result reporting.
//
// Replaces the interactive A/B/up/down menu loop (the stock upstream
// behavior -- everything above this point, including menu_main/menu_armv4/
// etc., is untouched and documents the test groupings / run order) with a
// flat walk over every real test (MenuEntry.type == 0) across the five
// test-category submenus, skipping "INITIAL STATE" (type 2: informational
// register dumps only, no pass/fail -- see UPSTREAM-README.md).
//
// Same slot-2/ExpMemory convention as tools/armwrestler and
// tools/arm7wrestler (see PROVENANCE.md): 0x09000000+ is flat host RAM
// through the "Memory Expansion Pak" addon, an ordinary memory-mapped
// store from the guest's point of view (no different from the ROM's own
// enable_trace()/disable_trace() debug hooks at 0x08004400/0x08005500,
// common.cpp) and readable 1:1 by the Wii host regardless of which guest
// CPU wrote it.
//
// A failing test (fail_test/timeout_test/timeout_rw, menu.s) never returns
// to its caller -- it jumps back to arm9_main's main_post_init label
// (main.s), which calls back into cpp_menu() from scratch. autorun_slot is
// a plain global (not touched by that restart -- it isn't part of
// arm9_main's one-time boot init, see main.s), advanced *before* each test
// is invoked, so re-entry after a failure resumes at the next slot instead
// of repeating the one that just failed.
//
// Layout (32-bit words, all little-endian as written by the guest):
//   0x09000000  sentinel (0 until done, then 'RKW1')
//   0x09000004  total tests run
//   0x09000008  total tests failed
//   0x0900000C  fail-log entry count (capped at AUTORUN_MAX_SLOTS)
//   0x09000010 + i*8   fail-log entry i: {name ptr (live ARM9 address into
//                      this ROM's own rodata -- the MenuEntry.text this
//                      test came from), detail (the fail_test/timeout_test
//                      sub-case number, or -1 for timeout_rw)}
//---------------------------------------------------------------------------

static const Menu* const autorun_submenus[] = {
    &menu_armv4, &menu_armv5, &menu_ipc, &menu_dsmath, &menu_memory
};

static const int AUTORUN_MAX_SLOTS = 32;
static volatile u32* const RW_BASE = (volatile u32*)0x09000000;

int autorun_slot = 0;
int autorun_running_slot = -1; // slot currently executing, or -1; read by cpp_fail_test()

// find the Nth real test (type == 0) across all autorun_submenus, in order
static const MenuEntry* autorun_find(int want_slot)
{
    int slot = 0;
    for (const Menu* m : autorun_submenus)
    {
        for (int i = 0; m->entries[i].text != nullptr; i++)
        {
            if (m->entries[i].type != 0) continue; // skip submenus / info-only entries
            if (slot == want_slot) return &m->entries[i];
            slot++;
        }
    }
    return nullptr;
}

extern "C" void cpp_menu()
{
    static bool cleared = false;
    if (!cleared)
    {
        for (u32 i = 0; i < 512/4; i++) RW_BASE[i] = 0;
        cleared = true;
    }

    while (true)
    {
        const MenuEntry* entry = autorun_find(autorun_slot);
        if (!entry)
        {
            RW_BASE[0] = 0x31574B52; // 'RKW1' -- done sentinel, written last
            while (true) { }
        }

        autorun_running_slot = autorun_slot;
        autorun_slot++; // advance first: a failure below never returns here

        voidfncptr f = (voidfncptr)entry->ptr;
        call_fncptr(f); // use call_fncptr so compiler doesn't generate blx

        // reached only on success -- see cpp_fail_test() for the failure path
        RW_BASE[1]++; // total_run
        autorun_running_slot = -1;
    }
}

extern "C" void cpp_fail_test(const char* str, int offset, int number)
{
    // make sure we are in the right VRAM mode etc
    init_iostate();

    if (autorun_running_slot >= 0)
    {
        RW_BASE[1]++; // total_run
        RW_BASE[2]++; // total_fail

        u32 logCount = RW_BASE[3];
        if (logCount < AUTORUN_MAX_SLOTS)
        {
            const MenuEntry* entry = autorun_find(autorun_running_slot);
            RW_BASE[4 + logCount * 2]     = entry ? (u32)entry->text : 0;
            RW_BASE[4 + logCount * 2 + 1] = (u32)number; // -1 for timeout_rw
            RW_BASE[3] = logCount + 1;
        }
    }

    // draw the string (fail, timeout, ...) -- harmless when headless, kept
    // for manual debugging via screenshot
    draw_string(0, 0, str);

    // + the number of the test that failed (3 hexadecimal digits)
    // timeout_rw doesn't give a test number (-1), so don't draw that
    if (number >= 0) draw_hex_value<3>(offset, 0, number);
}
