Summary: A library of handy utility functions
Name: glib2
Version: 2.32.3
Release: 5
License: LGPLv2+
Group: System/Libraries
URL: http://www.gtk.org
Source: %{name}-%{version}.tar.gz
#Source: http://download.gnome.org/sources/glib/2.32/%{name}-%{version}.tar.gz
Source1001:     %{name}.manifest

BuildRequires: pkgconfig
BuildRequires: gettext-tools
BuildRequires: libattr-devel
BuildRequires: zlib-devel
BuildRequires: python
BuildRequires: python-xml
BuildRequires: libffi-devel
BuildRequires: elfutils-libelf-devel

%description
GLib is the low-level core library that forms the basis for projects
such as GTK+ and GNOME. It provides data structure handling for C,
portability wrappers, and interfaces for such runtime functionality
as an event loop, threads, dynamic loading, and an object system.


%package devel
Summary: A library of handy utility functions
Group: Development/Libraries
Requires: pkgconfig
Requires: %{name} = %{version}-%{release}

%description devel
The glib2-devel package includes the header files for the GLib library.

# anaconda needs static libs, see RH bug #193143
%package static
Summary: A library of handy utility functions
Group: Development/Libraries
Requires: %{name}-devel = %{version}-%{release}

%description static
The glib2-static package includes static libraries of the GLib library.


%prep
%setup -q

%build
cp %{SOURCE1001} .
%configure --disable-gtk-doc --enable-static --disable-selinux --disable-visibility --enable-debug=yes

make %{?_smp_mflags}

%install
rm -rf $RPM_BUILD_ROOT
make install DESTDIR=$RPM_BUILD_ROOT

rm -f $RPM_BUILD_ROOT%{_libdir}/*.la
rm -f $RPM_BUILD_ROOT%{_libdir}/gio/modules/*.{a,la}
rm -f $RPM_BUILD_ROOT%{_datadir}/glib-2.0/gdb/*.{pyc,pyo}
rm -f $RPM_BUILD_ROOT%{_libdir}/gdbus-codegen/*.{pyc,pyo}

touch $RPM_BUILD_ROOT%{_libdir}/gio/modules/giomodule.cache

# MeeGo does not provide bash completion
rm -rf ${RPM_BUILD_ROOT}%{_sysconfdir}/bash_completion.d

%find_lang glib20

mkdir -p $RPM_BUILD_ROOT%{_datadir}/license
for keyword in LICENSE COPYING COPYRIGHT;
do
	for file in `find %{_builddir} -name $keyword`;
	do
		cat $file >> $RPM_BUILD_ROOT%{_datadir}/license/%{name};
		echo "";
	done;
done

%clean
rm -rf $RPM_BUILD_ROOT

%post -p /sbin/ldconfig

%postun -p /sbin/ldconfig

%files -f glib20.lang
%manifest %{name}.manifest
%defattr(-, root, root, -)
%doc AUTHORS COPYING NEWS README
%{_datadir}/license/%{name}
%{_libdir}/libglib-2.0.so.*
%{_libdir}/libgthread-2.0.so.*
%{_libdir}/libgmodule-2.0.so.*
%{_libdir}/libgobject-2.0.so.*
%{_libdir}/libgio-2.0.so.*
%dir %{_datadir}/glib-2.0
%dir %{_datadir}/glib-2.0/schemas
%dir %{_libdir}/gio
%dir %{_libdir}/gio/modules
%ghost %{_libdir}/gio/modules/giomodule.cache
%{_bindir}/gio-querymodules*
%{_bindir}/glib-compile-schemas
%{_bindir}/gsettings
%{_bindir}/gdbus
%doc %{_mandir}/man1/gio-querymodules.1.gz
%doc %{_mandir}/man1/glib-compile-schemas.1.gz
%doc %{_mandir}/man1/gsettings.1.gz
%doc %{_mandir}/man1/gdbus.1.gz

%files devel
%defattr(-, root, root, -)
%{_libdir}/lib*.so
%{_libdir}/glib-2.0
%{_includedir}/*
%{_datadir}/aclocal/*
%{_libdir}/pkgconfig/*
%{_datadir}/glib-2.0/gdb
%{_datadir}/glib-2.0/gettext
%{_datadir}/glib-2.0/schemas/gschema.dtd
%{_bindir}/glib-genmarshal
%{_bindir}/glib-gettextize
%{_bindir}/glib-mkenums
%{_bindir}/gobject-query
%{_bindir}/gtester
%{_bindir}/gdbus-codegen
%{_bindir}/glib-compile-resources
%{_bindir}/gresource
%{_libdir}/gdbus-2.0/codegen
%attr (0755, root, root) %{_bindir}/gtester-report
%doc %{_datadir}/gtk-doc/html/*
%doc %{_mandir}/man1/glib-genmarshal.1.gz
%doc %{_mandir}/man1/glib-gettextize.1.gz
%doc %{_mandir}/man1/glib-mkenums.1.gz
%doc %{_mandir}/man1/gobject-query.1.gz
%doc %{_mandir}/man1/gtester-report.1.gz
%doc %{_mandir}/man1/gtester.1.gz
%doc %{_mandir}/man1/gdbus-codegen.1.gz
%doc %{_mandir}/man1/glib-compile-resources.1.gz
%doc %{_mandir}/man1/gresource.1.gz
%{_datadir}/gdb/auto-load%{_libdir}/libglib-2.0.so.*-gdb.py*
%{_datadir}/gdb/auto-load%{_libdir}/libgobject-2.0.so.*-gdb.py*

%files static
%defattr(-, root, root, -)
%{_libdir}/lib*.a

