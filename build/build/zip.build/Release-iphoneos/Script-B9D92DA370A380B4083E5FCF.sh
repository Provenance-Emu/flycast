#!/bin/sh
set -e
if test "$CONFIGURATION" = "Debug"; then :
  cd /Users/jmattiello/Workspace/Provenance/Provenance/Cores/Flycast/flycast/build/core/deps/libzip/lib
  /opt/homebrew/Cellar/cmake/3.30.5/bin/cmake -DPROJECT_SOURCE_DIR=/Users/jmattiello/Workspace/Provenance/Provenance/Cores/Flycast/flycast/core/deps/libzip -DCMAKE_CURRENT_BINARY_DIR=/Users/jmattiello/Workspace/Provenance/Provenance/Cores/Flycast/flycast/build/core/deps/libzip/lib -P /Users/jmattiello/Workspace/Provenance/Provenance/Cores/Flycast/flycast/core/deps/libzip/cmake/GenerateZipErrorStrings.cmake
fi
if test "$CONFIGURATION" = "Release"; then :
  cd /Users/jmattiello/Workspace/Provenance/Provenance/Cores/Flycast/flycast/build/core/deps/libzip/lib
  /opt/homebrew/Cellar/cmake/3.30.5/bin/cmake -DPROJECT_SOURCE_DIR=/Users/jmattiello/Workspace/Provenance/Provenance/Cores/Flycast/flycast/core/deps/libzip -DCMAKE_CURRENT_BINARY_DIR=/Users/jmattiello/Workspace/Provenance/Provenance/Cores/Flycast/flycast/build/core/deps/libzip/lib -P /Users/jmattiello/Workspace/Provenance/Provenance/Cores/Flycast/flycast/core/deps/libzip/cmake/GenerateZipErrorStrings.cmake
fi
if test "$CONFIGURATION" = "MinSizeRel"; then :
  cd /Users/jmattiello/Workspace/Provenance/Provenance/Cores/Flycast/flycast/build/core/deps/libzip/lib
  /opt/homebrew/Cellar/cmake/3.30.5/bin/cmake -DPROJECT_SOURCE_DIR=/Users/jmattiello/Workspace/Provenance/Provenance/Cores/Flycast/flycast/core/deps/libzip -DCMAKE_CURRENT_BINARY_DIR=/Users/jmattiello/Workspace/Provenance/Provenance/Cores/Flycast/flycast/build/core/deps/libzip/lib -P /Users/jmattiello/Workspace/Provenance/Provenance/Cores/Flycast/flycast/core/deps/libzip/cmake/GenerateZipErrorStrings.cmake
fi
if test "$CONFIGURATION" = "RelWithDebInfo"; then :
  cd /Users/jmattiello/Workspace/Provenance/Provenance/Cores/Flycast/flycast/build/core/deps/libzip/lib
  /opt/homebrew/Cellar/cmake/3.30.5/bin/cmake -DPROJECT_SOURCE_DIR=/Users/jmattiello/Workspace/Provenance/Provenance/Cores/Flycast/flycast/core/deps/libzip -DCMAKE_CURRENT_BINARY_DIR=/Users/jmattiello/Workspace/Provenance/Provenance/Cores/Flycast/flycast/build/core/deps/libzip/lib -P /Users/jmattiello/Workspace/Provenance/Provenance/Cores/Flycast/flycast/core/deps/libzip/cmake/GenerateZipErrorStrings.cmake
fi

