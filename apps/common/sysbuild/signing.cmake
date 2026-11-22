# SPDX-License-Identifier: GPL-3.0-or-later
#
# The own signing key (Kconfig.signing next to this file): checks the key file, warns about the
# development key, and gives the image its version. include() it from sysbuild.cmake.
# The key is never read here, and its content is never printed.

if(SB_CONFIG_ESDR_SIGNING)
  if("$ENV{ESDR_DEV_KEY}" STREQUAL "1")
    message(WARNING
      "\n"
      "************************************************************************\n"
      "* ESDR_DEV_KEY=1: MCUboot is built with the PUBLIC development key     *\n"
      "* ${SB_CONFIG_BOOT_SIGNATURE_KEY_FILE}\n"
      "* Anybody can sign an image this bootloader boots. Not for a product.  *\n"
      "************************************************************************\n")
  elseif(SB_CONFIG_BOOT_SIGNATURE_KEY_FILE MATCHES "/root-[^/]*$"
         OR SB_CONFIG_BOOT_SIGNATURE_KEY_FILE MATCHES "/mcuboot/[^ ]*\\.pem$")
    # A stale .config (made with ESDR_DEV_KEY=1) or a typed-in path: independent of the environment
    message(FATAL_ERROR
      "\nThe MCUboot signing key is MCUboot's public development key:\n"
      "  ${SB_CONFIG_BOOT_SIGNATURE_KEY_FILE}\n"
      "Anybody can sign an image this bootloader boots. Use ESDR_DEV_KEY=1 for it on purpose, else\n"
      "remove the build directory (its .config remembers the old key) or point ESDR_SIGNING_KEY at your own key.")
  elseif(NOT EXISTS "${SB_CONFIG_BOOT_SIGNATURE_KEY_FILE}")
    message(FATAL_ERROR
      "\nThe MCUboot signing key file does not exist:\n"
      "  ${SB_CONFIG_BOOT_SIGNATURE_KEY_FILE}\n"
      "It is the file in the environment variable ESDR_SIGNING_KEY, by default\n"
      "~/.config/esp-sdr/keys/esdr-mcuboot-ed25519.pem. Make one (once, keep it safe, never commit it):\n"
      "  mkdir -p ~/.config/esp-sdr/keys && chmod 700 ~/.config/esp-sdr/keys\n"
      "  nix-shell -p mcuboot-imgtool --run 'imgtool keygen -k ~/.config/esp-sdr/keys/esdr-mcuboot-ed25519.pem -t ed25519'\n"
      "or ESDR_DEV_KEY=1 for MCUboot's public development key (a bench build, anybody can sign for it).")
  endif()

  # The image version: VERSION's major.minor.patch, and the git short hash of the app's tree (the
  # first 8 hex digits of `git rev-parse --short=8 HEAD`) as the 32 bit build number; bit 15 of the
  # patch field is set when the tree has uncommitted changes. Set as an image setting at every configure (a change of HEAD
  # re-runs the configure, a change of the working tree does not: the dirty flag is the state at the
  # last configure).
  file(STRINGS ${APP_DIR}/VERSION version_lines)
  foreach(line ${version_lines})
    if(line MATCHES "^(VERSION_MAJOR|VERSION_MINOR|PATCHLEVEL)[ ]*=[ ]*([0-9]+)")
      set(ver_${CMAKE_MATCH_1} ${CMAKE_MATCH_2})
    endif()
  endforeach()
  set(build 0)
  find_program(GIT_EXECUTABLE git)
  if(GIT_EXECUTABLE)
    execute_process(COMMAND ${GIT_EXECUTABLE} -C ${APP_DIR} rev-parse --short=8 HEAD
                    OUTPUT_VARIABLE git_hash OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET
                    RESULT_VARIABLE git_rc)
    if(git_rc EQUAL 0 AND git_hash MATCHES "^[0-9a-f]+$")
      # Exactly eight digits, whatever git abbreviated to
      string(SUBSTRING "${git_hash}" 0 8 git_hash)
      execute_process(COMMAND ${GIT_EXECUTABLE} -C ${APP_DIR} status --porcelain --untracked-files=no
                      OUTPUT_VARIABLE git_dirty OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
      math(EXPR build "0x${git_hash}" OUTPUT_FORMAT DECIMAL)
      if(NOT "${git_dirty}" STREQUAL "")
        # The build number is all hash (8 digits): the flag rides on the
        # patch field's top bit
        math(EXPR ver_PATCHLEVEL "${ver_PATCHLEVEL} + 0x8000" OUTPUT_FORMAT DECIMAL)
      endif()
      # Configure again when HEAD or its branch moves
      foreach(ref HEAD)
        execute_process(COMMAND ${GIT_EXECUTABLE} -C ${APP_DIR} rev-parse --git-path ${ref}
                        OUTPUT_VARIABLE ref_path OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
        execute_process(COMMAND ${GIT_EXECUTABLE} -C ${APP_DIR} symbolic-ref -q HEAD
                        OUTPUT_VARIABLE branch OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
        foreach(p ${ref_path})
          cmake_path(ABSOLUTE_PATH p BASE_DIRECTORY ${APP_DIR})
          set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${p})
        endforeach()
        if(branch)
          execute_process(COMMAND ${GIT_EXECUTABLE} -C ${APP_DIR} rev-parse --git-path ${branch}
                          OUTPUT_VARIABLE branch_path OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
          cmake_path(ABSOLUTE_PATH branch_path BASE_DIRECTORY ${APP_DIR})
          set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${branch_path})
        endif()
      endforeach()
    endif()
  endif()
  if(NOT DEFINED DEFAULT_IMAGE)
    set(DEFAULT_IMAGE multiapp)
  endif()
  # The host then insists on signed extensions (apps/multiapp/CMakeLists.txt)
  set(${DEFAULT_IMAGE}_CONFIG_MULTIAPP_SIGNED_EXTENSIONS y CACHE INTERNAL "the image is signed with the own key")
  # Set at every configure; as an image setting, so that -D...EXTRA_CONF_FILE on the command line leaves it be
  set(${DEFAULT_IMAGE}_CONFIG_MCUBOOT_IMGTOOL_SIGN_VERSION
      "\"${ver_VERSION_MAJOR}.${ver_VERSION_MINOR}.${ver_PATCHLEVEL}+${build}\""
      CACHE INTERNAL "the image's version for imgtool")
endif()
