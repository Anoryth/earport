%global uuid earport@anoryth.github.io
# The C project lives in daemon/, the extension next to it
%global _vpath_srcdir daemon

Name:           earport
Version:        0.6.2
Release:        1%{?dist}
Summary:        AirPods integration for GNOME: battery, noise control and ear detection

License:        GPL-3.0-or-later
URL:            https://github.com/Anoryth/earport
Source0:        %{url}/archive/v%{version}/%{name}-%{version}.tar.gz

BuildRequires:  gcc
BuildRequires:  meson
BuildRequires:  pkgconfig(gio-unix-2.0)
BuildRequires:  bluez-libs-devel
BuildRequires:  systemd-rpm-macros
# Extension: translations and schema check
BuildRequires:  gettext
BuildRequires:  glib2
# Tests run on a private session bus
BuildRequires:  dbus-daemon

Requires:       bluez
Requires:       gnome-shell >= 46

%description
EarPort brings AirPods to the GNOME desktop: battery levels in the panel and
Quick Settings, noise control modes (noise cancellation, transparency,
adaptive), conversation awareness, and media paused when an AirPod leaves
the ear. A small service talks to the AirPods over Bluetooth; the GNOME
Shell extension shows it.

%prep
%autosetup

%build
%meson
%meson_build

%install
%meson_install
tools/pack-extension.sh --dir %{buildroot}%{_datadir}/gnome-shell/extensions/%{uuid}
# A system-wide extension can't compile its schema itself
glib-compile-schemas %{buildroot}%{_datadir}/gnome-shell/extensions/%{uuid}/schemas

%check
%meson_test

%post
%systemd_user_post earport-daemon.service

%preun
%systemd_user_preun earport-daemon.service

%files
%license LICENSE
%doc README.md CREDITS
%{_bindir}/earport-daemon
%{_userunitdir}/earport-daemon.service
%{_datadir}/dbus-1/services/io.github.anoryth.EarPort.service
%{_datadir}/gnome-shell/extensions/%{uuid}/

%changelog
* Thu Oct 29 2026 Florent Pauliat <flo7634@gmail.com> - 0.6.2-1
- Update to 0.6.2

* Sat Oct 24 2026 Florent Pauliat <flo7634@gmail.com> - 0.6.1-1
- Update to 0.6.1

* Sat Oct 10 2026 Florent Pauliat <flo7634@gmail.com> - 0.6.0-1
- First Fedora package
