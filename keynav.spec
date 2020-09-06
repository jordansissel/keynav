Name:      keynav
Version:   0.20200906.4
Release:   1%{dist}
Summary:   A powerful MUD client with a built-in Perl interpreter
License:   FIXME
Group:     Productivity
URL:       https://www.semicomplete.com/projects/keynav

Source0:  https://github.com/dhalucario/%{name}/archive/%{version}.tar.gz
BuildRoot: %(mktemp -ud %{_tmppath}/%{name}-%{version}-%{release}-XXXXXX)
BuildRequires: make
BuildRequires: gcc
BuildRequires: perl-podlators
BuildRequires: libX11-devel
BuildRequires: libXext-devel
BuildRequires: libXinerama-devel
BuildRequires: libXrandr-devel
BuildRequires: glib2-devel
BuildRequires: cairo-devel
BuildRequires: libxdo-devel

%description

%prep
%setup -q
make %{?_smp_mflags}

rm -rf $RPM_BUILD_ROOT
make DESTDIR=$RPM_BUILD_ROOT install

%files
%doc
