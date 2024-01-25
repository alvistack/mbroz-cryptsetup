# Copyright 2024 Wong Hoi Sing Edison <hswong3i@pantarei-design.com>
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

%global debug_package %{nil}

%global source_date_epoch_from_changelog 0

%global _lto_cflags %{?_lto_cflags} -ffat-lto-objects

Name: cryptsetup
Epoch: 100
Version: 2.7.4
Release: 1%{?dist}
Summary: Utility for setting up encrypted disks
License: GPL-2.0-or-later
URL: https://github.com/mbroz/cryptsetup/tags
Source0: %{name}_%{version}.orig.tar.gz
BuildRequires: autoconf
BuildRequires: automake
BuildRequires: device-mapper-devel
BuildRequires: findutils
BuildRequires: gcc
BuildRequires: gettext-devel
BuildRequires: json-c-devel
BuildRequires: libblkid-devel
BuildRequires: libssh-devel
BuildRequires: libtool
BuildRequires: libuuid-devel
BuildRequires: make
BuildRequires: openssl-devel
BuildRequires: pkgconfig
BuildRequires: popt-devel

%description
The cryptsetup package contains a utility for setting up disk encryption
using dm-crypt kernel module.

%prep
%autosetup -T -c -n %{name}_%{version}-%{release}
tar -zx -f %{S:0} --strip-components=1 -C .

%build
./autogen.sh
%configure \
    --disable-asciidoc \
    --disable-cryptsetup-reencrypt \
    --enable-shared \
    --with-tmpfilesdir=%{_tmpfilesdir}
%make_build

%install
%make_install
rm -rf %{buildroot}%{_mandir}
rm -rf %{buildroot}%{_libdir}/*.la
rm -rf %{buildroot}%{_libdir}/*/*.la

%check

%if 0%{?suse_version} > 1500 || 0%{?sle_version} > 150000
%package -n libcryptsetup-devel
Summary: Headers and libraries for using encrypted file systems
Requires: libcryptsetup12 = %{epoch}:%{version}-%{release}
Requires: pkgconfig

%description -n libcryptsetup-devel
The cryptsetup-devel package contains libraries and header files used
for writing code that makes use of disk encryption.

%package -n libcryptsetup12
Summary: Cryptsetup shared library

%description -n libcryptsetup12
This package contains the cryptsetup shared library, libcryptsetup.

%package -n cryptsetup-ssh
Summary: Cryptsetup LUKS2 SSH token
Requires: libcryptsetup12 = %{epoch}:%{version}-%{release}

%description -n cryptsetup-ssh
This package contains the LUKS2 SSH token.

%post
%{?regenerate_initrd_post}
%tmpfiles_create %{_tmpfilesdir}/cryptsetup.conf

%postun
%{?regenerate_initrd_post}

%posttrans
%{?regenerate_initrd_posttrans}

%post -n libcryptsetup12 -p /sbin/ldconfig
%postun -n libcryptsetup12 -p /sbin/ldconfig

%files
%license COPYING
%dir %{_datadir}/locale/*
%dir %{_datadir}/locale/*/LC_MESSAGES
%{_datadir}/locale/*/LC_MESSAGES/*.mo
%{_sbindir}/cryptsetup
%{_sbindir}/integritysetup
%{_sbindir}/veritysetup
%{_tmpfilesdir}/cryptsetup.conf

%files -n libcryptsetup12
%{_libdir}/libcryptsetup.so.*

%files -n libcryptsetup-devel
%{_includedir}/libcryptsetup.h
%{_libdir}/libcryptsetup.so
%{_libdir}/pkgconfig/*

%files -n cryptsetup-ssh
%dir %{_libdir}/cryptsetup
%{_libdir}/cryptsetup/libcryptsetup-token-ssh.so
%{_sbindir}/cryptsetup-ssh
%endif

%if !(0%{?suse_version} > 1500) && !(0%{?sle_version} > 150000)
%package -n cryptsetup-devel
Summary: Headers and libraries for using encrypted file systems
Requires: cryptsetup-libs = %{epoch}:%{version}-%{release}
Requires: pkgconfig

%description -n cryptsetup-devel
The cryptsetup-devel package contains libraries and header files used
for writing code that makes use of disk encryption.

%package -n cryptsetup-libs
Summary: Cryptsetup shared library

%description -n cryptsetup-libs
This package contains the cryptsetup shared library, libcryptsetup.

%package -n cryptsetup-ssh-token
Summary: Cryptsetup LUKS2 SSH token
Requires: cryptsetup-libs = %{epoch}:%{version}-%{release}

%description -n cryptsetup-ssh-token
This package contains the LUKS2 SSH token.

%package -n veritysetup
Summary: A utility for setting up dm-verity volumes
Requires: cryptsetup-libs = %{epoch}:%{version}-%{release}

%description -n veritysetup
The veritysetup package contains a utility for setting up disk
verification using dm-verity kernel module.

%package -n integritysetup
Summary: A utility for setting up dm-integrity volumes
Requires: cryptsetup-libs = %{epoch}:%{version}-%{release}

%description -n integritysetup
The integritysetup package contains a utility for setting up disk
integrity protection using dm-integrity kernel module.

%post -n cryptsetup-libs -p /sbin/ldconfig
%postun -n cryptsetup-libs -p /sbin/ldconfig

%files
%license COPYING
%{_sbindir}/cryptsetup

%files -n cryptsetup-devel
%{_includedir}/libcryptsetup.h
%{_libdir}/libcryptsetup.so
%{_libdir}/pkgconfig/libcryptsetup.pc

%files -n cryptsetup-libs
%dir %{_datadir}/locale/*
%dir %{_datadir}/locale/*/LC_MESSAGES
%dir %{_libdir}/cryptsetup
%{_datadir}/locale/*/LC_MESSAGES/*.mo
%{_libdir}/libcryptsetup.so.*
%{_tmpfilesdir}/cryptsetup.conf

%files -n cryptsetup-ssh-token
%dir %{_libdir}/cryptsetup
%{_libdir}/cryptsetup/libcryptsetup-token-ssh.so
%{_sbindir}/cryptsetup-ssh

%files -n veritysetup
%{_sbindir}/veritysetup

%files -n integritysetup
%{_sbindir}/integritysetup
%endif

%changelog
