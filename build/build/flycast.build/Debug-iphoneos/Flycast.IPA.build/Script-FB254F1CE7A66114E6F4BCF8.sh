#!/bin/sh
set -e
if test "$CONFIGURATION" = "Debug"; then :
  cd /Users/jmattiello/Workspace/Provenance/Provenance/Cores/Flycast/flycast/build
  mkdir -p /Users/jmattiello/Workspace/Provenance/Provenance/Cores/Flycast/flycast/build/Payload
  rm -rf /Users/jmattiello/Workspace/Provenance/Provenance/Cores/Flycast/flycast/build/Payload/*
  cp -r /Users/jmattiello/Workspace/Provenance/Provenance/Cores/Flycast/flycast/build/Debug-iphoneos/Flycast.app /Users/jmattiello/Workspace/Provenance/Provenance/Cores/Flycast/flycast/build/Payload
  for lib in /Users/jmattiello/Workspace/Provenance/Provenance/Cores/Flycast/flycast/build/Payload/Flycast.app/Frameworks/*.dylib
  do xcrun bitcode_strip -r $lib -o $lib
  done
  ldid -S/Users/jmattiello/Workspace/Provenance/Provenance/Cores/Flycast/flycast/shell/apple/emulator-ios/emulator/flycast.entitlements /Users/jmattiello/Workspace/Provenance/Provenance/Cores/Flycast/flycast/build/Payload/Flycast.app/Flycast
  /opt/homebrew/Cellar/cmake/3.30.5/bin/cmake -E tar cfv /Users/jmattiello/Workspace/Provenance/Provenance/Cores/Flycast/flycast/build/Debug-iphoneos/Flycast.ipa --format=zip /Users/jmattiello/Workspace/Provenance/Provenance/Cores/Flycast/flycast/build/Payload
fi
if test "$CONFIGURATION" = "Release"; then :
  cd /Users/jmattiello/Workspace/Provenance/Provenance/Cores/Flycast/flycast/build
  mkdir -p /Users/jmattiello/Workspace/Provenance/Provenance/Cores/Flycast/flycast/build/Payload
  rm -rf /Users/jmattiello/Workspace/Provenance/Provenance/Cores/Flycast/flycast/build/Payload/*
  cp -r /Users/jmattiello/Workspace/Provenance/Provenance/Cores/Flycast/flycast/build/Release-iphoneos/Flycast.app /Users/jmattiello/Workspace/Provenance/Provenance/Cores/Flycast/flycast/build/Payload
  for lib in /Users/jmattiello/Workspace/Provenance/Provenance/Cores/Flycast/flycast/build/Payload/Flycast.app/Frameworks/*.dylib
  do xcrun bitcode_strip -r $lib -o $lib
  done
  ldid -S/Users/jmattiello/Workspace/Provenance/Provenance/Cores/Flycast/flycast/shell/apple/emulator-ios/emulator/flycast.entitlements /Users/jmattiello/Workspace/Provenance/Provenance/Cores/Flycast/flycast/build/Payload/Flycast.app/Flycast
  /opt/homebrew/Cellar/cmake/3.30.5/bin/cmake -E tar cfv /Users/jmattiello/Workspace/Provenance/Provenance/Cores/Flycast/flycast/build/Release-iphoneos/Flycast.ipa --format=zip /Users/jmattiello/Workspace/Provenance/Provenance/Cores/Flycast/flycast/build/Payload
fi
if test "$CONFIGURATION" = "MinSizeRel"; then :
  cd /Users/jmattiello/Workspace/Provenance/Provenance/Cores/Flycast/flycast/build
  mkdir -p /Users/jmattiello/Workspace/Provenance/Provenance/Cores/Flycast/flycast/build/Payload
  rm -rf /Users/jmattiello/Workspace/Provenance/Provenance/Cores/Flycast/flycast/build/Payload/*
  cp -r /Users/jmattiello/Workspace/Provenance/Provenance/Cores/Flycast/flycast/build/MinSizeRel-iphoneos/Flycast.app /Users/jmattiello/Workspace/Provenance/Provenance/Cores/Flycast/flycast/build/Payload
  for lib in /Users/jmattiello/Workspace/Provenance/Provenance/Cores/Flycast/flycast/build/Payload/Flycast.app/Frameworks/*.dylib
  do xcrun bitcode_strip -r $lib -o $lib
  done
  ldid -S/Users/jmattiello/Workspace/Provenance/Provenance/Cores/Flycast/flycast/shell/apple/emulator-ios/emulator/flycast.entitlements /Users/jmattiello/Workspace/Provenance/Provenance/Cores/Flycast/flycast/build/Payload/Flycast.app/Flycast
  /opt/homebrew/Cellar/cmake/3.30.5/bin/cmake -E tar cfv /Users/jmattiello/Workspace/Provenance/Provenance/Cores/Flycast/flycast/build/MinSizeRel-iphoneos/Flycast.ipa --format=zip /Users/jmattiello/Workspace/Provenance/Provenance/Cores/Flycast/flycast/build/Payload
fi
if test "$CONFIGURATION" = "RelWithDebInfo"; then :
  cd /Users/jmattiello/Workspace/Provenance/Provenance/Cores/Flycast/flycast/build
  mkdir -p /Users/jmattiello/Workspace/Provenance/Provenance/Cores/Flycast/flycast/build/Payload
  rm -rf /Users/jmattiello/Workspace/Provenance/Provenance/Cores/Flycast/flycast/build/Payload/*
  cp -r /Users/jmattiello/Workspace/Provenance/Provenance/Cores/Flycast/flycast/build/RelWithDebInfo-iphoneos/Flycast.app /Users/jmattiello/Workspace/Provenance/Provenance/Cores/Flycast/flycast/build/Payload
  for lib in /Users/jmattiello/Workspace/Provenance/Provenance/Cores/Flycast/flycast/build/Payload/Flycast.app/Frameworks/*.dylib
  do xcrun bitcode_strip -r $lib -o $lib
  done
  ldid -S/Users/jmattiello/Workspace/Provenance/Provenance/Cores/Flycast/flycast/shell/apple/emulator-ios/emulator/flycast.entitlements /Users/jmattiello/Workspace/Provenance/Provenance/Cores/Flycast/flycast/build/Payload/Flycast.app/Flycast
  /opt/homebrew/Cellar/cmake/3.30.5/bin/cmake -E tar cfv /Users/jmattiello/Workspace/Provenance/Provenance/Cores/Flycast/flycast/build/RelWithDebInfo-iphoneos/Flycast.ipa --format=zip /Users/jmattiello/Workspace/Provenance/Provenance/Cores/Flycast/flycast/build/Payload
fi

