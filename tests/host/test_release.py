# SPDX-License-Identifier: GPL-3.0-or-later
"""nix/release.py: the release assets and the web flasher's list from a nix result tree."""

import json
import zipfile

import pytest

from conftest import REPO, load

release = load("nix/release.py")
images = load("nix/images.py")

# apps/common/sysbuild/Kconfig.signing as far as images.py reads it, and the Kconfig.sysbuild of
# an app that opts into it.
SIGNING = """# comment
config ESDR_SIGNING
\tbool "Sign with the project's own ED25519 key"
\tdefault y if BOARD = "esp32s31_function_coreboard" || BOARD = "other_board"
\thelp
\t  default y if BOARD = "not_this_one": help text, which is not a default

\tbool

choice BOOT_SIGNATURE_TYPE
\tdefault BOOT_SIGNATURE_TYPE_ED25519 if ESDR_SIGNING
endchoice
"""
OPTS_IN = """# The own signing key defaults.
rsource "../common/sysbuild/Kconfig.signing"

source "share/sysbuild/Kconfig"
"""

SAMPLES = {
    "multiapp": """
sample: {name: multiapp, description: "Runs the
  other apps"}
common: {sysbuild: true}
tests:
  sample.esp_sdr.multiapp:
    platform_allow: [xiao_esp32s3/esp32s3/procpu, ues31s043h800v480c_u/esp32s31/hpcore,
                     esp32s31_function_coreboard/esp32s31/hpcore]
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
    platform_allow: [ues31s043h800v480c_u/esp32s31/hpcore,
                     esp32s31_function_coreboard/esp32s31/hpcore]
  esp_sdr.osmosdr_hs.loadlog:
    platform_allow: [ues31s043h800v480c_u/esp32s31/hpcore]
    extra_args: EXTRA_CONF_FILE=loadlog.conf
""",
    "stream_eth": """
sample: {name: stream_eth, description: VITA 49.2 over gigabit Ethernet}
tests:
  sample.esp_sdr.stream_eth:
    platform_allow: [esp32s31_function_coreboard/esp32s31/hpcore]
  sample.esp_sdr.stream_eth.onecore:
    platform_allow: [esp32s31_function_coreboard/esp32s31/hpcore]
    extra_args: EXTRA_CONF_FILE=onecore.conf
""",
    "txstream_eth": """
sample: {name: txstream_eth, description: VITA 49.2 transmit over gigabit Ethernet}
tests:
  sample.esp_sdr.txstream_eth:
    platform_allow: [esp32s31_function_coreboard/esp32s31/hpcore]
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
    (apps / "common/sysbuild").mkdir(parents=True)
    (apps / "common/sysbuild/Kconfig.signing").write_text(SIGNING)
    (apps / "multiapp/Kconfig.sysbuild").write_text(OPTS_IN)
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


def test_dev_key_multiapp_is_bench_only_and_never_recommended(tmp_path, run_main):
    apps, result = tree(tmp_path, {
        "esp32s31_function_coreboard": ["multiapp", "osmosdr_hs", "stream_eth",
                                        "stream_eth-onecore", "txstream_eth"],
        "xiao_esp32s3": ["multiapp", "osmosdr"]},
        {"esp32s31_function_coreboard": ["osmosdr_hs"], "xiao_esp32s3": ["osmosdr"]})
    out = tmp_path / "out"
    run_main(release, result, apps, "v1.2.3", out)

    got = entries(out, "esp32s31_function_coreboard")
    assert {n: e["devKey"] for n, e in got.items()} == {
        "multiapp": True, "osmosdr_hs": False, "stream_eth": False,
        "stream_eth-onecore": False, "txstream_eth": False}
    # The bench only multiapp is no recommendation, and what it carries stands on its own.
    assert {n: (e["rank"], e["inMultiapp"]) for n, e in got.items()} == {
        "multiapp": (2, False), "osmosdr_hs": (1, False), "stream_eth": (2, False),
        "stream_eth-onecore": (2, False), "txstream_eth": (2, False)}
    assert got["stream_eth"]["description"] == "VITA 49.2 over gigabit Ethernet"
    assert got["stream_eth-onecore"]["file"] == (
        "esp-sdr-stream_eth-onecore-v1.2.3-esp32s31_function_coreboard.bin")
    listed = json.loads((out / "firmware-esp32s31_function_coreboard.json").read_text())
    assert [e["name"] for e in listed][0] == "osmosdr_hs"

    # The same app on a board whose sysbuild does not sign stays the board's first image.
    xiao = entries(out, "xiao_esp32s3")
    assert xiao["multiapp"]["devKey"] is False
    assert (xiao["multiapp"]["rank"], xiao["osmosdr"]["inMultiapp"]) == (0, True)


def kconfig_tree(tmp_path, signing, sysbuild_kconfig):
    apps = tmp_path / "apps"
    for app in ("multiapp", "osmosdr"):
        (apps / app).mkdir(parents=True)
        (apps / app / "sample.yaml").write_text(SAMPLES[app])
    if signing is not None:
        (apps / "common/sysbuild").mkdir(parents=True)
        (apps / "common/sysbuild/Kconfig.signing").write_text(signing)
    if sysbuild_kconfig is not None:
        (apps / "multiapp/Kconfig.sysbuild").write_text(sysbuild_kconfig)
    return apps


def dev_keys(apps):
    return {f"{i['board']}/{i['name']}" for i in images.list_images(apps) if i["devKey"]}


def test_dev_key_follows_the_signing_kconfig(tmp_path):
    apps = kconfig_tree(tmp_path / "a", SIGNING, OPTS_IN)
    assert images.signing_boards(apps) == {"esp32s31_function_coreboard", "other_board"}
    assert dev_keys(apps) == {"esp32s31_function_coreboard/multiapp"}
    # The app has to opt in, as multiapp's Kconfig.sysbuild does.
    assert dev_keys(kconfig_tree(tmp_path / "b", SIGNING, 'source "share/sysbuild/Kconfig"\n')) == set()
    assert dev_keys(kconfig_tree(tmp_path / "c", SIGNING, None)) == set()
    # Without the signing file no image signs.
    assert dev_keys(kconfig_tree(tmp_path / "d", None, OPTS_IN)) == set()
    # A default for every board.
    everywhere = SIGNING.replace('default y if BOARD = "esp32s31_function_coreboard" || BOARD = '
                                 '"other_board"', "default y")
    apps = kconfig_tree(tmp_path / "e", everywhere, OPTS_IN)
    assert images.signing_boards(apps) == {"*"}
    assert dev_keys(apps) == {
        "esp32s31_function_coreboard/multiapp", "ues31s043h800v480c_u/multiapp",
        "xiao_esp32s3/multiapp"}


def test_a_default_it_cannot_read_stops_the_image_list(tmp_path):
    for k, bad in enumerate((
            'default y if BOARD != "esp32s31_function_coreboard"',
            'default y if !(BOARD = "esp32s31_function_coreboard")',
            'default n if BOARD = "xiao_esp32s3"',
            'default y if SOC_SERIES_ESP32S31',
            'default y if BOARD = "a" && SOC_SERIES_ESP32S31',
            'default y if y',
    )):
        text = SIGNING.replace(
            'default y if BOARD = "esp32s31_function_coreboard" || BOARD = "other_board"', bad)
        apps = kconfig_tree(tmp_path / str(k), text, OPTS_IN)
        with pytest.raises(SystemExit) as stop:
            images.list_images(apps)
        assert bad in str(stop.value)
        assert "Kconfig.signing:4:" in str(stop.value)
    # A comment after the condition, and "default n", are read.
    ok = SIGNING.replace('default y if BOARD = "esp32s31_function_coreboard" || BOARD = "other_board"',
                         'default n\n\tdefault y if BOARD = "a"  # the bench board')
    assert images.signing_boards(kconfig_tree(tmp_path / "ok", ok, OPTS_IN)) == {"a"}
    # A file without the config is not one this script knows.
    none = SIGNING.replace("config ESDR_SIGNING", "config OTHER")
    with pytest.raises(SystemExit):
        images.signing_boards(kconfig_tree(tmp_path / "none", none, OPTS_IN))


def test_the_repository_signs_one_image_with_the_dev_key():
    """The S31 CoreBoard's multiapp is the only image whose sysbuild signs with the own key:
    another one needs a decision on how the release builds it."""
    listed = images.list_images(REPO / "apps")
    assert {f"{i['board']}/{i['name']}" for i in listed if i["devKey"]} == {
        "esp32s31_function_coreboard/multiapp"}
    names = {i["name"] for i in listed if i["board"] == "esp32s31_function_coreboard"}
    assert {"multiapp", "stream_eth", "stream_eth-onecore", "stream_eth-product", "txstream_eth",
            "txstream_eth-injector", "rxbench", "ptp_chain"} <= names
    assert "coexbench" in {i["name"] for i in listed if i["board"] == "xiao_esp32s3"}
