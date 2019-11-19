
%undefine		_hardened_build

Name:		libntirpc
Version:	@NTIRPC_VERSION@
Release:	1%{?dev:%{dev}}%{?dist}
Summary:	New Transport Independent RPC Library
Group:		System Environment/Libraries
License:	BSD
Url:		https://github.com/nfs-ganesha/ntirpc

Source0:	https://github.com/nfs-ganesha/ntirpc/archive/v%{version}/ntirpc-%{version}.tar.gz

BuildRequires:	cmake
BuildRequires:	krb5-devel
# libtirpc has /etc/netconfig, most machines probably have it anyway
# for NFS client
Requires:	libtirpc

%description
This package contains a new implementation of the original libtirpc,
transport-independent RPC (TI-RPC) library for NFS-Ganesha. It has
the following features not found in libtirpc:
 1. Bi-directional operation
 2. Full-duplex operation on the TCP (vc) transport
 3. Thread-safe operating modes
 3.1 new locking primitives and lock callouts (interface change)
 3.2 stateless send/recv on the TCP transport (interface change)
 4. Flexible server integration support
 5. Event channels (remove static arrays of xprt handles, new EPOLL/KEVENT
    integration)

%package devel
Summary:	Development headers for %{name}
Requires:	%{name}%{?_isa} = %{version}

%description devel
Development headers and auxiliary files for developing with %{name}.

%prep
%setup -q -n ntirpc-%{version}

%build
%cmake . -DOVERRIDE_INSTALL_PREFIX=/usr -DTIRPC_EPOLL=1 -DUSE_GSS=ON "-GUnix Makefiles"

make %{?_smp_mflags}

%install
## make install is broken in various ways
## make install DESTDIR=%%{buildroot}
mkdir -p %{buildroot}%{_libdir}/pkgconfig
install -p -m 0755 src/%{name}.so.%{version} %{buildroot}%{_libdir}/
ln -s %{name}.so.%{version} %{buildroot}%{_libdir}/%{name}.so.1
ln -s %{name}.so.%{version} %{buildroot}%{_libdir}/%{name}.so
mkdir -p %{buildroot}%{_includedir}/ntirpc

make DESTDIR=%{buildroot} install

%post -p /sbin/ldconfig

%postun -p /sbin/ldconfig

%files
%{_libdir}/libntirpc.so.*
%{!?_licensedir:%global license %%doc}
%license COPYING
%doc NEWS README

%files devel
%{_libdir}/libntirpc.so
%dir %{_includedir}/ntirpc
%{_includedir}/ntirpc/*
%{_libdir}/pkgconfig/libntirpc.pc

%changelog
* Wed Jul 19 2017 Daniel Gryniewicz <dang at redhat.com> 1.6.0-1
- Upstream spec file
