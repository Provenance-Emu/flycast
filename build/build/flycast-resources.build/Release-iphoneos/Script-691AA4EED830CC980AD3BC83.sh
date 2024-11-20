#!/bin/sh
set -e
if test "$CONFIGURATION" = "Debug"; then :
  cd /Users/jmattiello/Workspace/Provenance/Provenance/Cores/Flycast/flycast/build
  /opt/homebrew/Cellar/cmake/3.30.5/bin/cmake -E copy_if_different /Users/jmattiello/Workspace/Provenance/Provenance/Cores/Flycast/flycast/build/__cmrc_flycast-resources/lib_.cpp /Users/jmattiello/Workspace/Provenance/Provenance/Cores/Flycast/flycast/build/__cmrc_flycast-resources/lib.cpp
fi
if test "$CONFIGURATION" = "Release"; then :
  cd /Users/jmattiello/Workspace/Provenance/Provenance/Cores/Flycast/flycast/build
  /opt/homebrew/Cellar/cmake/3.30.5/bin/cmake -E copy_if_different /Users/jmattiello/Workspace/Provenance/Provenance/Cores/Flycast/flycast/build/__cmrc_flycast-resources/lib_.cpp /Users/jmattiello/Workspace/Provenance/Provenance/Cores/Flycast/flycast/build/__cmrc_flycast-resources/lib.cpp
fi
if test "$CONFIGURATION" = "MinSizeRel"; then :
  cd /Users/jmattiello/Workspace/Provenance/Provenance/Cores/Flycast/flycast/build
  /opt/homebrew/Cellar/cmake/3.30.5/bin/cmake -E copy_if_different /Users/jmattiello/Workspace/Provenance/Provenance/Cores/Flycast/flycast/build/__cmrc_flycast-resources/lib_.cpp /Users/jmattiello/Workspace/Provenance/Provenance/Cores/Flycast/flycast/build/__cmrc_flycast-resources/lib.cpp
fi
if test "$CONFIGURATION" = "RelWithDebInfo"; then :
  cd /Users/jmattiello/Workspace/Provenance/Provenance/Cores/Flycast/flycast/build
  /opt/homebrew/Cellar/cmake/3.30.5/bin/cmake -E copy_if_different /Users/jmattiello/Workspace/Provenance/Provenance/Cores/Flycast/flycast/build/__cmrc_flycast-resources/lib_.cpp /Users/jmattiello/Workspace/Provenance/Provenance/Cores/Flycast/flycast/build/__cmrc_flycast-resources/lib.cpp
fi

