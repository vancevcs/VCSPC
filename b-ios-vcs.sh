#!/bin/bash
# iOS build for the VCS fork: an unsigned device build, for sideloading.
#
# Upstream's b-ios.sh is its CI's: it also makes a jailbreak .deb, chowns with sudo and fake-signs
# with ldid. This only builds. The app lands at build-ios/Release-iphoneos/PPSSPP.app, and
# `python3 Tools/vcspackage.py --ios --zip` turns it into the .ipa - which is also where it is
# named, given its icon and launch screen, and signed, the same split b-macos.sh and the Mac
# package make. Re-running only rebuilds what changed.
set -e
cd "$(dirname "$0")"
ROOT="$PWD"

# Same as b-macos.sh: cmake from PyPI in .venv rather than Homebrew.
if [ ! -x .venv/bin/cmake ]; then
	python3 -m venv .venv
	.venv/bin/pip install --quiet --upgrade pip
	.venv/bin/pip install --quiet cmake ninja
fi
export PATH="$ROOT/.venv/bin:$PATH"

mkdir -p build-ios
cd build-ios
# Listed as a source at configure time, so it has to exist before Xcode runs any script phase.
echo "const char *PPSSPP_GIT_VERSION = \"$(git describe --always)\";" > git-version.cpp
echo "#define PPSSPP_GIT_VERSION_NO_UPDATE 1" >> git-version.cpp

# Every time, not only the first. The project's own ZERO_CHECK step regenerates it after a
# CMakeLists change, but only once xcodebuild has already loaded the old one - so a changed compile
# flag otherwise takes effect a build late, with this one recompiling nothing.
cmake -DCMAKE_TOOLCHAIN_FILE=../cmake/Toolchains/ios.cmake -GXcode .. > /dev/null
xcodebuild -project PPSSPP.xcodeproj -scheme PPSSPP -sdk iphoneos -configuration Release build \
	CODE_SIGNING_REQUIRED=NO CODE_SIGNING_ALLOWED=NO -quiet
echo "Built build-ios/Release-iphoneos/PPSSPP.app"
