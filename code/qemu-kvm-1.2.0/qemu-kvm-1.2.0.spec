Name:       qemu-kvm		
Version:    1.2.0	
Release:    1%{?dist}
Summary:    my qemu

Group:      x86
License:    GPLv2
URL:	    https://sourceforge.net/projects/kvm/files/
Source0:    %{name}-%{version}.tar.gz

BuildRequires:	zlib zlib-devel glib2 glib2-devel kernel-devel

%description
just for test once

%prep
%setup -q


%build
./configure  --prefix=%{buildroot} --extra-cflags=-lrt --extra-cflags=-lm
make -j 16


%install
make install


%files
%doc
/bin/*
/etc/*
/libexec/*


%changelog

