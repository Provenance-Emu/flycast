#!/bin/sh
set -e
if test "$CONFIGURATION" = "Debug"; then :
  cd /Users/jmattiello/Workspace/Provenance/Provenance/Cores/Flycast/flycast/build
  /opt/homebrew/Cellar/cmake/3.30.5/bin/cmake -D_CMRC_GENERATE_MODE=TRUE -DNAMESPACE=flycast -DSYMBOL=f_1289_picture_f355_print_template_png -DINPUT_FILE=/Users/jmattiello/Workspace/Provenance/Provenance/Cores/Flycast/flycast/resources/picture/f355_print_template.png -DOUTPUT_FILE=/Users/jmattiello/Workspace/Provenance/Provenance/Cores/Flycast/flycast/build/__cmrc_flycast-resources/intermediate/picture/f355_print_template.png.cpp -P /Users/jmattiello/Workspace/Provenance/Provenance/Cores/Flycast/flycast/shell/cmake/CMakeRC.cmake
fi
if test "$CONFIGURATION" = "Release"; then :
  cd /Users/jmattiello/Workspace/Provenance/Provenance/Cores/Flycast/flycast/build
  /opt/homebrew/Cellar/cmake/3.30.5/bin/cmake -D_CMRC_GENERATE_MODE=TRUE -DNAMESPACE=flycast -DSYMBOL=f_1289_picture_f355_print_template_png -DINPUT_FILE=/Users/jmattiello/Workspace/Provenance/Provenance/Cores/Flycast/flycast/resources/picture/f355_print_template.png -DOUTPUT_FILE=/Users/jmattiello/Workspace/Provenance/Provenance/Cores/Flycast/flycast/build/__cmrc_flycast-resources/intermediate/picture/f355_print_template.png.cpp -P /Users/jmattiello/Workspace/Provenance/Provenance/Cores/Flycast/flycast/shell/cmake/CMakeRC.cmake
fi
if test "$CONFIGURATION" = "MinSizeRel"; then :
  cd /Users/jmattiello/Workspace/Provenance/Provenance/Cores/Flycast/flycast/build
  /opt/homebrew/Cellar/cmake/3.30.5/bin/cmake -D_CMRC_GENERATE_MODE=TRUE -DNAMESPACE=flycast -DSYMBOL=f_1289_picture_f355_print_template_png -DINPUT_FILE=/Users/jmattiello/Workspace/Provenance/Provenance/Cores/Flycast/flycast/resources/picture/f355_print_template.png -DOUTPUT_FILE=/Users/jmattiello/Workspace/Provenance/Provenance/Cores/Flycast/flycast/build/__cmrc_flycast-resources/intermediate/picture/f355_print_template.png.cpp -P /Users/jmattiello/Workspace/Provenance/Provenance/Cores/Flycast/flycast/shell/cmake/CMakeRC.cmake
fi
if test "$CONFIGURATION" = "RelWithDebInfo"; then :
  cd /Users/jmattiello/Workspace/Provenance/Provenance/Cores/Flycast/flycast/build
  /opt/homebrew/Cellar/cmake/3.30.5/bin/cmake -D_CMRC_GENERATE_MODE=TRUE -DNAMESPACE=flycast -DSYMBOL=f_1289_picture_f355_print_template_png -DINPUT_FILE=/Users/jmattiello/Workspace/Provenance/Provenance/Cores/Flycast/flycast/resources/picture/f355_print_template.png -DOUTPUT_FILE=/Users/jmattiello/Workspace/Provenance/Provenance/Cores/Flycast/flycast/build/__cmrc_flycast-resources/intermediate/picture/f355_print_template.png.cpp -P /Users/jmattiello/Workspace/Provenance/Provenance/Cores/Flycast/flycast/shell/cmake/CMakeRC.cmake
fi

