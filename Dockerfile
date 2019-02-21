# This isn't ideal from a CI perspective because we're lacking pinned versions
# here, but Arch is the only distribution (besides Rawhide) that has new enough
# dependencies. Eventually, when wlroots becomes less bleeding-edge, we can
# switch to other images that provide stable package versions.
FROM archlinux/base:latest

# Install the required packages for compiling wlroots and Cage, and a simple
# xdg-shell-stable application to launch Cage with.
RUN pacman -Syu --noconfirm xorg-server-xvfb awk procps-ng termite git bash clang \
      pkgconf libinput libxkbcommon mesa meson pixman libsystemd wayland \
      wayland-protocols xcb-util-image xcb-util-wm  xcb-util-errors \
  && pacman -Scc --noconfirm

# Install wlroots, which is required by Cage. Note that we compile a tagged
# version, instead of master, to avoid any breaking changes in wlroots.
RUN git clone https://github.com/swaywm/wlroots.git \
  && cd wlroots \
  && git checkout tags/0.4.1 \
  && meson build --prefix=/usr \
                 -Dlibcap=disabled \
                 -Dlogind=enabled \
                 -Dxcb-errors=enabled \
                 -Dxcb-icccm=enabled \
                 -Dxwayland=disabled \
                 -Dx11-backend=enabled \
                 -Drootston=false \
                 -Dexamples=false \
  && ninja -C build \
  && ninja -C build install

# Cage's (or actually, wlroots') logind session expects XDG_RUNTIME_DIR to be
# set.
ENV XDG_RUNTIME_DIR=/tmp

# We have Travis place the clone in this directory, so we need to cd to it.
WORKDIR /root/cage
