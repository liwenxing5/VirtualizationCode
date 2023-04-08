Name:       dpdk		
Version:    20.11	
Release:    1%{?dist}
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
meson build --prefix=%{buildroot}
cd build
ninja -j 16


%install
ninja install -C build


%files
%doc
/bin/*
/lib64/*
/include/*
/share/*

%changelog

