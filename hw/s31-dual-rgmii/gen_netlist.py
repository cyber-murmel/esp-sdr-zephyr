#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Netlist of the two-module ESP32-S31 RGMII board. Design only, not built or measured.

This file is the single source of truth: the components and the connections are data below. It
writes three files next to itself, byte for byte the same on every run (no dates; the tstamps are
uuid5 of the reference):

- s31-dual-rgmii.net: KiCad netlist, s-expression, format E
- netlist.csv: net, ref, pin, pinfunction
- bom.csv: refs, qty, value, footprint, lcsc, note

Run: python3 gen_netlist.py rewrites them. --check regenerates them in memory and exits 1 if a
file differs, if an electrical check fails, if the written .net does not parse back to the same
components (value, footprint, LCSC, note, DNP) and nets, or if a self-test mutation of the circuit
goes unnoticed. --check --footprints DIR also compares every pad of a KiCad stock footprint (DIR is
the footprints directory of the KiCad library) with the pins the netlist uses; --footprints is an
option of --check only. --selftest runs just the mutations and lists them.
"""

import argparse
import copy
import csv
import io
import re
import sys
import uuid
import xml.etree.ElementTree as ET
from pathlib import Path

HERE = Path(__file__).resolve().parent
NAME = "s31-dual-rgmii"
NOTE = "design only, not built or measured"
NS = uuid.UUID("6d0f3a52-31c7-5b84-9a1e-5331d0a1b0b1")

# ---------------------------------------------------------------------------------------------
# Library parts: (lib, part) -> description and pins {number: (function, electrical type)}
# ---------------------------------------------------------------------------------------------

PASSIVE = "passive"


def two_pin(a="~", b="~"):
    return {"1": (a, PASSIVE), "2": (b, PASSIVE)}


def module_pins():
    """ESP32-S31-WROOM-3 datasheet v0.7, table 3-1 (99 pins)."""
    pins = {}
    for n in (1, 2, 26, 47, 48, *range(72, 91), *range(92, 100)):
        pins[n] = ("GND", "power_in")
    for n in (3, 4):
        pins[n] = ("3V3", "power_in")
    pins[5] = ("EN", "input")
    names = {6: "IO2", 7: "IO3", 8: "IO0", 9: "IO1", 10: "IO4", 11: "IO5", 12: "IO6", 13: "IO7"}
    names.update({6 + k: f"IO{k}" for k in range(8, 20)})        # pins 14..25 are IO8..IO19
    names.update({27 + k: f"SD_D{k}" for k in range(4)})
    names.update({31: "SD_CLK", 32: "SD_CMD", 40: "USB_DP", 41: "USB_DM"})
    names.update({42: "IO33", 43: "IO34", 44: "IO35", 45: "IO36", 46: "IO37"})
    names.update({49: "IO38", 50: "IO39", 51: "IO40"})
    names.update({io + 10: f"IO{io}" for io in range(42, 58)})   # pins 52..67 are IO42..IO57
    names.update({68: "TX0", 69: "RX0", 70: "IO60", 71: "IO61"})
    for n, name in names.items():
        pins[n] = (name, "bidirectional")
    for n in (*range(33, 40), 91):
        pins[n] = ("NC", "no_connect")
    return {str(n): pins[n] for n in sorted(pins)}


PARTS = {
    ("esp-sdr", "ESP32-S31-WROOM-3"): ("ESP32-S31-WROOM-3 module, 99 pins", module_pins()),
    ("Device", "R"): ("Resistor", two_pin()),
    ("Device", "C"): ("Unpolarized capacitor", two_pin()),
    ("Device", "C_Polarized"): ("Polarized capacitor", two_pin("+", "-")),
    ("Diode", "SS34"): ("3 A 40 V Schottky diode, SMA", two_pin("K", "A")),
    ("Switch", "SW_Push"): ("Push button", two_pin("1", "2")),
    ("Regulator_Linear", "AMS1117-3.3"): (
        "1 A low dropout regulator, 3.3 V, SOT-223",
        {"1": ("GND", "power_in"), "2": ("VO", "power_out"), "3": ("VI", "power_in")}),
    ("Connector_Generic", "Conn_01x03"): (
        "Generic connector, single row, 3 pins",
        {str(n): (f"Pin_{n}", PASSIVE) for n in (1, 2, 3)}),
    ("74xGxx", "74LVC1G34"): (
        "Single buffer, SOT-353",
        {"1": ("NC", "no_connect"), "2": ("A", "input"), "3": ("GND", "power_in"),
         "4": ("Y", "output"), "5": ("VCC", "power_in")}),
    ("Connector", "USB_C_Receptacle_USB2.0_16P"): (
        "USB Type-C receptacle, USB 2.0, 16 pins",
        {"A1": ("GND", "passive"), "B12": ("GND", "passive"), "A12": ("GND", "passive"),
         "B1": ("GND", "passive"), "A4": ("VBUS", "passive"), "B9": ("VBUS", "passive"),
         "A9": ("VBUS", "passive"), "B4": ("VBUS", "passive"), "A5": ("CC1", "passive"),
         "B5": ("CC2", "passive"), "A6": ("D+", "passive"), "B6": ("D+", "passive"),
         "A7": ("D-", "passive"), "B7": ("D-", "passive"), "A8": ("SBU1", "passive"),
         "B8": ("SBU2", "passive"), "S1": ("SHIELD", "passive")}),
}

MODULE = ("esp-sdr", "ESP32-S31-WROOM-3")
FP_MODULE = "esp-sdr:ESP32-S31-WROOM-3"
FP_R = "Resistor_SMD:R_0402_1005Metric"
FP_C = "Capacitor_SMD:C_0402_1005Metric"
FP_C0805 = "Capacitor_SMD:C_0805_2012Metric"
FP_CTANT = "Capacitor_Tantalum_SMD:CP_EIA-3528-21_Kemet-B"
FP_D = "Diode_SMD:D_SMA"
FP_SW = "Button_Switch_SMD:SW_SPST_TL3342"
FP_LDO = "Package_TO_SOT_SMD:SOT-223-3_TabPin2"
FP_BUF = "Package_TO_SOT_SMD:SOT-353_SC-70-5"
FP_USB = "Connector_USB:USB_C_Receptacle_GCT_USB4105-xx-A_16P_TopMnt_Horizontal"
FP_HDR3 = "Connector_PinHeader_2.54mm:PinHeader_1x03_P2.54mm_Vertical"

# ---------------------------------------------------------------------------------------------
# RGMII pad set 0 (IO_MUX, fixed): signal -> (module pin, GPIO)
# ---------------------------------------------------------------------------------------------

RGMII = {
    "TXD0": (14, 8), "TXD1": (15, 9), "TXD2": (16, 10), "TXD3": (17, 11), "TX_CTL": (18, 12),
    "TXC": (19, 13), "RXC": (20, 14), "RX_CTL": (21, 15),
    "RXD3": (22, 16), "RXD2": (23, 17), "RXD1": (24, 18), "RXD0": (25, 19),
}
# The five data lines of a direction: (driver signal, receiver signal, net suffix)
LANES = [("TXD0", "RXD0", "D0"), ("TXD1", "RXD1", "D1"), ("TXD2", "RXD2", "D2"),
         ("TXD3", "RXD3", "D3"), ("TX_CTL", "RX_CTL", "CTL")]

# ---------------------------------------------------------------------------------------------
# The circuit. Sides: A has refs in the 100s, B in the 200s, the link parts are in the 300s.
# ---------------------------------------------------------------------------------------------

COMPONENTS = {}   # ref -> dict(part, value, footprint, lcsc, note, dnp)
NETS = {}         # net name -> ["ref.pin", ...]
NC = {}           # ref -> pins deliberately left open
SIDES = {"A": 1, "B": 2}


def comp(ref, part, value, footprint, lcsc="", note="", dnp=False):
    assert ref not in COMPONENTS, ref
    COMPONENTS[ref] = dict(part=part, value=value, footprint=footprint, lcsc=lcsc, note=note,
                           dnp=dnp)


def tie(net, *nodes):
    NETS.setdefault(net, []).extend(nodes)


def build_side(s):
    x = SIDES[s]
    def ref(prefix, k):
        return f"{prefix}{x}{k:02d}"

    u, ldo, usb, uart, pps = ref("U", 1), ref("U", 2), ref("J", 1), ref("J", 2), ref("J", 3)
    cc1, cc2, enr, bootr = ref("R", 1), ref("R", 2), ref("R", 3), ref("R", 4)
    cin, cbulk, cd1, cd2, cen = (ref("C", k) for k in (1, 2, 3, 4, 5))
    diode, rst, boot = ref("D", 1), ref("SW", 1), ref("SW", 2)

    comp(u, MODULE, "ESP32-S31-WROOM-3", FP_MODULE,
         note="custom footprint: draw it from the datasheet land pattern")
    comp(ldo, ("Regulator_Linear", "AMS1117-3.3"), "AMS1117-3.3", FP_LDO,
         note="1 A; dropout 1.1 to 1.3 V is tight from USB 5 V; needs an output capacitor with "
         "ESR (tantalum or polymer)")
    comp(usb, ("Connector", "USB_C_Receptacle_USB2.0_16P"), "USB-C receptacle 16P", FP_USB,
         note="power and USB-Serial-JTAG; pads as in the KiCad footprint")
    comp(uart, ("Connector_Generic", "Conn_01x03"), "UART0 TX RX GND", FP_HDR3)
    comp(pps, ("Connector_Generic", "Conn_01x03"), "IO2 (PPS) IO3 GND", FP_HDR3)
    comp(cc1, ("Device", "R"), "5.1k", FP_R, note="CC1 pull-down (USB sink)")
    comp(cc2, ("Device", "R"), "5.1k", FP_R, note="CC2 pull-down (USB sink)")
    comp(enr, ("Device", "R"), "10k", FP_R, note="EN pull-up")
    comp(bootr, ("Device", "R"), "10k", FP_R, note="IO61 (BOOT) pull-up")
    comp(cin, ("Device", "C"), "10u", FP_C0805, note="+5V input of the regulator")
    comp(cbulk, ("Device", "C_Polarized"), "22u", FP_CTANT,
         note="3V3 bulk next to the module; tantalum or polymer for the AMS1117")
    comp(cd1, ("Device", "C"), "100n", FP_C, note="3V3 decoupling at a module 3V3 pin")
    comp(cd2, ("Device", "C"), "100n", FP_C, note="3V3 decoupling at a module 3V3 pin")
    comp(cen, ("Device", "C"), "1u", FP_C, note="EN delay (10k and 1u)")
    comp(diode, ("Diode", "SS34"), "SS34", FP_D,
         note="3 A; VBUS to the common +5V: no back-powering through the RGMII pads")
    comp(rst, ("Switch", "SW_Push"), "Reset", FP_SW, note="EN to GND")
    comp(boot, ("Switch", "SW_Push"), "Boot", FP_SW, note="IO61 to GND: download mode at reset")

    v33, vbus, en, bt = f"+3V3_{s}", f"{s}_VBUS", f"{s}_EN", f"{s}_BOOT"
    # power
    tie("GND", *(f"{u}.{p}" for p in (1, 2, 26, 47, 48, *range(72, 91), *range(92, 100))))
    tie(v33, f"{u}.3", f"{u}.4", f"{ldo}.2", f"{cbulk}.1", f"{cd1}.1", f"{cd2}.1", f"{enr}.1",
        f"{bootr}.1")
    tie("GND", f"{ldo}.1", f"{cin}.2", f"{cbulk}.2", f"{cd1}.2", f"{cd2}.2", f"{cen}.2",
        f"{rst}.2", f"{boot}.2", f"{cc1}.2", f"{cc2}.2", f"{uart}.3", f"{pps}.3")
    tie("+5V", f"{diode}.1", f"{ldo}.3", f"{cin}.1")
    tie(vbus, f"{diode}.2", *(f"{usb}.{p}" for p in ("A4", "B9", "A9", "B4")))
    tie("GND", *(f"{usb}.{p}" for p in ("A1", "B12", "A12", "B1", "S1")))
    # USB-Serial-JTAG: IO33 is D-, IO34 is D+; CC pull-downs
    tie(f"{s}_USB_DP", f"{usb}.A6", f"{usb}.B6", f"{u}.43")
    tie(f"{s}_USB_DN", f"{usb}.A7", f"{usb}.B7", f"{u}.42")
    tie(f"{s}_CC1", f"{usb}.A5", f"{cc1}.1")
    tie(f"{s}_CC2", f"{usb}.B5", f"{cc2}.1")
    # EN and BOOT
    tie(en, f"{u}.5", f"{enr}.2", f"{cen}.1", f"{rst}.1")
    tie(bt, f"{u}.71", f"{bootr}.2", f"{boot}.1")
    # headers
    tie(f"{s}_UART_TX", f"{u}.68", f"{uart}.1")
    tie(f"{s}_UART_RX", f"{u}.69", f"{uart}.2")
    tie(f"{s}_PPS", f"{u}.6", f"{pps}.1")
    tie(f"{s}_IO3", f"{u}.7", f"{pps}.2")

    NC[usb] = ["A8", "B8"]
    # everything the module does not use: unused IOs, USB HS OTG pair, the datasheet's NC pads
    used = {1, 2, 3, 4, 5, 6, 7, 26, 42, 43, 47, 48, 68, 69, 71, *range(14, 26), *range(72, 91),
            *range(92, 100)}
    NC[u] = [str(n) for n in range(1, 100) if n not in used]


def build_link(tx, rx, base):
    """One direction, tx side drives: five data lines, the clock and its delay options."""
    xt, xr = SIDES[tx], SIDES[rx]
    utx, urx = f"U{xt}01", f"U{xr}01"
    pair = tx + rx
    for i, (drv, rcv, sfx) in enumerate(LANES):
        r = f"R{base + 1 + i}"
        comp(r, ("Device", "R"), "22R", FP_R, note="series at the driver (RGMII data line)")
        tie(f"{tx}_{drv.replace('_', '')}", f"{utx}.{RGMII[drv][0]}", f"{r}.1")
        tie(f"{pair}_{sfx}", f"{r}.2", f"{urx}.{RGMII[rcv][0]}")
    rs, rby, rout, ub, cb = (f"R{base + 6}", f"R{base + 7}", f"R{base + 8}", f"U{base + 1}",
                             f"C{base + 1}")
    comp(rs, ("Device", "R"), "22R", FP_R, note="series at the driver (TXC)")
    comp(rby, ("Device", "R"), "0R", FP_R, dnp=True,
         note="option 1, clock bypass (no delay): fit it and leave the buffer and its 22R off")
    comp(rout, ("Device", "R"), "22R", FP_R, note="series at the delay buffer output (TXC)")
    comp(ub, ("74xGxx", "74LVC1G34"), "74LVC1G34", FP_BUF, lcsc="C52140426",
         note="option 2 (default), delay buffer near the receiver; the LCSC part is an MDD clone "
         "with no timing data, prefer Nexperia or TI; 0.5/2.0/4.1 ns (min/typ/max)")
    comp(cb, ("Device", "C"), "100n", FP_C,
         note="supply of the delay buffer, from the receiving side's 3V3")
    tie(f"{tx}_TXC", f"{utx}.{RGMII['TXC'][0]}", f"{rs}.1")
    tie(f"{pair}_CLK_SRC", f"{rs}.2", f"{rby}.1", f"{ub}.2")
    tie(f"{pair}_CLK_BUF", f"{ub}.4", f"{rout}.1")
    tie(f"{pair}_CLK_RX", f"{rout}.2", f"{rby}.2", f"{urx}.{RGMII['RXC'][0]}")
    tie(f"+3V3_{rx}", f"{ub}.5", f"{cb}.1")
    tie("GND", f"{ub}.3", f"{cb}.2")
    NC[ub] = ["1"]


for side in SIDES:
    build_side(side)
build_link("A", "B", 300)
build_link("B", "A", 310)

# ---------------------------------------------------------------------------------------------
# The model: nets with unconnected pins added, as KiCad's exporter does
# ---------------------------------------------------------------------------------------------


def natural(text):
    return [(0, int(t), "") if t.isdigit() else (1, 0, t) for t in re.findall(r"\d+|\D+", text)]


def pin_info(ref, pin):
    """(function, type) of a pin; a pin the part lacks shows up as an error in the checks."""
    return PARTS[COMPONENTS[ref]["part"]][1].get(pin, ("?", "unknown"))


def model():
    """{net: [(ref, pin)]} including the 'unconnected-(...)' nets."""
    nets = {name: [tuple(n.split(".")) for n in nodes] for name, nodes in NETS.items()}
    for ref, pins in NC.items():
        for pin in pins:
            fn = pin_info(ref, pin)[0]
            nets[f"unconnected-({ref}-{fn}-Pad{pin})"] = [(ref, pin)]
    return {name: sorted(nodes, key=lambda n: (natural(n[0]), natural(n[1])))
            for name, nodes in sorted(nets.items())}


# ---------------------------------------------------------------------------------------------
# Electrical checks
# ---------------------------------------------------------------------------------------------


def check_model(nets):
    errors = []
    pin_net = {}
    for name, nodes in nets.items():
        for ref, pin in nodes:
            if ref not in COMPONENTS:
                errors.append(f"{name}: unknown component {ref}")
                continue
            if pin not in PARTS[COMPONENTS[ref]["part"]][1]:
                errors.append(f"{name}: {ref} has no pin {pin}")
            if (ref, pin) in pin_net:
                errors.append(f"{ref} pin {pin} is in two nets: {pin_net[(ref, pin)]}, {name}")
            pin_net[(ref, pin)] = name
        if name.startswith("unconnected-"):
            if len(nodes) != 1:
                errors.append(f"{name}: an unconnected net holds {len(nodes)} nodes")
        elif len(nodes) < 2:
            errors.append(f"{name}: net with {len(nodes)} node")
        if sum(pin_info(r, p)[1] == "power_out" for r, p in nodes) > 1:
            errors.append(f"{name}: two power outputs")
    for ref, c in COMPONENTS.items():
        pins = set(PARTS[c["part"]][1])
        nc = set(NC.get(ref, []))
        wired = {p for (r, p), n in pin_net.items()
                 if r == ref and not n.startswith("unconnected-")}
        for p in sorted(nc & wired, key=natural):
            errors.append(f"{ref} pin {p} is connected and marked no-connect")
        for p in sorted(pins - nc - wired, key=natural):
            errors.append(f"{ref} pin {p} is neither connected nor marked no-connect")
        for p in sorted(nc - pins, key=natural):
            errors.append(f"{ref}: no-connect list names pin {p}, not a pin of the part")
    mod_pins = {str(n) for n in range(1, 100)}
    for ref, c in COMPONENTS.items():
        if c["part"] == MODULE and set(PARTS[MODULE][1]) != mod_pins:
            errors.append(f"{ref}: the module part does not have the pins 1 to 99")
    errors += check_links(nets, pin_net)
    errors += check_circuit(nets, pin_net)
    return errors


def check_links(nets, pin_net):
    """Each RGMII line leads from a driver pad over one series resistor to the receiver pad."""
    errors = []
    for tx, rx in (("A", "B"), ("B", "A")):
        utx, urx = f"U{SIDES[tx]}01", f"U{SIDES[rx]}01"
        for drv, rcv, _ in LANES:
            net = pin_net.get((utx, str(RGMII[drv][0])))
            rs = [n for n in nets.get(net, []) if n[0].startswith("R")]
            if len(rs) != 1:
                errors.append(f"{utx} {drv}: expected one series resistor, got {rs}")
                continue
            other = pin_net.get((rs[0][0], "2" if rs[0][1] == "1" else "1"))
            if (urx, str(RGMII[rcv][0])) not in nets.get(other, []):
                errors.append(f"{utx} {drv} does not lead to {urx} {rcv}")
        # the clock: driver pad, 22R, source net (bypass and buffer input), receiver's RXC pad
        src = pin_net.get((utx, str(RGMII["TXC"][0])))
        if src != f"{tx}_TXC" or not any(r.startswith("R") for r, _ in nets.get(src, [])):
            errors.append(f"{utx} TXC: expected the net {tx}_TXC with a series resistor")
        rxc = pin_net.get((urx, str(RGMII["RXC"][0])))
        if rxc != f"{tx}{rx}_CLK_RX":
            errors.append(f"{urx} RXC is on {rxc}, not on {tx}{rx}_CLK_RX")
    return errors


SUPPLY_RAILS = ("+5V", "+3V3_A", "+3V3_B")


def check_circuit(nets, pin_net):
    """The clock path, the supply and the diodes are what the README says."""
    errors = []

    def expect(ref, pin, want, what):
        got = pin_net.get((ref, str(pin)))
        if got != want:
            errors.append(f"{what}: {ref} pin {pin} is on {got}, expected {want}")

    def only(net, allowed, what):
        extra = sorted(set(nets.get(net, [])) - set(allowed), key=lambda n: natural(n[0]))
        if extra:
            errors.append(f"{what}: {net} also holds {extra}")

    for tx, rx, base in (("A", "B", 300), ("B", "A", 310)):
        urx = f"U{SIDES[rx]}01"
        pair = tx + rx
        src, buf, rxnet = f"{pair}_CLK_SRC", f"{pair}_CLK_BUF", f"{pair}_CLK_RX"
        rs, rby, rout, ub, cb = (f"R{base + 6}", f"R{base + 7}", f"R{base + 8}", f"U{base + 1}",
                                 f"C{base + 1}")
        rxc = str(RGMII["RXC"][0])
        # source -> (buffer and its resistor | bypass) -> receiver
        expect(rs, 1, f"{tx}_TXC", "clock series resistor")
        expect(rs, 2, src, "clock series resistor")
        expect(ub, 2, src, "buffer input (pin 2 A)")
        expect(ub, 4, buf, "buffer output (pin 4 Y)")
        expect(rout, 1, buf, "buffer output resistor")
        expect(rout, 2, rxnet, "buffer output resistor")
        expect(rby, 1, src, "bypass")
        expect(rby, 2, rxnet, "bypass")
        expect(urx, rxc, rxnet, "RXC")
        only(src, [(rs, "2"), (rby, "1"), (ub, "2")], "clock source net")
        only(buf, [(ub, "4"), (rout, "1")], "buffer output net")
        only(rxnet, [(rby, "2"), (rout, "2"), (urx, rxc)], "receiver clock net")
        # the buffer runs from the receiving side's 3V3
        expect(ub, 5, f"+3V3_{rx}", "buffer VCC (pin 5)")
        expect(ub, 3, "GND", "buffer GND (pin 3)")
        expect(cb, 1, f"+3V3_{rx}", "buffer capacitor")
        expect(cb, 2, "GND", "buffer capacitor")
        # default assembly: buffer path fitted, bypass not fitted
        if COMPONENTS[ub]["part"] != ("74xGxx", "74LVC1G34") or COMPONENTS[ub]["dnp"] \
                or COMPONENTS[rout]["dnp"] or not COMPONENTS[rby]["dnp"]:
            errors.append(f"{pair}: default assembly is the buffer fitted and the bypass DNP")
        if (COMPONENTS[rby]["value"], COMPONENTS[rs]["value"], COMPONENTS[rout]["value"]) \
                != ("0R", "22R", "22R"):
            errors.append(f"{pair}: bypass 0R, series resistors 22R expected")
    # supply: each module from its own regulator, diodes from VBUS to the common rail
    for side, x in SIDES.items():
        u, ldo, d, usb = f"U{x}01", f"U{x}02", f"D{x}01", f"J{x}01"
        expect(d, 2, f"{side}_VBUS", "diode anode (pin 2 A)")
        expect(d, 1, "+5V", "diode cathode (pin 1 K)")
        for p in ("A4", "B9", "A9", "B4"):
            expect(usb, p, f"{side}_VBUS", "USB-C VBUS")
        expect(ldo, 3, "+5V", "regulator input")
        expect(ldo, 2, f"+3V3_{side}", "regulator output")
        expect(ldo, 1, "GND", "regulator GND")
        expect(u, 3, f"+3V3_{side}", "module 3V3")
        expect(u, 4, f"+3V3_{side}", "module 3V3")
    for ref, c in COMPONENTS.items():
        pins = PARTS[c["part"]][1]
        if len(pins) == 2:
            nets2 = [pin_net.get((ref, p)) for p in sorted(pins)]
            if all(n in SUPPLY_RAILS for n in nets2) and nets2[0] != nets2[1]:
                errors.append(f"{ref} joins the supply rails {nets2[0]} and {nets2[1]}")
    return errors


def move(node, to):
    """Self-test helper: put a node (ref.pin) on another net."""
    for nodes in NETS.values():
        if node in nodes:
            nodes.remove(node)
    NETS.setdefault(to, []).append(node)


def mutate_add_bridge():
    comp("R999", ("Device", "R"), "0R", FP_R)
    tie("+3V3_A", "R999.1")
    tie("+3V3_B", "R999.2")


def mutate_dnp(ref, value):
    COMPONENTS[ref]["dnp"] = value


# (what is broken, a function that breaks it, a text the checks must report)
MUTATIONS = [
    ("RX data lines swapped", lambda: (move("U201.25", "AB_D1"), move("U201.24", "AB_D0")),
     "does not lead to"),
    ("a pin in two nets", lambda: NETS["A_EN"].append("U102.1"), "is in two nets"),
    ("a net with one node", lambda: NETS.update(LONELY=["R101.1"]), "net with 1 node"),
    ("a pin neither connected nor no-connect", lambda: NC["U101"].remove("8"),
     "neither connected nor marked"),
    ("buffer reversed (A and Y swapped)",
     lambda: (move("U301.2", "AB_CLK_BUF"), move("U301.4", "AB_CLK_SRC")), "buffer input"),
    ("buffer VCC from the sending side's 3V3", lambda: move("U301.5", "+3V3_A"),
     "buffer VCC"),
    ("buffer capacitor on the sending side's 3V3", lambda: move("C311.1", "+3V3_B"),
     "buffer capacitor"),
    ("buffer output past its resistor", lambda: move("U301.4", "AB_CLK_RX"), "buffer output"),
    ("bypass not between source and receiver", lambda: move("R307.2", "AB_CLK_BUF"), "bypass"),
    ("bypass fitted by default", lambda: mutate_dnp("R307", False), "default assembly"),
    ("buffer not fitted by default", lambda: mutate_dnp("U311", True), "default assembly"),
    ("TXC without its series resistor", lambda: move("U101.19", "AB_CLK_SRC"), "U101 TXC"),
    ("RXC on the source net", lambda: move("U201.20", "AB_CLK_SRC"), "RXC"),
    ("diode reversed", lambda: (move("D101.1", "A_VBUS"), move("D101.2", "+5V")),
     "diode anode"),
    ("module 3V3 on the other side's rail", lambda: move("U201.3", "+3V3_A"), "module 3V3"),
    ("regulator input from VBUS, not the rail", lambda: move("U102.3", "A_VBUS"),
     "regulator input"),
    ("a two-pin part between +3V3_A and +3V3_B", mutate_add_bridge, "joins the supply rails"),
]


# The same for the written .net: (what is changed in its text, old text, new text)
TEXT_MUTATIONS = [
    ("a Note changed in the .net", "EN pull-up", "EN pull-down"),
    ("the DNP field dropped from the .net", '(field (name "DNP") "yes")', ""),
    ("a pin moved to another net in the .net", '(node (ref "R301") (pin "2")',
     '(node (ref "R301") (pin "1")'),
]


def selftest(verbose=False):
    """Break the circuit in each way of MUTATIONS: the checks must report it."""
    errors = []
    saved = copy.deepcopy((COMPONENTS, NETS, NC))
    assert check_model(model()) == [], "the unbroken circuit fails its own checks"
    for what, mutate, text in MUTATIONS:
        try:
            mutate()
            found = check_model(model())
        finally:
            for target, original in zip((COMPONENTS, NETS, NC), copy.deepcopy(saved)):
                target.clear()
                target.update(original)
        hits = [e for e in found if text in e]
        if not hits:
            errors.append(f"self-test: not caught: {what} (expected '{text}', got {found[:2]})")
        elif verbose:
            print(f"caught: {what}: {hits[0]}")
    nets, text = model(), netlist_text(model())
    for what, old, new in TEXT_MUTATIONS:
        found = check_roundtrip(text.replace(old, new, 1), nets) if old in text else []
        if not found:
            errors.append(f"self-test: not caught: {what}")
        elif verbose:
            print(f"caught: {what}: {found[0]}")
    return errors


# ---------------------------------------------------------------------------------------------
# Writers
# ---------------------------------------------------------------------------------------------


class Sym(str):
    """An unquoted atom."""


def quote(text):
    return '"' + text.replace("\\", "\\\\").replace('"', '\\"').replace("\n", "\\n") + '"'


def sexp(node, indent=0):
    """Serialise nested lists: strings are quoted, Sym is not. Short flat lists stay on a line."""
    if not isinstance(node, list):
        return quote(node) if not isinstance(node, Sym) else str(node)
    flat = "(" + " ".join(sexp(c) for c in node) + ")"
    simple = all(not isinstance(c, list) or all(not isinstance(g, list) for g in c) for c in node)
    if simple and len(flat) + indent <= 100 and "\n" not in flat:
        return flat
    pad = " " * (indent + 2)
    return "(" + sexp(node[0]) + "".join("\n" + pad + sexp(c, indent + 2) for c in node[1:]) + ")"


def tstamp(ref):
    return str(uuid.uuid5(NS, ref))


def netlist_text(nets):
    comps = [Sym("components")]
    for ref in sorted(COMPONENTS, key=natural):
        c = COMPONENTS[ref]
        lib, part = c["part"]
        fields = [Sym("fields"), [Sym("field"), [Sym("name"), "LCSC"], c["lcsc"]]]
        if c["note"]:
            fields.append([Sym("field"), [Sym("name"), "Note"], c["note"]])
        if c["dnp"]:
            fields.append([Sym("field"), [Sym("name"), "DNP"], "yes"])
        comps.append([Sym("comp"), [Sym("ref"), ref], [Sym("value"), c["value"]],
                      [Sym("footprint"), c["footprint"]], fields,
                      [Sym("libsource"), [Sym("lib"), lib], [Sym("part"), part],
                       [Sym("description"), PARTS[c["part"]][0]]],
                      [Sym("sheetpath"), [Sym("names"), "/"], [Sym("tstamps"), "/"]],
                      [Sym("tstamps"), tstamp(ref)]])
    libparts = [Sym("libparts")]
    for lib, part in sorted({c["part"] for c in COMPONENTS.values()}):
        desc, pins = PARTS[(lib, part)]
        libparts.append([Sym("libpart"), [Sym("lib"), lib], [Sym("part"), part],
                         [Sym("description"), desc],
                         [Sym("pins")] + [[Sym("pin"), [Sym("num"), n], [Sym("name"), f],
                                           [Sym("type"), t]]
                                          for n, (f, t) in sorted(pins.items(),
                                                                  key=lambda i: natural(i[0]))]])
    libraries = [Sym("libraries")] + [
        [Sym("library"), [Sym("logical"), lib],
         [Sym("uri"), "${KICAD8_SYMBOL_DIR}/" + lib + ".kicad_sym"]]
        for lib in sorted({c["part"][0] for c in COMPONENTS.values()})]
    netsx = [Sym("nets")]
    for code, (name, nodes) in enumerate(nets.items(), 1):
        entry = [Sym("net"), [Sym("code"), str(code)], [Sym("name"), name]]
        for ref, pin in nodes:
            fn, typ = pin_info(ref, pin)
            node = [Sym("node"), [Sym("ref"), ref], [Sym("pin"), pin]]
            if fn != "~":
                node.append([Sym("pinfunction"), fn])
            node.append([Sym("pintype"), typ])
            entry.append(node)
        netsx.append(entry)
    design = [Sym("design"), [Sym("source"), f"{NAME}.kicad_sch"], [Sym("date"), ""],
              [Sym("tool"), "gen_netlist.py"],
              [Sym("sheet"), [Sym("number"), "1"], [Sym("name"), "/"], [Sym("tstamps"), "/"],
               [Sym("title_block"), [Sym("title"), "ESP32-S31 dual-module RGMII link"],
                [Sym("company"), ""], [Sym("rev"), "0"], [Sym("date"), ""],
                [Sym("source"), f"{NAME}.kicad_sch"],
                [Sym("comment"), [Sym("number"), "1"], [Sym("value"), NOTE]]]]]
    export = [Sym("export"), [Sym("version"), "E"], design, comps, libparts, libraries, netsx]
    return sexp(export) + "\n"


def netlist_csv(nets):
    out = io.StringIO()
    w = csv.writer(out, lineterminator="\n")
    w.writerow(["net", "ref", "pin", "pinfunction"])
    for name, nodes in nets.items():
        for ref, pin in nodes:
            fn = pin_info(ref, pin)[0]
            w.writerow([name, ref, pin, "" if fn == "~" else fn])
    return out.getvalue()


def bom_csv():
    groups = {}
    for ref in sorted(COMPONENTS, key=natural):
        c = COMPONENTS[ref]
        group = groups.setdefault((c["value"], c["footprint"], c["lcsc"], c["dnp"]), ([], []))
        group[0].append(ref)
        if c["note"] and c["note"] not in group[1]:
            group[1].append(c["note"])
    out = io.StringIO()
    w = csv.writer(out, lineterminator="\n")
    w.writerow(["refs", "qty", "value", "footprint", "lcsc", "note"])
    for (value, fp, lcsc, dnp), (refs, notes) in sorted(groups.items(),
                                                        key=lambda g: natural(g[1][0][0])):
        w.writerow([" ".join(refs), len(refs), value, fp, lcsc,
                    "; ".join((["DNP by default"] if dnp else []) + notes)])
    return out.getvalue()


def outputs():
    nets = model()
    errors = check_model(nets)
    return nets, errors, {f"{NAME}.net": netlist_text(nets), "netlist.csv": netlist_csv(nets),
                          "bom.csv": bom_csv()}


# ---------------------------------------------------------------------------------------------
# Reading back
# ---------------------------------------------------------------------------------------------

TOKEN = re.compile(r'\s*(?:(\()|(\))|"((?:[^"\\]|\\.)*)"|([^\s()"]+))')


def parse_sexp(text):
    """A small s-expression parser: lists are Python lists, atoms and strings are str."""
    pos, stack, top = 0, [], []
    while pos < len(text):
        m = TOKEN.match(text, pos)
        if m is None:
            if text[pos:].strip():
                raise ValueError(f"cannot parse at offset {pos}: {text[pos:pos + 20]!r}")
            break
        pos = m.end()
        if m.group(1):
            stack.append(top)
            top = []
        elif m.group(2):
            if not stack:
                raise ValueError("unbalanced )")
            done, top = top, stack.pop()
            top.append(done)
        elif m.group(3) is not None:
            top.append(re.sub(r"\\(.)", lambda e: "\n" if e.group(1) == "n" else e.group(1),
                              m.group(3)))
        else:
            top.append(m.group(4))
    if stack:
        raise ValueError("unbalanced (")
    return top


def child(node, head):
    return [c for c in node if isinstance(c, list) and c and c[0] == head]


def check_roundtrip(text, nets):
    errors = []
    try:
        (root,) = parse_sexp(text)
    except ValueError as e:
        return [f"the .net does not parse: {e}"]
    comps = {}
    for c in child(child(root, "components")[0], "comp"):
        ref = child(c, "ref")[0][1]
        fields = {f[1][1]: f[2] for f in child(child(c, "fields")[0], "field")}
        comps[ref] = (child(c, "value")[0][1], child(c, "footprint")[0][1], fields["LCSC"],
                      fields.get("Note", ""), fields.get("DNP") == "yes")
    want = {r: (c["value"], c["footprint"], c["lcsc"], c["note"], c["dnp"])
            for r, c in COMPONENTS.items()}
    if comps != want:
        errors.append("components of the .net differ from the model")
    got = {}
    for n in child(child(root, "nets")[0], "net"):
        got[child(n, "name")[0][1]] = [(child(k, "ref")[0][1], child(k, "pin")[0][1])
                                       for k in child(n, "node")]
    if got != nets:
        errors.append("nets of the .net differ from the model")
    return errors


def check_svgs(nets):
    errors = []
    for name in ("block-diagram.svg", "wiring-diagram.svg"):
        path = HERE / name
        if not path.is_file():
            errors.append(f"{name} is missing")
            continue
        try:
            root = ET.parse(path).getroot()
        except ET.ParseError as e:
            errors.append(f"{name} is not well-formed XML: {e}")
            continue
        if name == "wiring-diagram.svg":
            text = " ".join(root.itertext())
            for net in nets:
                if re.fullmatch(r"(AB|BA)_(D[0-3]|CTL|CLK_SRC|CLK_BUF|CLK_RX)", net) \
                        and net not in text:
                    errors.append(f"{name} does not show net {net}")
            for ref in COMPONENTS:
                if re.fullmatch(r"[RUC]3\d\d", ref) and ref not in text:
                    errors.append(f"{name} does not show {ref}")
    return errors


def check_footprints(directory):
    """Every pin used on a KiCad stock footprint must be one of its pads."""
    errors = []
    for fp in sorted({c["footprint"] for c in COMPONENTS.values()} - {FP_MODULE}):
        lib, name = fp.split(":")
        path = Path(directory) / f"{lib}.pretty" / f"{name}.kicad_mod"
        if not path.is_file():
            errors.append(f"{fp}: not found under {directory}")
            continue
        pads = set(re.findall(r'\(pad "?([^"\s)]+)"?', path.read_text()))
        for ref, c in COMPONENTS.items():
            if c["footprint"] == fp:
                missing = set(PARTS[c["part"]][1]) - pads
                if missing:
                    errors.append(f"{ref}: pins {sorted(missing, key=natural)} are not pads "
                                  f"of {fp}")
    return errors


def main(argv):
    ap = argparse.ArgumentParser(description="Write or check the netlist of the dual-module board.")
    ap.add_argument("--check", action="store_true",
                    help="compare the files with what the script writes, run all checks")
    ap.add_argument("--footprints", metavar="DIR",
                    help="with --check: compare pins with the pads of the KiCad footprints in DIR")
    ap.add_argument("--selftest", action="store_true",
                    help="break the circuit in each way of MUTATIONS and list what is caught")
    args = ap.parse_args(argv)
    if args.footprints and not args.check:
        ap.error("--footprints is an option of --check")
    if args.selftest:
        errors = selftest(verbose=True)
        if errors:
            sys.exit("\n".join(errors))
        print(f"ok: {len(MUTATIONS) + len(TEXT_MUTATIONS)} mutations caught")
        return 0
    nets, errors, files = outputs()
    if args.check:
        for name, text in files.items():
            path = HERE / name
            if not path.is_file():
                errors.append(f"{name} is missing: run gen_netlist.py")
            elif path.read_text() != text:
                errors.append(f"{name} is out of date: run gen_netlist.py")
        if (HERE / f"{NAME}.net").is_file():
            errors += check_roundtrip((HERE / f"{NAME}.net").read_text(), nets)
        errors += check_svgs(nets)
        if args.footprints:
            errors += check_footprints(args.footprints)
        errors += selftest()
        if errors:
            sys.exit("\n".join(["gen_netlist.py --check failed:", *errors]))
        n_nodes = sum(len(v) for k, v in nets.items() if not k.startswith("unconnected-"))
        print(f"ok: {len(COMPONENTS)} components, "
              f"{sum(not k.startswith('unconnected-') for k in nets)} nets, {n_nodes} nodes, "
              f"{sum(k.startswith('unconnected-') for k in nets)} unconnected pins, "
              f"{len(MUTATIONS) + len(TEXT_MUTATIONS)} mutations caught")
        return 0
    if errors:
        sys.exit("\n".join(["electrical checks failed:", *errors]))
    for name, text in files.items():
        (HERE / name).write_text(text)
        print(f"wrote {name}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
