# Source this file from bash or zsh before using the Mac development toolchain.
# Apple Silicon Homebrew is the default; Intel/custom installations can override it.
export VTRPON_HOMEBREW_PREFIX="${VTRPON_HOMEBREW_PREFIX:-/opt/homebrew}"
export PATH="$VTRPON_HOMEBREW_PREFIX/opt/qtbase/bin:$VTRPON_HOMEBREW_PREFIX/opt/qttools/bin:$VTRPON_HOMEBREW_PREFIX/bin:$PATH"
export CMAKE_PREFIX_PATH="$VTRPON_HOMEBREW_PREFIX:$VTRPON_HOMEBREW_PREFIX/opt/zlib${CMAKE_PREFIX_PATH:+:$CMAKE_PREFIX_PATH}"
export PKG_CONFIG_PATH="$VTRPON_HOMEBREW_PREFIX/opt/gstreamer/lib/pkgconfig:$VTRPON_HOMEBREW_PREFIX/opt/zlib/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
export CMAKE_BUILD_PARALLEL_LEVEL="${CMAKE_BUILD_PARALLEL_LEVEL:-2}"
