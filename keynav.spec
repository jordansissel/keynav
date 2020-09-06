Name:      keynav
Version:   0.20200906.1
Release:   1%{dist}
Summary:   A powerful MUD client with a built-in Perl interpreter
License:   FIXME
Group:     Productivity
URL:       https://www.semicomplete.com/projects/keynav

Source0:  https://github.com/dhalucario/%{name}/archive/%{version}.tar.gz

BuildRequires: make
BuildRequires: gcc
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

%build
%configure
make %{?_smp_mflags}

%install
rm -rf $RPM_BUILD_ROOT
%make_install

%files
%license LICENSE
%doc
