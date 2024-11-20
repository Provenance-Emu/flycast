#!/bin/sh
set -e
if test "$CONFIGURATION" = "Debug"; then :
  cd /Users/jmattiello/Workspace/Provenance/Provenance/Cores/Flycast/flycast/build
  /opt/homebrew/Cellar/cmake/3.30.5/bin/cmake -D_CMRC_GENERATE_MODE=TRUE -DNAMESPACE=flycast -DSYMBOL=f_2788_fonts_printer_kanji24x24_bin_zip -DINPUT_FILE=/Users/jmattiello/Workspace/Provenance/Provenance/Cores/Flycast/flycast/fonts/printer_kanji24x24.bin.zip -DOUTPUT_FILE=/Users/jmattiello/Workspace/Provenance/Provenance/Cores/Flycast/flycast/build/__cmrc_flycast-resources/intermediate/fonts/printer_kanji24x24.bin.zip.cpp -P /Users/jmattiello/Workspace/Provenance/Provenance/Cores/Flycast/flycast/shell/cmake/CMakeRC.cmake
fi
if test "$CONFIGURATION" = "Release"; then :
  cd /Users/jmattiello/Workspace/Provenance/Provenance/Cores/Flycast/flycast/build
  /opt/homebrew/Cellar/cmake/3.30.5/bin/cmake -D_CMRC_GENERATE_MODE=TRUE -DNAMESPACE=flycast -DSYMBOL=f_2788_fonts_printer_kanji24x24_bin_zip -DINPUT_FILE=/Users/jmattiello/Workspace/Provenance/Provenance/Cores/Flycast/flycast/fonts/printer_kanji24x24.bin.zip -DOUTPUT_FILE=/Users/jmattiello/Workspace/Provenance/Provenance/Cores/Flycast/flycast/build/__cmrc_flycast-resources/intermediate/fonts/printer_kanji24x24.bin.zip.cpp -P /Users/jmattiello/Workspace/Provenance/Provenance/Cores/Flycast/flycast/shell/cmake/CMakeRC.cmake
fi
if test "$CONFIGURATION" = "MinSizeRel"; then :
  cd /Users/jmattiello/Workspace/Provenance/Provenance/Cores/Flycast/flycast/build
  /opt/homebrew/Cellar/cmake/3.30.5/bin/cmake -D_CMRC_GENERATE_MODE=TRUE -DNAMESPACE=flycast -DSYMBOL=f_2788_fonts_printer_kanji24x24_bin_zip -DINPUT_FILE=/Users/jmattiello/Workspace/Provenance/Provenance/Cores/Flycast/flycast/fonts/printer_kanji24x24.bin.zip -DOUTPUT_FILE=/Users/jmattiello/Workspace/Provenance/Provenance/Cores/Flycast/flycast/build/__cmrc_flycast-resources/intermediate/fonts/printer_kanji24x24.bin.zip.cpp -P /Users/jmattiello/Workspace/Provenance/Provenance/Cores/Flycast/flycast/shell/cmake/CMakeRC.cmake
fi
if test "$CONFIGURATION" = "RelWithDebInfo"; then :
  cd /Users/jmattiello/Workspace/Provenance/Provenance/Cores/Flycast/flycast/build
  /opt/homebrew/Cellar/cmake/3.30.5/bin/cmake -D_CMRC_GENERATE_MODE=TRUE -DNAMESPACE=flycast -DSYMBOL=f_2788_fonts_printer_kanji24x24_bin_zip -DINPUT_FILE=/Users/jmattiello/Workspace/Provenance/Provenance/Cores/Flycast/flycast/fonts/printer_kanji24x24.bin.zip -DOUTPUT_FILE=/Users/jmattiello/Workspace/Provenance/Provenance/Cores/Flycast/flycast/build/__cmrc_flycast-resources/intermediate/fonts/printer_kanji24x24.bin.zip.cpp -P /Users/jmattiello/Workspace/Provenance/Provenance/Cores/Flycast/flycast/shell/cmake/CMakeRC.cmake
fi

