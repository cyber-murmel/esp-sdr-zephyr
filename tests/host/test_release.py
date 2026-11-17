# SPDX-License-Identifier: GPL-3.0-or-later
"""nix/release.py: the release assets and the web flasher's list from a nix result tree."""

import json
import zipfile

from conftest import load

release = load("nix/release.py")

SAMPLES = {
    "multiapp": """
sample: {name: multiapp, description: "Runs the
  other apps"}
common: {sysbuild: true}
tests:
  sample.esp_sdr.multiapp:
    platform_allow: [xiao_esp32s3/esp32s3/procpu, ues31s043h800v480c_u/esp32s31/hpcore]
""",
    "osmosdr": """
sample: {name: osmosdr, description: USB SDR}
tests:
  sample.esp_sdr.osmosdr:
    platform_allow: [xiao_esp32s3/esp32s3/procpu]
""",
    "osmosdr_hs": """
sample: {name: osmosdr_hs, description: High speed}
tests:
  esp_sdr.osmosdr_hs:
    platform_allow: [ues31s043h800v480c_u/esp32s31/hpcore]
  esp_sdr.osmosdr_hs.loadlog:
    platform_allow: [ues31s043h800v480c_u/esp32s31/hpcore]
    extra_args: EXTRA_CONF_FILE=loadlog.conf
""",
    "capture": """
sample: {name: capture, description: Bursts}
tests:
  sample.esp_sdr.capture:
    platform_allow: [xiao_esp32s3/esp32s3/procpu, ues31s043h800v480c_u/esp32s31/hpcore]
""",
    "osmosdr_serial": """
sample: {name: osmosdr_serial, description: Serial SDR}
tests:
  sample.esp_sdr.osmosdr_serial:
    platform_allow: [why2025_badge/esp32c6/hpcore]
""",
    "osmosdr_bridge": """
sample: {name: osmosdr_bridge, description: Bridge}
tests:
  esp_sdr.osmosdr_bridge:
    platform_allow: [why2025_badge/esp32p4/hpcore]
""",
    "mch2022_coproc": """
sample: {name: coproc, description: RP2040}
tests:
  esp_sdr.mch2022_coproc:
    platform_allow: [mch2022_rp2040/rp2040]
""",
}


def tree(tmp_path, built, llext):
    """apps/<app>/sample.yaml and result/<board>/<name>/ as collect.py writes them."""
    apps, result = tmp_path / "apps", tmp_path / "result"
    for app, text in SAMPLES.items():
        (apps / app).mkdir(parents=True)
        (apps / app / "sample.yaml").write_text(text)
    for board, names in built.items():
        for name in names:
            d = result / board / name
            d.mkdir(parents=True)
            if board == "mch2022_rp2040":
                (d / "mch2022_coproc.uf2").write_bytes(b"uf2")
            else:
                (d / "merged.bin").write_bytes(name.encode())
                (d / f"{name}.elf").write_bytes(b"\x7fELF")
            if name == "multiapp":
                (d / "llext").mkdir()
                for app in llext.get(board, ()):
                    (d / "llext" / f"{app}.llext").write_bytes(b"")
    (result / "bin").mkdir(parents=True)
    return apps, result


def entries(out, board):
    return {e["name"]: e for e in json.loads((out / f"firmware-{board}.json").read_text())}


def test_multiapp_first_and_its_osmosdr_stays_down(tmp_path, run_main):
    apps, result = tree(tmp_path, {"xiao_esp32s3": ["multiapp", "osmosdr", "capture"]},
                        {"xiao_esp32s3": ["osmosdr", "capture", "hello"]})
    out = tmp_path / "out"
    run_main(release, result, apps, "v1.2.3", out)

    got = entries(out, "xiao_esp32s3")
    assert {n: (e["rank"], e["inMultiapp"]) for n, e in got.items()} == {
        "multiapp": (0, False), "osmosdr": (2, True), "capture": (2, True)}
    listed = json.loads((out / "firmware-xiao_esp32s3.json").read_text())
    assert [e["name"] for e in listed][0] == "multiapp"
    assert got["multiapp"]["description"] == "Runs the other apps"
    assert got["osmosdr"]["file"] == "esp-sdr-osmosdr-v1.2.3-xiao_esp32s3.bin"
    assert (out / got["osmosdr"]["file"]).read_bytes() == b"osmosdr"
    assert got["osmosdr"]["size"] == 7
    assert len(got["osmosdr"]["sha256"]) == 64

    with zipfile.ZipFile(out / "esp-sdr-v1.2.3-xiao_esp32s3.zip") as z:
        names = set(z.namelist())
    assert "esp-sdr-v1.2.3-xiao_esp32s3/multiapp/llext/hello.llext" in names
    assert "esp-sdr-v1.2.3-xiao_esp32s3/osmosdr/osmosdr.elf" in names


def test_osmosdr_second_when_multiapp_lacks_it(tmp_path, run_main):
    apps, result = tree(tmp_path, {"ues31s043h800v480c_u": [
        "multiapp", "osmosdr_hs", "osmosdr_hs-loadlog", "capture"]},
        {"ues31s043h800v480c_u": ["capture"]})
    out = tmp_path / "out"
    run_main(release, result, apps, "v1.2.3", out)

    got = entries(out, "ues31s043h800v480c_u")
    assert {n: e["rank"] for n, e in got.items()} == {
        "multiapp": 0, "osmosdr_hs": 1, "osmosdr_hs-loadlog": 2, "capture": 2}
    assert not got["osmosdr_hs"]["inMultiapp"]
    assert got["capture"]["inMultiapp"]
    listed = json.loads((out / "firmware-ues31s043h800v480c_u.json").read_text())
    assert [e["name"] for e in listed] == ["multiapp", "osmosdr_hs", "capture",
                                           "osmosdr_hs-loadlog"]


def test_two_chips_rank_apart_and_rp2040_is_uf2(tmp_path, run_main):
    apps, result = tree(tmp_path, {"why2025_badge": ["osmosdr_serial", "osmosdr_bridge"],
                                   "mch2022_rp2040": ["mch2022_coproc"]}, {})
    out = tmp_path / "out"
    run_main(release, result, apps, "v1.2.3", out)

    got = entries(out, "why2025_badge")
    assert {n: (e["chip"], e["rank"]) for n, e in got.items()} == {
        "osmosdr_serial": ("esp32c6", 1), "osmosdr_bridge": ("esp32p4", 1)}
    rp = entries(out, "mch2022_rp2040")["mch2022_coproc"]
    assert rp["file"] == "esp-sdr-mch2022_coproc-v1.2.3-mch2022_rp2040.uf2"
    assert rp["chip"] == "rp2040"


def test_an_image_without_a_test_stops(tmp_path, run_main):
    apps, result = tree(tmp_path, {"xiao_esp32s3": ["osmosdr", "stray"]}, {})
    try:
        run_main(release, result, apps, "v1", tmp_path / "out")
    except SystemExit as e:
        assert "stray" in str(e)
    else:
        raise AssertionError("no exit")
