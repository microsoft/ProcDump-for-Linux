#!/bin/sh
set -eu

if [ "$#" -ne 7 ]; then
    echo "Usage: $0 <source-dir> <cargo-target-dir> <package-name> <version> <release> <deb|rpm> <architecture>" >&2
    exit 2
fi

SOURCE_DIR=$1
TARGET_DIR=$2
PACKAGE_NAME=$3
PACKAGE_VERSION=$4
PACKAGE_RELEASE=$5
PACKAGE_TYPE=$6
ARCHITECTURE=$7

case "$PACKAGE_NAME" in
    ''|*[!a-z0-9+.-]*) echo "Invalid package name: $PACKAGE_NAME" >&2; exit 2 ;;
esac
case "$PACKAGE_VERSION" in
    ''|*[!A-Za-z0-9.+~_-]*) echo "Invalid package version: $PACKAGE_VERSION" >&2; exit 2 ;;
esac
case "$PACKAGE_RELEASE" in
    ''|*[!A-Za-z0-9.+~_-]*) echo "Invalid package release: $PACKAGE_RELEASE" >&2; exit 2 ;;
esac
case "$ARCHITECTURE" in
    ''|*[!A-Za-z0-9_+-]*) echo "Invalid package architecture: $ARCHITECTURE" >&2; exit 2 ;;
esac

BINARY="$TARGET_DIR/release/procdump"
MAN_PAGE="$SOURCE_DIR/procdump.1"
PACKAGES_DIR="$TARGET_DIR/packages"

if [ ! -x "$BINARY" ]; then
    echo "Release binary not found: $BINARY" >&2
    exit 1
fi
if [ ! -f "$MAN_PAGE" ]; then
    echo "Manual page not found: $MAN_PAGE" >&2
    exit 1
fi

mkdir -p "$PACKAGES_DIR"

build_deb() {
    if ! command -v dpkg-deb >/dev/null 2>&1; then
        echo "dpkg-deb is required to build Debian packages" >&2
        exit 1
    fi

    package_version="$PACKAGE_VERSION-$PACKAGE_RELEASE"
    package_root="$PACKAGES_DIR/deb/${PACKAGE_NAME}_${package_version}_${ARCHITECTURE}"
    if [ "$PACKAGE_RELEASE" = "0" ]; then
        package_file="$PACKAGES_DIR/${PACKAGE_NAME}_${PACKAGE_VERSION}_${ARCHITECTURE}.deb"
    else
        package_file="$PACKAGES_DIR/${PACKAGE_NAME}_${package_version}_${ARCHITECTURE}.deb"
    fi
    rm -rf "$package_root"
    mkdir -p \
        "$package_root/DEBIAN" \
        "$package_root/usr/bin" \
        "$package_root/usr/share/doc/$PACKAGE_NAME" \
        "$package_root/usr/share/man/man1"

    install -m 0755 "$BINARY" "$package_root/usr/bin/procdump"
    gzip -9 -n -c "$MAN_PAGE" > "$package_root/usr/share/man/man1/procdump.1.gz"
    install -m 0644 "$SOURCE_DIR/LICENSE" "$package_root/usr/share/doc/$PACKAGE_NAME/copyright"
    install -m 0644 "$SOURCE_DIR/NOTICE.txt" "$package_root/usr/share/doc/$PACKAGE_NAME/NOTICE.txt"

    installed_size=$(du -sk "$package_root/usr" | awk '{print $1}')
    cat > "$package_root/DEBIAN/control" <<EOF
Package: $PACKAGE_NAME
Version: $package_version
Section: devel
Priority: optional
Architecture: $ARCHITECTURE
Installed-Size: $installed_size
Maintainer: Sysinternals <syssite@microsoft.com>
Depends: gdb, libc6, libelf1 | libelf1t64, libgcc-s1, libstdc++6, zlib1g, libzstd1
Homepage: https://github.com/microsoft/ProcDump-for-Linux
Description: Sysinternals process dump utility
 ProcDump monitors applications and generates process dumps in response to
 resource, runtime, and signal triggers for postmortem debugging.
EOF

    find "$package_root" -type d -exec chmod 0755 {} +
    chmod 0644 \
        "$package_root/DEBIAN/control" \
        "$package_root/usr/share/man/man1/procdump.1.gz"

    dpkg-deb --build --root-owner-group "$package_root" "$package_file"
    echo "Debian package: $package_file"
}

build_rpm() {
    if ! command -v rpmbuild >/dev/null 2>&1; then
        echo "rpmbuild is required to build RPM packages" >&2
        exit 1
    fi
    case "$PACKAGE_VERSION" in
        *-*) echo "RPM versions cannot contain '-': $PACKAGE_VERSION" >&2; exit 2 ;;
    esac

    top_dir="$PACKAGES_DIR/rpm"
    rm -rf "$top_dir"
    mkdir -p "$top_dir/BUILD" "$top_dir/BUILDROOT" "$top_dir/RPMS" \
        "$top_dir/SOURCES" "$top_dir/SPECS" "$top_dir/SRPMS"
    install -m 0755 "$BINARY" "$top_dir/SOURCES/procdump"
    gzip -9 -n -c "$MAN_PAGE" > "$top_dir/SOURCES/procdump.1.gz"
    install -m 0644 "$SOURCE_DIR/LICENSE" "$top_dir/SOURCES/LICENSE"
    install -m 0644 "$SOURCE_DIR/NOTICE.txt" "$top_dir/SOURCES/NOTICE.txt"

    spec="$top_dir/SPECS/procdump.spec"
    cat > "$spec" <<EOF
Name:           $PACKAGE_NAME
Version:        $PACKAGE_VERSION
Release:        $PACKAGE_RELEASE%{?dist}
Summary:        Sysinternals process dump utility
License:        MIT
URL:            https://github.com/microsoft/ProcDump-for-Linux
Source0:        procdump
Source1:        procdump.1.gz
Source2:        LICENSE
Source3:        NOTICE.txt
BuildArch:      $ARCHITECTURE
Requires:       gdb
Requires:       libstdc++

%description
ProcDump monitors applications and generates process dumps in response to
resource, runtime, and signal triggers for postmortem debugging.

%prep

%build

%install
install -Dpm 0755 %{SOURCE0} %{buildroot}%{_bindir}/procdump
install -Dpm 0644 %{SOURCE1} %{buildroot}%{_mandir}/man1/procdump.1.gz
install -Dpm 0644 %{SOURCE2} %{buildroot}%{_licensedir}/%{name}/LICENSE
install -Dpm 0644 %{SOURCE3} %{buildroot}%{_docdir}/%{name}/NOTICE.txt

%files
%{_bindir}/procdump
%{_mandir}/man1/procdump.1.gz
%license %{_licensedir}/%{name}/LICENSE
%doc %{_docdir}/%{name}/NOTICE.txt

%changelog
EOF

    rpmbuild --define "_topdir $top_dir" --define "_build_id_links none" -bb "$spec"
    for package_file in "$top_dir"/RPMS/*/*.rpm; do
        if [ -f "$package_file" ]; then
            cp "$package_file" "$PACKAGES_DIR/"
            echo "RPM package: $PACKAGES_DIR/$(basename "$package_file")"
        fi
    done
}

case "$PACKAGE_TYPE" in
    deb) build_deb ;;
    rpm) build_rpm ;;
    *) echo "Unsupported package type: $PACKAGE_TYPE" >&2; exit 2 ;;
esac