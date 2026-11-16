# SPDX-License-Identifier: GPL-3.0-or-later
"""include/esp_sdr/esp_sdr_place.h under the host preprocessor: categories, rules, pins, scopes,
file pins, flag checks, the assembler form and the builds that place nothing. Stub headers stand
for the Espressif attributes, so that each region's expansion shows by name."""

import shutil
import subprocess
import textwrap

import pytest

from conftest import REPO

CC = shutil.which("cc") or shutil.which("gcc")
pytestmark = pytest.mark.skipif(CC is None, reason="no host C compiler")

STUBS = {
    "esp_attr.h": """
        #define IRAM_ATTR IRAM(__COUNTER__)
        #define DRAM_ATTR DRAM(__COUNTER__)
        #define EXT_RAM_BSS_ATTR EXTBSS(__COUNTER__)
        #define EXT_RAM_NOINIT_ATTR EXTNOINIT(__COUNTER__)
        """,
    "zephyr/kernel.h": """
        #define __aligned(x) ALIGNED(x)
        """,
    "zephyr/linker/section_tags.h": """
        #define __noinit NOINIT(__COUNTER__)
        """,
    "esp_sdr/esp_sdr.h": """
        #define ESP_SDR_HIGH_RAM HIGHSEC
        #define ESP_SDR_HIGH_BUF_DEFINE(t, n, d, a) HIGHBUF(t, n, d, a)
        #define ESP_SDR_PSRAM_BUF_DEFINE(t, n, d, a) PSRAMBUF(t, n, d, a)
        #define ESP_SDR_HIGH_STACK_DEFINE(n, s) HIGHSTACK(n, s)
        """,
}

RULES = """
    #define ESP_SDR_PLACE_PACED_SAMPLE ESP_SDR_IRAM
    #define ESP_SDR_PLACE_PACED_CHUNK ESP_SDR_IRAM
    #define ESP_SDR_PLACE_PACED_EVENT ESP_SDR_FLASH
    #define ESP_SDR_PLACE_SAMPLE ESP_SDR_FLASH
    #define ESP_SDR_PLACE_CHUNK ESP_SDR_IRAM
    #define ESP_SDR_PLACE_EVENT ESP_SDR_HOTBLOCK
    #define ESP_SDR_PLACE_STATE ESP_SDR_HIGH
    #define ESP_SDR_PLACE_BULK ESP_SDR_PSRAM
    #define ESP_SDR_PLACE_TABLE ESP_SDR_DRAM
    #define ESP_SDR_PLACE_STACK ESP_SDR_HIGH
    #define ESP_SDR_PIN_CODE_le_feed ESP_SDR_IRAM
    #define ESP_SDR_PIN_CODE_unpinned ESP_SDR_RULE
    #define ESP_SDR_PIN_DATA_lut ESP_SDR_DEFAULT
    #define ESP_SDR_PIN_DATA_stage ESP_SDR_DRAM
    #define ESP_SDR_PIN_DATA_tx_dac_stage ESP_SDR_DEFAULT
    #define ESP_SDR_PIN_DATA_heap ESP_SDR_PSRAM_NOINIT
    #define ESP_SDR_PIN_DATA_raw ESP_SDR_PSRAM_FILE
    #define ESP_SDR_PLACE_link_SAMPLE ESP_SDR_IRAM
    #define ESP_SDR_PIN_CODE_link_emit16 ESP_SDR_FLASH
    #define ESP_SDR_PIN_CODE_bad_code ESP_SDR_DEFAULT
    #define ESP_SDR_PIN_DATA_bad_data ESP_SDR_IRAM
    """


def expand(tmp_path, src, defines=("CONFIG_SOC_FAMILY_ESPRESSIF_ESP32",), rules=RULES, asm=False):
    """{label: expansion} of the lines `label: text` of src, or the failure's stderr."""
    for name, text in STUBS.items():
        p = tmp_path / "stubs" / name
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text(textwrap.dedent(text))
    (tmp_path / "esp_sdr_place_rules.h").write_text(textwrap.dedent(rules))
    f = tmp_path / ("t.S" if asm else "t.c")
    f.write_text("#include <esp_sdr/esp_sdr_place.h>\n" + textwrap.dedent(src))
    cmd = [CC, "-E", "-P", "-I", str(tmp_path / "stubs"), "-I", str(REPO / "include"),
           "-I", str(tmp_path), *[f"-D{d}" for d in defines]]
    if asm:
        cmd += ["-x", "assembler-with-cpp"]
    r = subprocess.run(cmd + [str(f)], capture_output=True, text=True)
    if r.returncode:
        return r.stderr
    out = {}
    for line in r.stdout.splitlines():
        if ":" in line:
            label, _, text = line.partition(":")
            out[label.strip()] = " ".join(text.split())
    return out


