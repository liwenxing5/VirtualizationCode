Name:       qemu		
Version:    2.8.0	
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
./configure  --prefix=%{buildroot} --extra-cflags=-lrt --extra-cflags=-lm --target-list=x86_64-softmmu --enable-debug --enable-kvm     --enable-vnc
make -j 16


%install
make install


%files
%doc
/bin/*
/var/*
/libexec/*
/share/*


%changelog

