%bcond_with kdbus
%bcond_with kdbus_use_policy_library

%define baseline 2.44
%define with_systemtap 0
%define keepstatic 1

Name:           glib2
Version:        2.44.1
Release:        0
# FIXME: find out if tapsets should really be in devel package or in main package
Summary:        General-Purpose Utility Library
License:        LGPL-2.0+
Group:          Base/Libraries
Url:            http://www.gtk.org/
Source:         http://download.gnome.org/sources/glib/%{baseline}/glib-%{version}.tar.xz
Source1:        glib2.sh
Source2:        glib2.csh
# Not upstream file. Only proposes upstream packages:
Source4:        glib2-upstream-gnome_defaults.conf
Source6:        macros.glib2
# Not depending on gtk-doc shortens bootstrap compilation path.
# Please update this file from the latest gtk-doc package:
Source7:        gtk-doc.m4
Source99:       baselibs.conf
Source1001: 	glib2.manifest
#Patch0:         glib2_aslr_20150416.patch

BuildRequires:  automake
BuildRequires:  fdupes
BuildRequires:  gcc-c++
BuildRequires:  pkg-config
BuildRequires:  python
BuildRequires:  gettext-tools
BuildRequires:  which
# gdbus-codegen is run during the build, so we need python-xml
BuildRequires:  python-xml
%if 0%{?with_systemtap}
BuildRequires:  systemtap-sdt-devel
%endif
# Needed for gresource
BuildRequires:	elfutils-libelf-devel-static
BuildRequires:  pkgconfig(libffi)
BuildRequires:  pkgconfig(libpcre)
BuildRequires:  pkgconfig(zlib)
%if %{with kdbus}
%if %{with kdbus_use_policy_library}
BuildRequires:  pkgconfig(dbus-policy)
BuildRequires:  pkgconfig(libsmack)
BuildRequires:  expat-devel
%endif
%endif

%description
GLib is a general-purpose utility library, which provides many useful
data types, macros, type conversions, string utilities, file utilities,
a main loop abstraction, and so on.

%package tools
Summary:        General-Purpose Utility Library -- Tools

%description tools
GLib is a general-purpose utility library, which provides many useful
data types, macros, type conversions, string utilities, file utilities,
a main loop abstraction, and so on.

%package -n gio-branding-upstream
Summary:        Upstream definitions of default settings and applications
Requires:       libgio = %{version}
Provides:       %{name}-branding-upstream = %{version}
Obsoletes:      %{name}-branding-upstream < %{version}
Provides:       gio-branding = %{version}
Conflicts:      otherproviders(gio-branding)
#Supplements:    packageand(libgio:branding-upstream)
BuildArch:      noarch
#BRAND: The /etc/gnome_defaults.conf allows to define arbitrary
#BRAND: applications as preferred defaults.
#BRAND: A /usr/share/glib-2.0/schemas/$NAME.gschema.override file can
#BRAND: be used to override the default value for GSettings keys. See
#BRAND: README.Gsettings-overrides for more details. The branding
#BRAND: package should then have proper Requires for features changed
#BRAND: with such an override file.
# NOTE: gnome_defaults is not an upstream feature, but a SuSE
# enhancement, but to conform branding conventions, the package is named
# as gio-branding-upstream.

%description -n gio-branding-upstream
This package provides upstream defaults for settings stored with
GSettings and applications used by the MIME system.

%package devel
#'
Requires:       %{name} = %{version}
Requires:       glibc-devel
Requires:       pkg-config
# Now require the subpackages too
Requires:       glib2-tools = %{version}
Requires:       libgio = %{version}
Requires:       libglib = %{version}
Requires:       libgmodule = %{version}
Requires:       libgobject = %{version}
Requires:       libgthread = %{version}
# Required by gdbus-codegen
Requires:       python-xml
Provides:       glib2-doc = 2.19.6
Obsoletes:      glib2-doc < 2.19.6
Summary:        General-Purpose Utility Library -- Development Files

%description devel
GLib is a general-purpose utility library, which provides many useful
data types, macros, type conversions, string utilities, file utilities,
a main loop abstraction, and so on.

This package contains the development files for GLib.

%package devel-static
Requires:       %{name}-devel = %{version}
Summary:        General-Purpose Utility Library -- Static Libraries

%description devel-static
GLib is a general-purpose utility library, which provides many useful
data types, macros, type conversions, string utilities, file utilities,
a main loop abstraction, and so on.

This package contains static versions of the GLib libraries.

%package -n libglib
Summary:        General-Purpose Utility Library
Provides:       %{name} = %{version}
Obsoletes:      %{name} < %{version}

