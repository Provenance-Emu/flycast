#!/bin/sh
set -e
if test "$CONFIGURATION" = "Debug"; then :
  cd /Users/jmattiello/Workspace/Provenance/Provenance/Cores/Flycast/flycast/build
  /opt/homebrew/Cellar/cmake/3.30.5/bin/cmake -D_CMRC_GENERATE_MODE=TRUE -DNAMESPACE=flycast -DSYMBOL=f_53d3_fonts_fa_solid_900_ttf_zip -DINPUT_FILE=/Users/jmattiello/Workspace/Provenance/Provenance/Cores/Flycast/flycast/fonts/fa-solid-900.ttf.zip -DOUTPUT_FILE=/Users/jmattiello/Workspace/Provenance/Provenance/Cores/Flycast/flycast/build/__cmrc_flycast-resources/intermediate/fonts/fa-solid-900.ttf.zip.cpp -P /Users/jmattiello/Workspace/Provenance/Provenance/Cores/Flycast/flycast/shell/cmake/CMakeRC.cmake
fi
if test "$CONFIGURATION" = "Release"; then :
  cd /Users/jmattiello/Workspace/Provenance/Provenance/Cores/Flycast/flycast/build
  /opt/homebrew/Cellar/cmake/3.30.5/bin/cmake -D_CMRC_GENERATE_MODE=TRUE -DNAMESPACE=flycast -DSYMBOL=f_53d3_fonts_fa_solid_900_ttf_zip -DINPUT_FILE=/Users/jmattiello/Workspace/Provenance/Provenance/Cores/Flycast/flycast/fonts/fa-solid-900.ttf.zip -DOUTPUT_FILE=/Users/jmattiello/Workspace/Provenance/Provenance/Cores/Flycast/flycast/build/__cmrc_flycast-resources/intermediate/fonts/fa-solid-900.ttf.zip.cpp -P /Users/jmattiello/Workspace/Provenance/Provenance/Cores/Flycast/flycast/shell/cmake/CMakeRC.cmake
fi
if test "$CONFIGURATION" = "MinSizeRel"; then :
  cd /Users/jmattiello/Workspace/Provenance/Provenance/Cores/Flycast/flycast/build
  /opt/homebrew/Cellar/cmake/3.30.5/bin/cmake -D_CMRC_GENERATE_MODE=TRUE -DNAMESPACE=flycast -DSYMBOL=f_53d3_fonts_fa_solid_900_ttf_zip -DINPUT_FILE=/Users/jmattiello/Workspace/Provenance/Provenance/Cores/Flycast/flycast/fonts/fa-solid-900.ttf.zip -DOUTPUT_FILE=/Users/jmattiello/Workspace/Provenance/Provenance/Cores/Flycast/flycast/build/__cmrc_flycast-resources/intermediate/fonts/fa-solid-900.ttf.zip.cpp -P /Users/jmattiello/Workspace/Provenance/Provenance/Cores/Flycast/flycast/shell/cmake/CMakeRC.cmake
fi
if test "$CONFIGURATION" = "RelWithDebInfo"; then :
  cd /Users/jmattiello/Workspace/Provenance/Provenance/Cores/Flycast/flycast/build
  /opt/homebrew/Cellar/cmake/3.30.5/bin/cmake -D_CMRC_GENERATE_MODE=TRUE -DNAMESPACE=flycast -DSYMBOL=f_53d3_fonts_fa_solid_900_ttf_zip -DINPUT_FILE=/Users/jmattiello/Workspace/Provenance/Provenance/Cores/Flycast/flycast/fonts/fa-solid-900.ttf.zip -DOUTPUT_FILE=/Users/jmattiello/Workspace/Provenance/Provenance/Cores/Flycast/flycast/build/__cmrc_flycast-resources/intermediate/fonts/fa-solid-900.ttf.zip.cpp -P /Users/jmattiello/Workspace/Provenance/Provenance/Cores/Flycast/flycast/shell/cmake/CMakeRC.cmake
fi

