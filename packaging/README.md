# Packaging

Linux packages are built from a Release build by:

```bash
./packaging/linux/make-packages.sh
```

Outputs land in `dist/` (ignored by git):

- `vkq3ng_<version>-<release>_amd64.deb`
- `vkq3ng-<version>-<release>.x86_64.rpm`
- `vkq3ng-<version>-linux-x86_64.tar.gz`
- `SHA256SUMS`

Useful overrides:

```bash
BUILD=0 ./packaging/linux/make-packages.sh   # use existing bin/Release
VERSION=1.0.1 RELEASE=2 ./packaging/linux/make-packages.sh
OUT=/tmp/out ./packaging/linux/make-packages.sh
```

The system packages install the engine under `/usr/lib/vkq3ng/bin`, the
launcher as `/usr/bin/vkq3ng`, and the CC0 blue-noise textures under
`/usr/share/vkq3ng/baseq3`.  Quake III Arena data is not included; put it
there or point `Q3_BASE` at another installation.

The RPM is generated from `packaging/linux/vkq3ng.spec.in`; the Debian
control file is `packaging/linux/debian/control.in`.
