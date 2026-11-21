Name:           kateai
Version:        0.4.0
Release:        1%{?dist}
Summary:        AI coding agent plugin for the Kate text editor

License:        LGPL-2.1-or-later
URL:            https://github.com/KateAI/kate-ai
Source0:        %{name}-%{version}.tar.gz

BuildRequires:  cmake
BuildRequires:  extra-cmake-modules
BuildRequires:  gcc-c++
BuildRequires:  ninja-build
BuildRequires:  qt6-qtbase-devel
BuildRequires:  kf6-ktexteditor-devel
BuildRequires:  kf6-kcoreaddons-devel
BuildRequires:  kf6-ki18n-devel
BuildRequires:  kf6-kxmlgui-devel
BuildRequires:  kf6-kconfigwidgets-devel
BuildRequires:  kf6-kconfig-devel
BuildRequires:  kf6-kwidgetsaddons-devel
BuildRequires:  qt6-qtbase-devel

Requires:       kate
Requires:       kf6-ktexteditor
Requires:       qt6-qtbase
Requires:       bubblewrap

%description
Kate AI is a KTextEditor plugin that adds an agent chat tool view to Kate.
It reads the workspace, edits open documents, runs sandboxed commands, and
asks before destructive actions. Works on Fedora and Fedora Asahi Remix.

%prep
%autosetup -n %{name}-%{version}

%build
%cmake \
    -DCMAKE_BUILD_TYPE=Release \
    -DKATEAI_USER_INSTALL=OFF \
    -DKDE_INSTALL_USE_QT_SYS_PATHS=ON \
    -DBUILD_TESTING=OFF
%cmake_build

%install
%cmake_install

%files
%license LICENSE
%doc README.md
%{_libdir}/qt6/plugins/kf6/ktexteditor/kateai.so
# Page-size shim used by the agent sandbox on 4K-page systems (e.g. Asahi).
%dir %{_libdir}/kateai
%{_libdir}/kateai/libpage4k.so

%changelog
* Mon Nov 16 2026 ObiWindu <Obi.wandu@proton.me> - 0.4.0-1
- Release 0.4.0: secure credential storage, session database recovery,
  checkpoint safety snapshots, edit conflict detection, explicit turn
  lifecycle, sanitized MCP/ACP environments, and schema migration.

* Sun Nov 15 2026 ObiWindu <Obi.wandu@proton.me> - 0.3.0-1
- Release 0.3.0: ACP card fixes, apply_patch hardening, async read safety,
  live sub-agent activity, and improved tool/permission handling.

* Wed Sep 30 2026 ObiWindu <Obi.wandu@proton.me> - 0.1.0-1
- Initial package.
