cmake_minimum_required(VERSION 2.8)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  if(DEFINED ENV{VITASDK})
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "toolchain file")
  else()
    message(FATAL_ERROR "Please define VITASDK to point to your SDK path!")
  endif()
endif()

project(Bully C CXX)
include("${VITASDK}/share/vita.cmake" REQUIRED)
set(VITA_APP_NAME "Bully")
set(VITA_TITLEID  "BULLY0000")
# APP_VER as LiveArea shows it. This was never set, so every vpk the project
# ever produced defaulted to 01.00 -- upstream's included. 01.01 is the first
# release with the texture cache, and the first with a number that means one.
set(VITA_VERSION  "01.01")
# ATTRIBUTE2=12 is upstream's. CONTENT_ID is not: vita-mksfoex defaults it to
# HB0001-ABCD99999_00-0000000000000000, a placeholder every homebrew built with
# the toolchain shares, and it does not match the title id sitting next to it.
# The format is SSSSSS-TTTTTTTTT_00 plus sixteen digits, so the title id goes in
# where the placeholder was.
set(VITA_MKSFOEX_FLAGS "-d ATTRIBUTE2=12 -s CONTENT_ID=HB0001-${VITA_TITLEID}_00-0000000000000000")

# --section-start: vita-elf-create appends a few kilobytes of SCE metadata to the
# end of the executable segment, and the linker had been leaving it only whatever
# was left below the next 64 KB boundary, where the writable segment starts. That
# margin was down to 1388 bytes, and a commit that added four kilobytes of code
# failed the build with "Cannot allocate 4496 bytes for SCE data at end of
# segment 0; segment 1 overlaps" -- a cliff with a 64 KB period that any commit
# could walk off, reported by a tool that then segfaulted rather than saying what
# to do about it. Pinning the writable segment where it is gives the metadata a
# fixed 128 KB of room instead of a remainder. (-z max-page-size does not work
# here: it merges the two segments into one RWE segment.)
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -g -Wl,-q,--section-start=.init_array=0x81190000,--wrap,memcpy,--wrap,memmove,--wrap,memset -D_GNU_SOURCE -Wall -O3 -mfloat-abi=softfp")
set(CMAKE_CXX_FLAGS "${CMAKE_C_FLAGS} -std=c++11")

# Regenerated on every build, so the banner in the log names the commit that
# produced the binary rather than whenever main.c was last recompiled.
add_custom_target(build_id ALL
  COMMAND ${CMAKE_COMMAND} -DSRC=${CMAKE_SOURCE_DIR}
                           -DOUT=${CMAKE_BINARY_DIR}/build_id.h
                           -P ${CMAKE_SOURCE_DIR}/cmake/build_id.cmake
  BYPRODUCTS ${CMAKE_BINARY_DIR}/build_id.h
  COMMENT "Stamping the build id"
)
include_directories(${CMAKE_BINARY_DIR})

add_executable(Bully
  loader/main.c
  loader/streaming_patch.c
  loader/io_census.c
  loader/open_memo.c
  loader/frame_profile.c
  loader/prologue.c
  loader/texture_cache.c
  loader/vertex_cache.c
  loader/dialog.c
  loader/fios.c
  loader/so_util.c
  loader/jni_patch.c
  loader/movie_patch.c
  loader/openal_patch.c
)

target_link_libraries(Bully
  -Wl,--whole-archive pthread -Wl,--no-whole-archive
  # These depend on each other in both directions -- vitaGL calls into mathneon
  # and libstdc++, openal calls into libm -- and a linker only resolves an
  # archive's undefined symbols from archives listed after it. Group them so it
  # iterates until everything is resolved instead of relying on one true order.
  -Wl,--start-group
  mathneon
  mpg123
  openal
  vitaGL
  vitashark
  # vitaShaRK references these to patch the ShaccCg module through taiHEN.
  SceShaccCgExt
  taihen_stub
  stdc++
  m
  -Wl,--end-group
  kubridge_stub
  SceAppMgr_stub
  SceAppUtil_stub
  SceAudio_stub
  SceAudioIn_stub
  SceAvPlayer_stub
  SceCtrl_stub
  SceCommonDialog_stub
  SceDisplay_stub
  SceKernelDmacMgr_stub
  SceFios2_stub
  SceGxm_stub
  SceLibcBridge_stub
  SceShaccCg_stub
  SceSysmodule_stub
  ScePower_stub
  SceTouch_stub
  SceVshBridge_stub
)

add_dependencies(Bully build_id)

vita_create_self(eboot.bin Bully UNSAFE)
vita_create_vpk(Bully.vpk ${VITA_TITLEID} eboot.bin
  VERSION ${VITA_VERSION}
  NAME ${VITA_APP_NAME}
  FILE ${CMAKE_SOURCE_DIR}/sce_sys/icon0.png sce_sys/icon0.png
       ${CMAKE_SOURCE_DIR}/sce_sys/livearea/contents/bg.png sce_sys/livearea/contents/bg.png
       ${CMAKE_SOURCE_DIR}/sce_sys/livearea/contents/startup.png sce_sys/livearea/contents/startup.png
       ${CMAKE_SOURCE_DIR}/sce_sys/livearea/contents/template.xml sce_sys/livearea/contents/template.xml
)

add_custom_target(copy
  COMMAND cp eboot.bin E:/app/${VITA_TITLEID}/eboot.bin
  DEPENDS eboot.bin
)