%description -n libglib
GLib is a general-purpose utility library, which provides many useful
data types, macros, type conversions, string utilities, file utilities,
a main loop abstraction, and so on.

%package -n libgmodule
Summary:        General-Purpose Utility Library -- Library for Modules

%description -n libgmodule
GLib is a general-purpose utility library, which provides many useful
data types, macros, type conversions, string utilities, file utilities,
a main loop abstraction, and so on.

The libgmodule library provides a portable way to dynamically load
object files (commonly known as 'plug-ins').

%package -n libgio
Summary:        General-Purpose Utility Library -- Library for VFS
Requires:       gio-branding = %{version}
# bnc#555605: shared-mime-info is required by libgio to properly detect mime types.
#Requires:       shared-mime-info
# bnc#678518: libgio interacts with others by means of dbus-launch
#Requires:       dbus-1-x11
Provides:       gio = %{version}

%description -n libgio
GLib is a general-purpose utility library, which provides many useful
data types, macros, type conversions, string utilities, file utilities,
a main loop abstraction, and so on.

GIO provides a modern, easy-to-use VFS API.


%package -n libgthread
Summary:        General-Purpose Utility Library -- Library for Threads

%description -n libgthread
GLib is a general-purpose utility library, which provides many useful
data types, macros, type conversions, string utilities, file utilities,
a main loop abstraction, and so on.

The libgthread library provides a portable way to write multi-threaded
software.

%package -n libgobject
Summary:        General-Purpose Utility Library -- Object-Oriented Framework for C

%description -n libgobject
GLib is a general-purpose utility library, which provides many useful
data types, macros, type conversions, string utilities, file utilities,
a main loop abstraction, and so on.

The GObject library provides an object-oriented framework for C.

%prep
%setup -q -n %{name}-%{version}
cp %{SOURCE1001} .
cp -a %{S:1} %{S:2} .
cp -a %{S:4} gnome_defaults.conf
if ! test -f %{_datadir}/aclocal/gtk-doc.m4 ; then
    cp -a %{S:7} m4macros/
fi
#%patch0 -p1 -b .aslr

%build


export CFLAGS="%optflags -fPIC"
export CXXFLAGS="${CXXFLAGS:-%optflags} -fPIC"
%if "%{?tizen_profile_name}" == "tv"
export CFLAGS+=" -D_TV_PROFILE"
%endif
NOCONFIGURE=1 ./autogen.sh
%configure \
    --enable-static \
%if %{with kdbus}
    --enable-kdbus-transport \
%if %{with kdbus_use_policy_library}
    --enable-policy-in-lib \
%endif
%endif
%if 0%{?with_systemtap}
    --enable-systemtap \
%endif
    --with-pcre=internal

%{__make} %{?_smp_mflags} V=1


%install
%make_install

mkdir -p %{buildroot}%{_sysconfdir}/profile.d
install -D -m0644 glib2.sh %{buildroot}%{_sysconfdir}/profile.d/zzz-glib2.sh
install -D -m0644 glib2.csh %{buildroot}%{_sysconfdir}/profile.d/zzz-glib2.csh
install -D -m0644 gnome_defaults.conf %{buildroot}%{_sysconfdir}/gnome_defaults.conf
# default apps magic
mkdir -p %{buildroot}%{_localstatedir}/cache/gio-2.0 %{buildroot}%{_datadir}/applications
touch %{buildroot}%{_localstatedir}/cache/gio-2.0/gnome-defaults.list
touch %{buildroot}%{_localstatedir}/cache/gio-2.0/xfce-defaults.list
touch %{buildroot}%{_localstatedir}/cache/gio-2.0/lxde-defaults.list
ln -s %{_localstatedir}/cache/gio-2.0/gnome-defaults.list %{buildroot}%{_datadir}/applications/defaults.list
# gio-querymodules magic
%if "%{_lib}" == "lib64"
mv %{buildroot}%{_bindir}/gio-querymodules %{buildroot}%{_bindir}/gio-querymodules-64
%endif
touch %{buildroot}%{_libdir}/gio/modules/giomodule.cache
# gsettings magic
touch %{buildroot}%{_datadir}/glib-2.0/schemas/gschemas.compiled
# remove files we don't care about
find %{buildroot}%{_libdir} -name '*.la' -type f -delete -print
# Install rpm macros
mkdir -p %{buildroot}%{_sysconfdir}/rpm
cp %{S:6} %{buildroot}%{_sysconfdir}/rpm
%fdupes %{buildroot}

find %{buildroot}/usr/share/locale/ -name "*.mo" -type f -exec rm {} \;

%post -n libglib -p /sbin/ldconfig

%post -n libgobject -p /sbin/ldconfig

