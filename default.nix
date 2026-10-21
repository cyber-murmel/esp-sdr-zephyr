# SPDX-License-Identifier: GPL-3.0-or-later
#
# nix-build: every image of apps/*/sample.yaml by board (result/<board>/<app>/, see
# nix/collect.py) and the host tools (result/bin, the esdr SoapySDR module in result/lib).
#
# The firmware builds from this repo as it is on disk and the other west projects at the
# revisions in west2nix.toml, fetched from their remotes, and their blobs. After
# a west.yml change, run `west2nix` (in the shell) and commit west2nix.toml. One image is one
# derivation: nix-build -A firmware.xiao_esp32s3.osmosdr builds just that one.
#
# Attributes: firmware, tools, soapy-esdr, soapysdr (with the esdr module), westWorkspace,
# dev (the packages shell.nix has) and shell.

let
  nixpkgs-ref = "c9bfd86ed684d27e63b0ff9ebb18699f84f27a3b"; # nixos-25.11-small
  pyproject-nix-ref = "69f57f27e52a87c54e28138a75ec741cd46663c9";
  zephyr-nix-ref = "6966fb1cbf2fdb494bea3062c5e8e7d44dd8ac9c";
  nixpkgs-esp-dev-ref = "5287d6e1ca9e15ebd5113c41b9590c468e1e001b";
  west2nix-ref = "f84670d66f881d9340b7d7626fbfe499438c134b";

  nixpkgs-esp-dev = builtins.fetchGit {
    url = "https://github.com/mirrexagon/nixpkgs-esp-dev.git";
    rev = nixpkgs-esp-dev-ref;
  };
in

{
  pkgs ? (import (builtins.fetchTarball "https://github.com/NixOS/nixpkgs/archive/${nixpkgs-ref}.tar.gz") {
    overlays = [ (import "${nixpkgs-esp-dev}/overlay.nix") ];
    # The Python library ecdsa is marked as insecure, but we need it for esptool.
    # See https://github.com/mirrexagon/nixpkgs-esp-dev/issues/109
    config.permittedInsecurePackages = [
      "python3.13-ecdsa-0.19.1"
    ];
  }),
  zephyr-src-ref ? "v4.4.0",
  # Apps left out of the firmware and the host tools.
  excludeApps ? [ ],
}:

