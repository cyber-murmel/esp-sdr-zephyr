# SPDX-License-Identifier: GPL-3.0-or-later
"""doc/registers/svdmerge.py on small made-up SVDs (the real merge needs Espressif's, which
scripts/pre-commit/svdmerge.sh downloads)."""

import hashlib
import xml.etree.ElementTree as ET

import pytest

from conftest import load

svdmerge = load("doc/registers/svdmerge.py")

BASE = """<device><version>1.0</version><description>Theirs.</description><peripherals>
<peripheral><name>MODEM</name><baseAddress>0x60000000</baseAddress>
 <addressBlock><offset>0</offset><size>0x100</size></addressBlock>
 <registers>
  <register><name>CTRL</name><addressOffset>0x0</addressOffset><fields>
   <field><name>EN</name><bitOffset>0</bitOffset><bitWidth>1</bitWidth></field>
   <field><name>MODE</name><description>old</description><bitRange>[7:4]</bitRange></field>
  </fields></register>
  <register><name>STAT</name><addressOffset>0x4</addressOffset></register>
 </registers></peripheral>
</peripherals></device>"""


def ours(body):
    return ET.ElementTree(ET.fromstring(f"<device><peripherals>{body}</peripherals></device>"))


def merged(body):
    base = ET.ElementTree(ET.fromstring(BASE))
    svdmerge.merge("chip", base, ours(body))
    return base.getroot()


def test_bits_and_span():
    f = ET.fromstring("<field><msb>9</msb><lsb>2</lsb></field>")
    assert svdmerge.bits(f) == (9, 2)
    p = ET.fromstring("<p><baseAddress>0x100</baseAddress><addressBlock><offset>0x10</offset>"
                      "<size>0x20</size></addressBlock></p>")
    assert svdmerge.span(p) == (0x110, 0x12F)
    assert svdmerge.span(ET.fromstring("<p><baseAddress>8</baseAddress></p>")) == (8, 8)


def test_fields_registers_and_peripherals():
    root = merged("""
<peripheral><name>MODEM</name><baseAddress>0x60000000</baseAddress><registers>
 <register><name>CTRL</name><addressOffset>0x0</addressOffset><fields>
  <field><name>MODE</name><description>new</description><bitRange>[7:4]</bitRange></field>
  <field><name>EN</name><description>enable</description><lsb>0</lsb><msb>0</msb></field>
  <field><name>GAIN</name><description>ours</description><bitRange>[11:8]</bitRange></field>
 </fields></register>
 <register><name>STAT</name><addressOffset>0x4</addressOffset><fields>
  <field><name>BUSY</name><bitRange>[0:0]</bitRange></field></fields></register>
 <register><name>DBG</name><addressOffset>0x40</addressOffset></register>
</registers></peripheral>
<peripheral><name>RADIO</name><baseAddress>0x60010000</baseAddress>
 <addressBlock><offset>0</offset><size>0x10</size></addressBlock></peripheral>
<peripheral><name>I2C_BBPLL</name><groupName>ANALOG_I2C</groupName>
 <baseAddress>0x0</baseAddress></peripheral>""")
    regs = {r.findtext("name"): r for r in root.iter("register")}
    fields = {f.findtext("name"): f for f in regs["CTRL"].iter("field")}
    assert fields["MODE"].findtext("description") == "new"
    assert fields["EN"].findtext("description") == "enable"
    assert fields["GAIN"].findtext("description") == "ours"
    assert [f.findtext("name") for f in regs["STAT"].iter("field")] == ["BUSY"]
    assert "DBG" in regs
    names = [p.findtext("name") for p in root.iter("peripheral")]
    assert names == ["MODEM", "RADIO"]
    assert root.findtext("version") == "1.0+esp-sdr"
    assert "esp-sdr-zephyr" in root.findtext("description")


@pytest.mark.parametrize("body", [
    # A field of theirs with other bits.
    """<peripheral><name>MODEM</name><baseAddress>0x60000000</baseAddress><registers>
       <register><name>CTRL</name><addressOffset>0</addressOffset><fields>
       <field><name>MODE</name><bitRange>[6:4]</bitRange></field></fields></register>
       </registers></peripheral>""",
    # A new field overlapping one of theirs.
    """<peripheral><name>MODEM</name><baseAddress>0x60000000</baseAddress><registers>
       <register><name>CTRL</name><addressOffset>0</addressOffset><fields>
       <field><name>X</name><bitRange>[5:3]</bitRange></field></fields></register>
       </registers></peripheral>""",
    # A new register at an offset of theirs.
    """<peripheral><name>MODEM</name><baseAddress>0x60000000</baseAddress><registers>
       <register><name>NEW</name><addressOffset>0x4</addressOffset></register>
       </registers></peripheral>""",
    # A new peripheral overlapping theirs.
    """<peripheral><name>OTHER</name><baseAddress>0x60000080</baseAddress>
       <addressBlock><offset>0</offset><size>0x10</size></addressBlock></peripheral>""",
])
def test_conflicts(body):
    with pytest.raises(SystemExit):
        merged(body)


def test_official_checks_the_hash(tmp_path, monkeypatch):
    (tmp_path / "esp32s3.svd").write_text(BASE)
    with pytest.raises(SystemExit):
        svdmerge.official("esp32s3", tmp_path)
    monkeypatch.setitem(svdmerge.SHA256, "esp32s3",
                        hashlib.sha256(BASE.encode()).hexdigest())
    assert svdmerge.official("esp32s3", tmp_path).getroot().findtext("version") == "1.0"
