Name:           radfu
Version:        1.4.0
Release:        1%{?dist}
Summary:        Renesas RA Device Firmware Update tool

License:        AGPL-3.0-or-later
URL:            https://github.com/vjardin/radfu
Source0:        %{name}-%{version}.tar.gz

BuildRequires:  meson
BuildRequires:  ninja-build
BuildRequires:  gcc
BuildRequires:  libcmocka-devel
BuildRequires:  help2man

%description
A flash programming tool for Renesas RA microcontroller series.
Communicates with the built-in ROM bootloader to perform firmware
update operations via USB or UART/SCI interfaces.

%prep
%autosetup

%build
%meson
%meson_build

%install
%meson_install

%files
%license LICENSE
%doc README.md
%{_bindir}/radfu
%{_mandir}/man1/radfu.1*
