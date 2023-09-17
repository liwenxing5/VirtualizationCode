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
meson --prefix=%{buildroot} -DCONFIG_RTE_LIBRTE_VHOST_NUMA=y build
cd build
ninja -j 16


%install
ninja install -C build
mv %{buildroot}/lib64/dpdk/pmds-23.0/* %{buildroot}/lib64
rm -rf %{buildroot}/lib64/dpdk
%files
%doc
/include/*
/share/*
/bin/*
/kernel/*
/lib64/*
%changelog