def test_rules_pins_and_fixed_categories(tmp_path):
    got = expand(tmp_path, """
        A: ESP_SDR_CODE(PACED_SAMPLE, ring_loop)
        B: ESP_SDR_CODE(SAMPLE, demod)
        C: ESP_SDR_CODE(SAMPLE, le_feed)
        D: ESP_SDR_CODE(SAMPLE, unpinned)
        E: ESP_SDR_CODE(NOCACHE, park)
        F: ESP_SDR_CODE(COLD, init)
        G: ESP_SDR_CODE(EVENT, acquire)
        H: ESP_SDR_CODE(CHUNK, poll)
        """)
    assert got["A"].startswith("IRAM(")       # the category's rule
    assert got["B"] == ""                      # flash: no attribute
    assert got["C"].startswith("IRAM(")       # a pin beats the rule
    assert got["D"] == ""                      # ESP_SDR_RULE: no pin
    assert got["E"].startswith("IRAM(")       # NOCACHE: always IRAM
    assert got["F"] == ""                      # COLD: always flash
    assert got["G"] == ""                      # the hot block without CONFIG_ESP_SDR_HOT_TEXT
    assert got["H"].startswith("IRAM(")


def test_hot_block_with_the_block_built(tmp_path):
    got = expand(tmp_path, "G: ESP_SDR_CODE(EVENT, acquire)\n",
                 defines=("CONFIG_SOC_FAMILY_ESPRESSIF_ESP32", "CONFIG_ESP_SDR_HOT_TEXT"))
    assert got["G"].startswith('__attribute__((section(".text.esp_sdr_hot."')


def test_data_regions_buffers_and_stacks(tmp_path):
    got = expand(tmp_path, """
        G: ESP_SDR_DATA_F(STATE, rs, NOINIT)
        H: ESP_SDR_DATA(BULK, cap)
        I: ESP_SDR_DATA(TABLE, lut)
        J: ESP_SDR_DATA(TABLE, other)
        K: ESP_SDR_BUF_DEFINE(BULK, int16_t, buf, [64], 16, NONE)
        L: ESP_SDR_STACK_DEFINE(ring_stack, 1536)
        M: ESP_SDR_DATA_F(BULK, heap, NOINIT)
        N: ESP_SDR_BUF_DEFINE(STATE, uint8_t, area, [32], 4, NOINIT)
        O: ESP_SDR_DATA(BULK, raw)
        """)
    assert got["G"] == "HIGHSEC"
    assert got["H"].startswith("EXTBSS(")
    assert got["I"] == ""
    assert got["J"].startswith("DRAM(")
    assert got["K"] == "PSRAMBUF(int16_t, buf, [64], 16)"
    assert got["L"] == "HIGHSTACK(ring_stack, 1536)"
    assert got["M"].startswith("EXTNOINIT(")
    assert got["N"] == "HIGHBUF(uint8_t, area, [32], 4)"
    assert got["O"] == '__attribute__((section(".ext_ram.bss")))'


def test_tokens_that_are_macros_stay_tokens(tmp_path):
    got = expand(tmp_path, """
        #define CHUNK 64
        #define STATE 3
        #define NOINIT 7
        #define ring 9
        #define SAMPLE 1
        #define le_feed 2
        C1: ESP_SDR_CODE(CHUNK, f)
        C2: ESP_SDR_DATA_F(STATE, ring, NOINIT)
        C3: ESP_SDR_CODE(SAMPLE, le_feed)
        """)
    assert got["C1"].startswith("IRAM(")
    assert got["C2"] == "HIGHSEC"
    assert got["C3"].startswith("IRAM(")


