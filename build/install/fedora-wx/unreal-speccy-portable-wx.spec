Name:           unreal-speccy-portable-wx
Version:        0.0.86.28
Release:        1%{?dist}
Summary:        Portable ZX Spectrum emulator (wxWidgets desktop UI)

License:        GPL-3.0-or-later
URL:            https://github.com/djdron/UnrealSpeccyP
Source0:        %{name}-%{version}.tar.gz

BuildRequires:  cmake >= 3.5
BuildRequires:  gcc-c++
BuildRequires:  make
BuildRequires:  wxGTK-devel
BuildRequires:  openal-soft-devel
BuildRequires:  glew-devel
BuildRequires:  mesa-libGL-devel
BuildRequires:  mesa-libGLU-devel
BuildRequires:  libX11-devel
BuildRequires:  SDL2-devel
BuildRequires:  zlib-devel
BuildRequires:  libpng-devel
BuildRequires:  minizip-ng-compat-devel
BuildRequires:  tinyxml2-devel
BuildRequires:  desktop-file-utils
BuildRequires:  shared-mime-info
BuildRequires:  libappstream-glib

Requires:       wxGTK
Requires:       openal-soft
Requires:       glew
Requires:       mesa-libGL
Requires:       mesa-libGLU
Requires:       SDL2
Requires:       zlib
Requires:       libpng

%description
Portable ZX-Spectrum emulator based on UnrealSpeccy by SMT.
Supports Z80 128K (Pentagon), AY/YM, Beeper, Beta Disk, Tape,
Kempston Joystick/Mouse, Snapshots and Replays.

Supported formats: sna, z80, szx, rzx, tap, tzx, csw,
trd, scl, fdi, td0, udi, zip.

Built from source using CMake + wxWidgets (native desktop UI).
This variant uses a native wxWidgets desktop UI with OpenAL Soft
audio and OpenGL/GLEW rendering instead of the SDL2-based UI.
Can be installed alongside unreal-speccy-portable (SDL2).

# Layout (same as classic deb packaging):
#   /usr/bin/unreal-speccy-portable-wx          — launcher script
#   /usr/lib/unreal-speccy-portable-wx/         — binary + res/

%prep
%autosetup -n %{name}-%{version}

%build
%cmake -S build/cmake \
    -DUSE_SDL=0 \
    -DUSE_SDL2=0 \
    -DUSE_WX_WIDGETS=ON \
    -DUSE_SYS_LIBS=ON \
    -DUSE_BENCHMARK=OFF \
    -DUSE_LIBRARY=OFF \
    -DCMAKE_BUILD_TYPE=Release
%cmake_build

%install
%cmake_install

LIBDIR=%{buildroot}/usr/lib/unreal-speccy-portable-wx
mkdir -p ${LIBDIR}

# Binary -> /usr/lib/unreal-speccy-portable-wx/unreal_speccy_portable
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
install -D -m 755 build/install/fedora-wx/unreal-speccy-portable-wx.launcher \
    %{buildroot}%{_bindir}/unreal-speccy-portable-wx

# Desktop entry
install -D -m 644 build/install/fedora-wx/unreal-speccy-portable-wx.desktop \
    %{buildroot}%{_datadir}/applications/unreal-speccy-portable-wx.desktop

# MIME types
install -D -m 644 build/install/fedora-wx/unreal_speccy_portable_wx.xml \
    %{buildroot}%{_datadir}/mime/packages/unreal_speccy_portable_wx.xml

# Icons (optional) — shared tree build/install/linux/icons/<WxH>/
if [ -d build/install/linux/icons ]; then
    for dir in build/install/linux/icons/*/ ; do
        [ -d "$dir" ] || continue
        size=$(basename "$dir")
        f="$dir/unreal_speccy_portable.png"
        if [ -f "$f" ]; then
            install -D -m 644 "$f" \
                %{buildroot}%{_datadir}/icons/hicolor/$size/apps/unreal_speccy_portable_wx.png
        fi
    done
fi

%check
desktop-file-validate %{buildroot}%{_datadir}/applications/unreal-speccy-portable-wx.desktop || true

%files
%license LICENSE
%doc README.md
%{_bindir}/unreal-speccy-portable-wx
/usr/lib/unreal-speccy-portable-wx/
%{_datadir}/applications/unreal-speccy-portable-wx.desktop
%{_datadir}/mime/packages/unreal_speccy_portable_wx.xml
%{_datadir}/icons/hicolor/*/apps/unreal_speccy_portable_wx.png

%changelog
* Fri Jul 24 2026 djdron <djdron@gmail.com> - 0.0.86.28-1
- Source-based RPM packaging for the wxWidgets desktop UI variant
- Binary + res/ under /usr/lib/unreal-speccy-portable-wx/
- Launcher script in /usr/bin (classic layout)
