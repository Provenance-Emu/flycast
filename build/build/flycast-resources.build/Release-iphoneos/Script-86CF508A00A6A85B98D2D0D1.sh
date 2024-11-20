#!/bin/sh
set -e
if test "$CONFIGURATION" = "Debug"; then :
  cd /Users/jmattiello/Workspace/Provenance/Provenance/Cores/Flycast/flycast/build
  /opt/homebrew/Cellar/cmake/3.30.5/bin/cmake -D_CMRC_GENERATE_MODE=TRUE -DNAMESPACE=flycast -DSYMBOL=f_9a8f_fonts_printer_ascii12x24_bin -DINPUT_FILE=/Users/jmattiello/Workspace/Provenance/Provenance/Cores/Flycast/flycast/fonts/printer_ascii12x24.bin -DOUTPUT_FILE=/Users/jmattiello/Workspace/Provenance/Provenance/Cores/Flycast/flycast/build/__cmrc_flycast-resources/intermediate/fonts/printer_ascii12x24.bin.cpp -P /Users/jmattiello/Workspace/Provenance/Provenance/Cores/Flycast/flycast/shell/cmake/CMakeRC.cmake
fi
if test "$CONFIGURATION" = "Release"; then :
  cd /Users/jmattiello/Workspace/Provenance/Provenance/Cores/Flycast/flycast/build
  /opt/homebrew/Cellar/cmake/3.30.5/bin/cmake -D_CMRC_GENERATE_MODE=TRUE -DNAMESPACE=flycast -DSYMBOL=f_9a8f_fonts_printer_ascii12x24_bin -DINPUT_FILE=/Users/jmattiello/Workspace/Provenance/Provenance/Cores/Flycast/flycast/fonts/printer_ascii12x24.bin -DOUTPUT_FILE=/Users/jmattiello/Workspace/Provenance/Provenance/Cores/Flycast/flycast/build/__cmrc_flycast-resources/intermediate/fonts/printer_ascii12x24.bin.cpp -P /Users/jmattiello/Workspace/Provenance/Provenance/Cores/Flycast/flycast/shell/cmake/CMakeRC.cmake
fi
if test "$CONFIGURATION" = "MinSizeRel"; then :
  cd /Users/jmattiello/Workspace/Provenance/Provenance/Cores/Flycast/flycast/build
  /opt/homebrew/Cellar/cmake/3.30.5/bin/cmake -D_CMRC_GENERATE_MODE=TRUE -DNAMESPACE=flycast -DSYMBOL=f_9a8f_fonts_printer_ascii12x24_bin -DINPUT_FILE=/Users/jmattiello/Workspace/Provenance/Provenance/Cores/Flycast/flycast/fonts/printer_ascii12x24.bin -DOUTPUT_FILE=/Users/jmattiello/Workspace/Provenance/Provenance/Cores/Flycast/flycast/build/__cmrc_flycast-resources/intermediate/fonts/printer_ascii12x24.bin.cpp -P /Users/jmattiello/Workspace/Provenance/Provenance/Cores/Flycast/flycast/shell/cmake/CMakeRC.cmake
fi
if test "$CONFIGURATION" = "RelWithDebInfo"; then :
  cd /Users/jmattiello/Workspace/Provenance/Provenance/Cores/Flycast/flycast/build
  /opt/homebrew/Cellar/cmake/3.30.5/bin/cmake -D_CMRC_GENERATE_MODE=TRUE -DNAMESPACE=flycast -DSYMBOL=f_9a8f_fonts_printer_ascii12x24_bin -DINPUT_FILE=/Users/jmattiello/Workspace/Provenance/Provenance/Cores/Flycast/flycast/fonts/printer_ascii12x24.bin -DOUTPUT_FILE=/Users/jmattiello/Workspace/Provenance/Provenance/Cores/Flycast/flycast/build/__cmrc_flycast-resources/intermediate/fonts/printer_ascii12x24.bin.cpp -P /Users/jmattiello/Workspace/Provenance/Provenance/Cores/Flycast/flycast/shell/cmake/CMakeRC.cmake
fi

