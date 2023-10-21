Name:       dpdk		
Version:    22.11	
Release:    3%{?dist}
Summary:    my dpdk

Group:      x86
License:    GPLv2
URL:	    git://dpdk.org/dpdk-stable
Source0:    %{name}-%{version}.tar.gz

BuildRequires:	python3 meson ninja-build

%description
just for test once

%prep
%setup -q


%build
#pip3 install pyelftools
meson  --buildtype debug --prefix=%{buildroot}/usr/ build
cd build
ninja -j 16


%install
ninja install -C build
%files
%doc
/usr/include/*
/usr/share/*
/usr/bin/*
/usr/kernel/*
/usr/lib64/*
%changelog

