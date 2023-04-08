Name:       qemu		
Version:    2.0.0	
Release:    1%{?dist}
Summary:    my qemu

Group:      x86
License:    GPLv2
URL:	    https://sourceforge.net/projects/kvm/files/
Source0:    %{name}-%{version}.tar.gz

BuildRequires:	zlib zlib-devel glib2 glib2-devel kernel-devel libfdt-devel

%description
just for test once

%prep
%setup -q


%build
./configure  --prefix=%{buildroot} --extra-cflags=-lrt --extra-cflags=-lm --target-list=x86_64-softmmu --enable-debug --enable-kvm --enable-vnc --disable-werror
make -j 16


%install
make install
install -d %{buildroot}/usr/bin
install -d %{buildroot}/usr/libexec
install -p -D -m 0755 %{buildroot}/bin/* %{buildroot}/usr/bin
install -p -D -m 0755 %{buildroot}/libexec/* %{buildroot}/usr/libexec
mv %{buildroot}/share/ %{buildroot}/usr/share
rm -rf  %{buildroot}/bin/
rm -rf  %{buildroot}/libexec

%files
%doc
/etc/*
/usr/bin/*
/usr/libexec/*
/usr/share/*
/var/*


%changelog