def test_file_pins_and_scopes(tmp_path):
    got = expand(tmp_path, """
        #define ESP_SDR_PLACE_FILE tx_dac
        Q1: ESP_SDR_DATA_F(STATE, stage, NOINIT)
        Q2: ESP_SDR_DATA_F(STATE, rs, NOINIT)
        """)
    assert got == {"Q1": "", "Q2": "HIGHSEC"}  # the file's pin beats the plain one
    got = expand(tmp_path, "Q3: ESP_SDR_DATA_F(STATE, stage, NOINIT)\n")
    assert got["Q3"].startswith("DRAM(")
    src = "S1: ESP_SDR_CODE(SAMPLE, fft)\nS2: ESP_SDR_CODE(SAMPLE, emit16)\n"
    got = expand(tmp_path, src, defines=("CONFIG_SOC_FAMILY_ESPRESSIF_ESP32",
                                         "ESP_SDR_PLACE_SCOPE=link"))
    assert got["S1"].startswith("IRAM(")      # the scope's rule
    assert got["S2"] == ""                     # the scope's pin
    assert expand(tmp_path, src) == {"S1": "", "S2": ""}  # another scope: the SoC's rule


@pytest.mark.parametrize("line, message", [
    ("X: ESP_SDR_DATA(SCRATCH, x)", "no ESP_SDR_PLACE_ rule"),
    ("X: ESP_SDR_DATA(STATE, x)", "needs NOINIT"),
    ("X: ESP_SDR_DATA_F(BULK, x, DMA)", "DMA data outside internal RAM"),
    ("X: ESP_SDR_DATA_F(BULK, x, ATOMIC)", "ATOMIC data in PSRAM"),
    ("X: ESP_SDR_DATA_F2(BULK, x, NOINIT, DMA)", "DMA data outside internal RAM"),
    ("X: ESP_SDR_DATA_F2(STATE, x, DMA, NOINIT)", "needs NOINIT"),
    ("X: ESP_SDR_CODE(SAMPLE, bad_code)", "a data region for code"),
    ("X: ESP_SDR_DATA(TABLE, bad_data)", "a data region for code"),
    ("X: ESP_SDR_BUF_DEFINE(STATE, int, x, [4], 4, NONE)", "needs NOINIT"),
])
def test_clashes_stop_the_build(tmp_path, line, message):
    got = expand(tmp_path, line + "\n")
    assert isinstance(got, str) and message in got


def test_assembler_sections(tmp_path):
    got = expand(tmp_path, """
        A1: ESP_SDR_ASM_SECTION(PACED_SAMPLE, ubt_pack8)
        A2: ESP_SDR_ASM_SECTION(SAMPLE, chancap_pie)
        A3: ESP_SDR_ASM_SECTION(NOCACHE, crash_dx)
        A4: ESP_SDR_ASM_SECTION(SAMPLE, le_feed)
        """, asm=True)
    assert got["A1"] == '.section ".iram1.ubt_pack8", "ax"'
    assert got["A2"] == '.section ".text.chancap_pie", "ax"'
    assert got["A3"] == '.section ".iram1.crash_dx", "ax"'
    assert got["A4"] == '.section ".iram1.le_feed", "ax"'


def test_other_targets_place_nothing(tmp_path):
    got = expand(tmp_path, """
        A: ESP_SDR_CODE(PACED_SAMPLE, ring_loop)
        B: ESP_SDR_DATA_F(STATE, rs, NOINIT)
        C: ESP_SDR_BUF_DEFINE(BULK, int16_t, buf, [64], 16, NONE)
        D: ESP_SDR_STACK_DEFINE(ring_stack, 1536)
        E: ESP_SDR_DATA(SCRATCH, no_rule_needed)
        """, defines=())
    assert got["A"] == got["B"] == got["E"] == ""
    # The source's own includes define __aligned and the stack macro there.
    assert got["C"] == "static int16_t buf [64] __aligned(16)"
    assert got["D"] == "static K_KERNEL_STACK_DEFINE(ring_stack, 1536)"


def test_region_probe_for_tools(tmp_path):
    got = expand(tmp_path, """
        R1: ESP_SDR_REGION_CODE(SAMPLE, le_feed)
        R2: ESP_SDR_REGION_CODE(SAMPLE, demod)
        R3: ESP_SDR_REGION_CODE(NOCACHE, park)
        R4: ESP_SDR_REGION_CODE(COLD, init)
        R5: ESP_SDR_REGION_CODE(EVENT, acquire)
        """)
    assert got == {"R1": "ESP_SDR_IRAM", "R2": "ESP_SDR_FLASH", "R3": "ESP_SDR_IRAM",
                   "R4": "ESP_SDR_FLASH", "R5": "ESP_SDR_HOTBLOCK"}