let
  inherit (pkgs) lib;

  versionOf = key: lib.head (lib.concatMap (line: lib.defaultTo [ ] (builtins.match "${key} = ([0-9]+)" line))
    (lib.splitString "\n" (builtins.readFile ./VERSION)));
  version = lib.concatMapStringsSep "." versionOf [ "VERSION_MAJOR" "VERSION_MINOR" "PATCHLEVEL" ];

  zephyr-nix = ((import (builtins.fetchTarball "https://github.com/nix-community/zephyr-nix/archive/${zephyr-nix-ref}.tar.gz")) {
    inherit (pkgs) lib newScope openocd autoreconfHook fetchFromGitHub gcc_multi python312;
    zephyr-src = builtins.fetchTarball "https://github.com/zephyrproject-rtos/zephyr/archive/${zephyr-src-ref}.tar.gz";
    pyproject-nix = (import (builtins.fetchTarball "https://github.com/pyproject-nix/pyproject.nix/archive/${pyproject-nix-ref}.tar.gz")) {
      inherit (pkgs) lib;
    };
  });

  west2nix = pkgs.callPackage (builtins.fetchTarball "https://github.com/adisbladis/west2nix/archive/${west2nix-ref}.tar.gz") { };

  # west2nix.toml from the workspace's west projects (at their manifest revisions), written
  # here; run from anywhere in the workspace. west2nix takes the revisions from manifest-rev,
  # which only west update moves: stop when one is behind west.yml.
  west2nix-update = pkgs.writeShellScriptBin "west2nix" ''
    set -e
    stale=$(west list -f '{name} {revision} {sha}' | ${pkgs.gawk}/bin/awk '$2 ~ /^[0-9a-f]{40}$/ && $2 != $3 { print $1 }')
    if [ -n "$stale" ]; then
      echo "manifest-rev is not the west.yml revision in: $stale" >&2
      echo "(git update-ref refs/heads/manifest-rev <revision> there, or west update)" >&2
      exit 1
    fi
    manifest=$(dirname "$(west manifest --path)")
    cd "$(west topdir)"
    ${west2nix.west2nix}/bin/west2nix "$@"
    mv west2nix.toml "$manifest/west2nix.toml"
    echo "moved to $manifest/west2nix.toml"
  '';

  zephyr-sdk = zephyr-nix.sdk.override {
    targets = [
      "xtensa-espressif_esp32_zephyr-elf"
      "xtensa-espressif_esp32s3_zephyr-elf"
      "riscv64-zephyr-elf"
    ];
  };

  python = zephyr-nix.pythonEnv.override {
    extraPackages = ps: with ps; [
      jsonschema
      # Markdown pages in the Sphinx docs (doc/).
      myst-parser
    ];
  };

  gnuradio = pkgs.gnuradio.override {
    extraPackages = with pkgs.gnuradioPackages; [
      osmosdr
    ];
    extraPythonPackages = with pkgs.gnuradio.python.pkgs; [
      numpy
      matplotlib
      pyqt5
      # The host tools' serial ports.
      pyserial
    ];
  };

  # The SoapySDR module of apps/osmosdr ("driver=esdr").
  soapy-esdr = pkgs.stdenv.mkDerivation {
    pname = "soapy-esdr";
    inherit version;
    src = lib.fileset.toSource {
      root = ./apps/osmosdr;
      fileset = lib.fileset.unions [ ./apps/osmosdr/soapy ./apps/osmosdr/src/esdr_proto.h ];
    };
    cmakeDir = "../soapy";
    nativeBuildInputs = [ pkgs.cmake ];
    buildInputs = [ pkgs.soapysdr pkgs.libusb1 ];
  };

  soapyPlugins = [ pkgs.soapyremote soapy-esdr ];
  soapyPluginPath = lib.makeSearchPath pkgs.soapysdr.passthru.searchPath soapyPlugins;
  # SoapySDRUtil and friends with the plugins.
  soapysdr = pkgs.soapysdr.override { extraPackages = soapyPlugins; };

  # Espressif's RISC-V binutils: their assembler knows the ESP32-P4's PIE vector instructions
  # (xesppie), the Zephyr SDK's does not. apps/common assembles the P4 channelizer's SIMD
  # kernels with it (src/esdr_pfbq_pie.S); a build without it runs the C reference kernels,
  # some 40 times slower.
  riscv32-esp-binutils = pkgs.stdenv.mkDerivation rec {
    pname = "riscv32-esp-elf-binutils";
    version = "14.2.0_20241119";
    src = pkgs.fetchurl {
      url = "https://github.com/espressif/crosstool-NG/releases/download/esp-${version}/riscv32-esp-elf-${version}-x86_64-linux-gnu.tar.xz";
      sha256 = "7faaa86d272f3e43c233f8a5ffeba327673224a752c2eb72394655d5e7950000";
    };
    nativeBuildInputs = [ pkgs.autoPatchelfHook ];
    buildInputs = [ pkgs.stdenv.cc.cc.lib pkgs.zlib ];
    dontConfigure = true;
    dontBuild = true;
    installPhase = ''
      mkdir -p $out/bin
      for t in as objdump readelf; do
        cp bin/riscv32-esp-elf-$t $out/bin/
      done
    '';
  };

  # What a firmware build needs.
  build = [
    pkgs.cmake
    pkgs.esptool
    pkgs.ninja
    python
    riscv32-esp-binutils
    zephyr-sdk
  ];

  # The development shell's packages.
  dev = build ++ (with pkgs; [
    black
    clang-tools
    dfu-util
    doxygen
    (esp-idf-xtensa.tools.openocd-esp32)
    gnuradio
    gqrx
    inspectrum
    librsvg
    libusb1
    mermaid-filter
    pandoc
    # The CI checks as git hooks (.pre-commit-config.yaml).
    pre-commit
    qemu-esp32
    soapyremote
    soapysdr
    sphinx
    tcpdump
    texliveBasic
    west2nix-update
  ]);

  env = {
    ZEPHYR_TOOLCHAIN_VARIANT = "zephyr";
    ZEPHYR_SDK_INSTALL_DIR = "${zephyr-sdk}";
  };

  shell = pkgs.mkShell (env // {
    packages = dev;
    # gqrx and GNU Radio find the esdr module too.
    SOAPY_SDR_PLUGIN_PATH = soapyPluginPath;
  });

  # Firmware

  yamlPython = pkgs.python3.withPackages (ps: [ ps.pyyaml ]);
  pycFiles = dir: lib.fileset.fileFilter (f: f.hasExt "pyc") dir;
  excludedApps = map (app: lib.fileset.maybeMissing (./apps + "/${app}")) excludeApps;

  # The images twister builds, from the sample.yaml files alone (nix/images.py).
  imageList = builtins.fromJSON (builtins.readFile (pkgs.runCommand "esp-sdr-images.json" {
    nativeBuildInputs = [ yamlPython ];
    samples = lib.fileset.toSource {
      root = ./apps;
      fileset = lib.fileset.fileFilter (f: f.name == "sample.yaml") ./apps;
    };
  } "python3 ${./nix/images.py} $samples ${lib.escapeShellArgs excludeApps} > $out"));

  lock = lib.importTOML ./west2nix.toml;
  selfPath = lock.manifest.self.path;

  # A project's source, as the west2nix hook fetches it.
  fetchProject = project: pkgs.fetchgit {
    inherit (project) url;
    inherit (project.nix) hash;
    fetchSubmodules = project.submodules or false;
    rev = project.revision;
  };

  # What west blobs fetch would put into the workspace (nix/blobs.py).
  moduleYmls = [ "${selfPath}=${./zephyr/module.yml}" ] ++ lib.concatMap (project:
    let yml = "${fetchProject project}/zephyr/module.yml"; in
    lib.optional (builtins.pathExists yml) "${project.path or project.name}=${yml}"
  ) lock.manifest.projects;
  blobList = builtins.fromJSON (builtins.readFile (pkgs.runCommand "esp-sdr-blobs.json" {
    nativeBuildInputs = [ yamlPython ];
  } "python3 ${./nix/blobs.py} ${lib.escapeShellArgs moduleYmls} > $out"));

  # This repo, as far as the firmware builds from it (its blobs come from blobList).
  firmwareSrc = lib.fileset.toSource {
    root = ./.;
    fileset = lib.fileset.difference
      (lib.fileset.unions [
        ./CMakeLists.txt
        ./Kconfig
        ./VERSION
        ./west.yml
        ./apps
        ./boards
        ./include
        ./lib
        ./scripts/west-commands.yml
        ./scripts/west_commands
        ./zephyr
      ])
      (lib.fileset.unions ([ (pycFiles ./apps) (pycFiles ./scripts) (lib.fileset.maybeMissing ./zephyr/blobs/lib) ] ++ excludedApps));
  };

  # The west workspace, made in $out: this repo at its path, the west2nix hook copies the other
  # projects in (each a git repo, which west wants) and runs west init, then the blobs.
  westWorkspace = pkgs.stdenvNoCC.mkDerivation {
    name = "esp-sdr-west-workspace-${version}";
    src = firmwareSrc;
    nativeBuildInputs = [
      (west2nix.mkWest2nixHook { manifest = ./west2nix.toml; })
      pkgs.gitMinimal
      python
    ];
    unpackPhase = ''
      mkdir -p $out
      cp -r $src $out/${selfPath}
      chmod -R u+w $out
      cd $out
    '';
    buildPhase = ''
      cd $out
      chmod -R u+w .
      # West would write this on its first command, read-only later.
      west config zephyr.base modules/zephyr
      ${lib.concatMapStrings (blob: ''
        install -Dm644 ${pkgs.fetchurl { inherit (blob) url sha256; }} ${blob.path}
      '') blobList}
    '';
    dontInstall = true;
    dontFixup = true;
  };

  mkImage = img: pkgs.stdenvNoCC.mkDerivation (env // {
    name = "esp-sdr-${img.board}-${img.name}-${version}";
    nativeBuildInputs = build ++ [ pkgs.gitMinimal ];
    buildCommand = ''
      export HOME=$TMPDIR
      # Zephyr's caches, which it would otherwise put into ZEPHYR_BASE.
      mkdir -p $HOME/.cache
      # West finds its projects (and so west build) by git, in a store path someone else owns.
      git config --global safe.directory '*'
      cd ${westWorkspace}/${selfPath}
      west build -d $TMPDIR/build -b ${img.platform} ${lib.optionalString img.sysbuild "--sysbuild"} \
        apps/${img.app} -o=-j$NIX_BUILD_CORES \
        -- ${lib.escapeShellArgs (map (arg: "-D${arg}") img.extraArgs)}
      python3 ${./nix/collect.py} $TMPDIR/build ${img.app} ${img.chip} $out/${img.board}/${img.name}
    '';
  });

  # firmware.<board>.<app>
  firmware = lib.mapAttrs (board: images: lib.listToAttrs (map (img: lib.nameValuePair img.name (mkImage img)) images))
    (lib.groupBy (img: img.board) imageList);

  # Host tools

  apps = lib.filter (app: !(lib.elem app excludeApps)) (lib.attrNames (lib.filterAttrs (n: t: t == "directory") (builtins.readDir ./apps)));
  toolDirs = lib.filter builtins.pathExists (map (app: ./apps + "/${app}/tools") apps);

  qt = gnuradio.unwrapped.qt;
  toolWrapperArgs = lib.escapeShellArgs [
    "--prefix" "SOAPY_SDR_PLUGIN_PATH" ":" soapyPluginPath
    "--prefix" "QT_PLUGIN_PATH" ":" (lib.makeSearchPath qt.qtbase.qtPluginPrefix (map lib.getBin [ qt.qtbase qt.qtwayland ]))
  ];

  # The apps' tools/*.py with GNU Radio's Python, esdr_usb and the link simulators. The tools
  # keep the tree's layout (libexec/esp-sdr/apps/<app>/tools/), as some import their siblings
  # and some another app's tools; modules named _*.py only serve the tools next to them.
  host-tools = pkgs.stdenv.mkDerivation {
    pname = "esp-sdr-host-tools";
    inherit version;
    src = lib.fileset.toSource {
      root = ./.;
      fileset = lib.fileset.difference
        (lib.fileset.unions ([ ./include ./apps/link/src ./apps/osmosdr/src/esdr_proto.h
          ./apps/osmosdr/src/esdr_seg.h ] ++ toolDirs))
        (pycFiles ./apps);
    };
    nativeBuildInputs = [ pkgs.makeWrapper ];
    buildInputs = [ pkgs.libusb1 ];
    buildPhase = ''
      runHook preBuild
      cc -O2 -Wall -Iinclude -Iapps/osmosdr/src -I${lib.getDev pkgs.libusb1}/include/libusb-1.0 \
        apps/osmosdr/tools/esdr_usb.c -lusb-1.0 -lm -o esdr_usb
      (cd apps/link
       cc -O2 -Wall -Isrc -I../../include src/qam.c src/rs.c src/hamming.c tools/qam_sim.c \
         -lm -o ../../qam_sim
       cc -O2 -Wall -Isrc -I../../include src/ofdm.c src/ofdm_frame.c src/qam.c src/rs.c \
         src/bch.c src/hamming.c tools/ofdm_sim.c -lm -o ../../ofdm_sim)
      runHook postBuild
    '';
    installPhase = ''
      runHook preInstall
      install -Dm755 -t $out/bin esdr_usb qam_sim ofdm_sim
      for tool in apps/*/tools/*.py; do
        name=$(basename $tool)
        install -Dm644 $tool $out/libexec/esp-sdr/$tool
        case $name in _*) continue ;; esac
        makeWrapper ${gnuradio.pythonEnv}/bin/python3 $out/bin/$name \
          --add-flags $out/libexec/esp-sdr/$tool ${toolWrapperArgs}
      done
      runHook postInstall
    '';
  };

  tools = pkgs.symlinkJoin {
    name = "esp-sdr-tools-${version}";
    paths = [ host-tools soapysdr pkgs.esptool pkgs.dfu-util ];
    nativeBuildInputs = [ pkgs.makeWrapper ];
    postBuild = ''
      makeWrapper ${pkgs.gqrx}/bin/gqrx $out/bin/gqrx --prefix SOAPY_SDR_PLUGIN_PATH : ${soapyPluginPath}
    '';
  };
in
pkgs.symlinkJoin {
  name = "esp-sdr-zephyr-${version}";
  paths = lib.concatMap lib.attrValues (lib.attrValues firmware) ++ [ tools ];
  passthru = {
    inherit pkgs dev shell firmware tools host-tools soapy-esdr soapysdr westWorkspace
      riscv32-esp-binutils;
    west2nix = west2nix-update;
  };
}