%post -n libgthread -p /sbin/ldconfig

%post -n libgio -p /sbin/ldconfig

%post -n libgmodule -p /sbin/ldconfig

%postun -n libglib -p /sbin/ldconfig

%postun -n libgobject -p /sbin/ldconfig

%postun -n libgthread -p /sbin/ldconfig

%postun -n libgio -p /sbin/ldconfig

%postun -n libgmodule -p /sbin/ldconfig


%files tools
%manifest %{name}.manifest
%defattr(-,root,root)
%dir %{_datadir}/bash-completion
%dir %{_datadir}/bash-completion/completions
%{_datadir}/bash-completion/completions/gresource
%{_datadir}/bash-completion/completions/gsettings
%{_bindir}/gdbus
%{_bindir}/gio-querymodules*
%{_bindir}/glib-compile-schemas
%{_bindir}/gresource
%{_bindir}/gsettings
%{_bindir}/gapplication
# We put those files here, but they don't really belong here. They just don't
# have a better home... The zzz-glib2 scripts could arguably be in
# libglib but that would break the shared library policy.
%{_sysconfdir}/profile.d/zzz-glib2.*

%files -n gio-branding-upstream
%manifest %{name}.manifest
%defattr(-,root,root)
%config (noreplace) %{_sysconfdir}/gnome_defaults.conf

%files -n libglib
%manifest %{name}.manifest
%defattr(-, root, root)
%doc COPYING
%{_libdir}/libglib*.so.*

%files -n libgmodule
%manifest %{name}.manifest
%defattr(-, root, root)
%{_libdir}/libgmodule*.so.*

%files -n libgobject
%manifest %{name}.manifest
%defattr(-, root, root)
%{_libdir}/libgobject*.so.*

%files -n libgthread
%manifest %{name}.manifest
%defattr(-, root, root)
%{_libdir}/libgthread*.so.*

%files -n libgio
%manifest %{name}.manifest
%defattr(-, root, root)
%{_libdir}/libgio*.so.*
%dir %{_libdir}/gio
%dir %{_libdir}/gio/modules
%ghost %{_libdir}/gio/modules/giomodule.cache
%dir %{_datadir}/glib-2.0/
%dir %{_datadir}/glib-2.0/schemas/
%ghost %{_datadir}/glib-2.0/schemas/gschemas.compiled
%{_datadir}/applications/defaults.list
%dir %{_localstatedir}/cache/gio-2.0
%ghost %{_localstatedir}/cache/gio-2.0/gnome-defaults.list
%ghost %{_localstatedir}/cache/gio-2.0/xfce-defaults.list
%ghost %{_localstatedir}/cache/gio-2.0/lxde-defaults.list



%files devel
%manifest %{name}.manifest
%defattr(-,root,root)

%{_bindir}/gdbus-codegen
%{_datadir}/bash-completion/completions/*
%_datadir/glib-2.0/codegen

%{_bindir}/glib-compile-resources
%{_bindir}/glib-genmarshal
%{_bindir}/glib-gettextize
%{_bindir}/glib-mkenums
%{_bindir}/gobject-query
%{_bindir}/gtester
%{_bindir}/gtester-report
%dir %{_datadir}/aclocal
%{_datadir}/aclocal/glib-2.0.m4
%{_datadir}/aclocal/glib-gettext.m4
%{_datadir}/aclocal/gsettings.m4
%dir %{_datadir}/glib-2.0/
%{_datadir}/glib-2.0/gdb/
%{_datadir}/glib-2.0/gdb/*
%{_datadir}/glib-2.0/gettext/
%{_datadir}/glib-2.0/gettext/po
%{_datadir}/glib-2.0/gettext/po/*
%{_datadir}/glib-2.0/schemas/gschema.dtd
%{_includedir}/glib-2.0
%{_includedir}/gio-unix-2.0
%{_libdir}/lib*.so
%dir %{_libdir}/glib-2.0/
%{_libdir}/glib-2.0/include/
%{_libdir}/glib-2.0/include/*
%{_libdir}/pkgconfig/*.pc
%{_datadir}/gdb/auto-load/%{_libdir}/*-gdb.py
%if 0%{?with_systemtap}
%{_datadir}/systemtap/tapset/*.stp
%endif
%{_sysconfdir}/rpm/macros.glib2
# Own these directories to not depend on gdb
%dir %{_datadir}/gdb
%dir %{_datadir}/gdb/auto-load
%dir %{_datadir}/gdb/auto-load%{_prefix}
%dir %{_datadir}/gdb/auto-load%{_libdir}

%files devel-static
%manifest %{name}.manifest
%defattr(-,root,root)
%{_libdir}/lib*.a

%changelog
