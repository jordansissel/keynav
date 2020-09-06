Name:      keynav
Version:   0.20200906.0
Release:   1%{dist}
Summary:   A powerful MUD client with a built-in Perl interpreter
License:   FIXME
Group:     Productivity
URL:       https://www.semicomplete.com/projects/keynav

BuildRequires: make
BuildRequires: gcc
Requires:

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
