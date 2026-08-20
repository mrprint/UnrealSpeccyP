# Source-based RPM packaging for Unreal Speccy Portable (Fedora) — wxWidgets variant

Analog of the Debian packaging in `build/install/linux-wx/` and the SDL2
RPM packaging in `build/install/fedora/`.

Builds the emulator **from source** using CMake + wxWidgets + OpenAL Soft +
GLEW/OpenGL and produces an RPM.  The resulting package uses a different name
(`unreal-speccy-portable-wx`) and install path (`/usr/lib/unreal-speccy-portable-wx/`)
so it can coexist with `unreal-speccy-portable` (SDL2 variant).

## Quick start (Fedora / RHEL / Rocky / Alma)

From the **root** of the UnrealSpeccyP repository:

```bash
# 1. Install build dependencies
sudo dnf install rpm-build rpmdevtools cmake gcc-c++ make \
    wxGTK3-devel openal-soft-devel glew-devel \
    mesa-libGL-devel mesa-libGLU-devel libX11-devel \
    SDL2-devel zlib-devel libpng-devel \
    desktop-file-utils shared-mime-info libappstream-glib \
    rsync

# Optional: set up standard rpmbuild tree
rpmdev-setuptree

# 2. Build the package
./build/install/fedora-wx/make_rpm.sh
```

The script will:
1. Create a source tarball of the current tree
2. Place it into `~/rpmbuild/SOURCES/`
3. Run `rpmbuild -ba`

Resulting packages appear in:
```
~/rpmbuild/RPMS/<arch>/unreal-speccy-portable-wx-0.0.86.28-1.*.rpm
~/rpmbuild/SRPMS/unreal-speccy-portable-wx-0.0.86.28-1.*.src.rpm
```

Install:
```bash
sudo dnf install ~/rpmbuild/RPMS/*/unreal-speccy-portable-wx-*.rpm
```

## Icons (optional)

Put PNGs into `build/install/linux/icons/` (shared with the SDL2 package):

```
build/install/linux/icons/
├── 16x16/unreal_speccy_portable.png
├── 22x22/unreal_speccy_portable.png
...
└── 128x128/unreal_speccy_portable.png
```

## Install layout (classic, same as deb)

```
/usr/bin/unreal-speccy-portable-wx              # launcher script
/usr/lib/unreal-speccy-portable-wx/
├── unreal_speccy_portable                       # real binary
└── res/                                         # ROMs + fonts
```

## Files

```
build/install/fedora-wx/
├── make_rpm.sh
├── README.md
├── unreal-speccy-portable-wx.spec
├── unreal-speccy-portable-wx.launcher
├── unreal-speccy-portable-wx.desktop
└── unreal_speccy_portable_wx.xml
```

## Notes

- CMake source dir: `build/cmake/`
- Build flags (matching the Debian wxWidgets variant):
  `-DUSE_WX_WIDGETS=ON -DUSE_SDL=0 -DUSE_SDL2=0 -DUSE_SYS_LIBS=ON`
- MIME types and `.desktop` match the Debian packaging (with `_wx` suffix)
- Works on Fedora, RHEL, Rocky, Alma Linux; on openSUSE use `zypper` equivalents
