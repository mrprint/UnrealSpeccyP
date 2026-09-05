Name:           unreal-speccy-portable-sdl2-desktop
Version:        0.0.86.28
Release:        1%{?dist}
Summary:        Portable ZX Spectrum emulator (SDL2 desktop OpenGL + Dear ImGui UI)

License:        GPL-3.0-or-later
URL:            https://github.com/djdron/UnrealSpeccyP
Source0:        %{name}-%{version}.tar.gz

BuildRequires:  cmake >= 3.5
BuildRequires:  gcc-c++
BuildRequires:  pkgconfig(sdl2)
BuildRequires:  glew-devel
BuildRequires:  mesa-libGL-devel
BuildRequires:  libX11-devel
BuildRequires:  zlib-devel
BuildRequires:  libpng-devel
BuildRequires:  minizip-ng-compat-devel
BuildRequires:  tinyxml2-devel
BuildRequires:  desktop-file-utils
BuildRequires:  shared-mime-info
BuildRequires:  libappstream-glib

Requires:       SDL2
Requires:       glew
Requires:       mesa-libGL
Requires:       libX11
Requires:       zlib
Requires:       libpng

%description
Portable ZX-Spectrum emulator based on UnrealSpeccy by SMT.
Supports Z80 128K (Pentagon), AY/YM, Beeper, Beta Disk, Tape,
Kempston Joystick/Mouse, Snapshots and Replays.

Supported formats: sna, z80, szx, rzx, tap, tzx, csw,
trd, scl, fdi, td0, udi, zip.

Built from source using CMake + SDL2 + desktop OpenGL + Dear ImGui
(sdl2_desktop platform). This variant uses an SDL2 + desktop OpenGL +
Dear ImGui UI instead of the SDL2 GLESv2 or wxWidgets-based UIs.
Can be installed alongside unreal-speccy-portable (SDL2) and
unreal-speccy-portable-wx (wxWidgets).

# Layout (same as classic deb packaging):
#   /usr/bin/unreal-speccy-portable-sdl2-desktop          — launcher script
#   /usr/lib/unreal-speccy-portable-sdl2-desktop/         — binary + res/

%prep
%autosetup -n %{name}-%{version}

%build
%cmake -S build/cmake \
    -DUSE_SDL=OFF \
    -DUSE_SDL2=OFF \
    -DUSE_WX_WIDGETS=OFF \
    -DUSE_SDL2_DESKTOP=ON \
    -DUSE_SYS_LIBS=ON \
    -DUSE_BENCHMARK=OFF \
    -DUSE_LIBRARY=OFF \
    -DCMAKE_BUILD_TYPE=Release
%cmake_build

%install
%cmake_install

LIBDIR=%{buildroot}/usr/lib/unreal-speccy-portable-sdl2-desktop
mkdir -p ${LIBDIR}

# Binary -> /usr/lib/unreal-speccy-portable-sdl2-desktop/unreal_speccy_portable
if [ -f %{buildroot}%{_bindir}/unreal_speccy_portable ]; then
    mv %{buildroot}%{_bindir}/unreal_speccy_portable \
       ${LIBDIR}/unreal_speccy_portable
elif [ -f %{buildroot}%{_bindir}/unreal-speccy-portable ]; then
    mv %{buildroot}%{_bindir}/unreal-speccy-portable \
       ${LIBDIR}/unreal_speccy_portable
elif [ -f %{_vpath_builddir}/unreal_speccy_portable ]; then
    install -m 755 %{_vpath_builddir}/unreal_speccy_portable \
        ${LIBDIR}/unreal_speccy_portable
else
    echo "ERROR: built binary not found" >&2
    exit 1
fi

# res/ (ROMs, fonts) next to the binary
cp -a res ${LIBDIR}/

# Launcher script
install -D -m 755 build/install/fedora-sdl2-desktop/unreal-speccy-portable-sdl2-desktop.launcher \
    %{buildroot}%{_bindir}/unreal-speccy-portable-sdl2-desktop

# Desktop entry
install -D -m 644 build/install/fedora-sdl2-desktop/unreal-speccy-portable-sdl2-desktop.desktop \
    %{buildroot}%{_datadir}/applications/unreal-speccy-portable-sdl2-desktop.desktop

# MIME types
install -D -m 644 build/install/fedora-sdl2-desktop/unreal_speccy_portable_sdl2_desktop.xml \
    %{buildroot}%{_datadir}/mime/packages/unreal_speccy_portable_sdl2_desktop.xml

# Icons (optional) — shared tree build/install/linux/icons/<WxH>/
if [ -d build/install/linux/icons ]; then
    for dir in build/install/linux/icons/*/ ; do
        [ -d "$dir" ] || continue
        size=$(basename "$dir")
        f="$dir/unreal_speccy_portable.png"
        if [ -f "$f" ]; then
            install -D -m 644 "$f" \
                %{buildroot}%{_datadir}/icons/hicolor/$size/apps/unreal_speccy_portable_sdl2_desktop.png
        fi
    done
fi

%check
desktop-file-validate %{buildroot}%{_datadir}/applications/unreal-speccy-portable-sdl2-desktop.desktop || true

%files
%license LICENSE
%doc README.md
%{_bindir}/unreal-speccy-portable-sdl2-desktop
/usr/lib/unreal-speccy-portable-sdl2-desktop/
%{_datadir}/applications/unreal-speccy-portable-sdl2-desktop.desktop
%{_datadir}/mime/packages/unreal_speccy_portable_sdl2_desktop.xml
%{_datadir}/icons/hicolor/*/apps/unreal_speccy_portable_sdl2_desktop.png

%changelog
* Fri Jul 24 2026 djdron <djdron@gmail.com> - 0.0.86.28-1
- Source-based RPM packaging for the SDL2 + desktop OpenGL + Dear ImGui UI variant
- Binary + res/ under /usr/lib/unreal-speccy-portable-sdl2-desktop/
- Launcher script in /usr/bin (classic layout)
