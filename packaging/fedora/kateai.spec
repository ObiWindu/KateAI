Name:           kateai
Version:        0.8.0
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
* Tue Nov 24 2026 ObiWindu <Obi.wandu@proton.me> - 0.8.0-1
- Release 0.8.0: deterministic prompt assembler with a stable cached prefix,
  model profiles and native Gemini cached-content resources, a graduated
  context engine with tool-result paging and rehydration, a memory hierarchy
  with scoped AGENTS/rules files and memory_write, a deferred tool registry
  with tool_search/tool_describe/tool_call, and edit canonicalisation for
  smart quotes, CRLF and whitespace drift.

* Sun Nov 22 2026 ObiWindu <Obi.wandu@proton.me> - 0.7.0-1
- Release 0.7.0: append-only session event log with replay and JSONL export,
  deterministic verification gate for mutating turns, deny-first policy engine
  with an immutable hardline floor and audit trail, hooks v1 for
  session/prompt/tool/stop lifecycle events, and turn middleware.

* Sat Nov 21 2026 ObiWindu <Obi.wandu@proton.me> - 0.6.0-1
- Release 0.6.0: live Kate debugger bridge and twelve debugger tools, build
  and terminal tools, structured LSP diagnostics from Kate's diagnostics
  model, ecosystem bridges (build, terminal, project tree, backtrace browser),
  native review/debug/graph tabs, and debugger-aware editor assistance.
- Ship the 4K-page sandbox shim, which rpmbuild previously reported as an
  unpackaged file on x86_64 and aarch64.

* Wed Nov 18 2026 ObiWindu <Obi.wandu@proton.me> - 0.5.1-1
- Release 0.5.1: make the sandbox runtime test portable to Fedora aarch64
  hosts that cannot create the network or cgroup namespaces required by the
  Strict/Read-only profiles. No user-facing behavior changes.

* Wed Nov 18 2026 ObiWindu <Obi.wandu@proton.me> - 0.5.0-1
- Release 0.5.0: hardened ACP agent launch and protocol handling, stricter
  sandbox and permission checks, safer checkpoints and patches, reliable
  conversation restore, and reworked sub-agent cards.

* Mon Nov 16 2026 ObiWindu <Obi.wandu@proton.me> - 0.4.0-1
- Release 0.4.0: secure credential storage, session database recovery,
  checkpoint safety snapshots, edit conflict detection, explicit turn
  lifecycle, sanitized MCP/ACP environments, and schema migration.

* Sun Nov 15 2026 ObiWindu <Obi.wandu@proton.me> - 0.3.0-1
- Release 0.3.0: ACP card fixes, apply_patch hardening, async read safety,
  live sub-agent activity, and improved tool/permission handling.

* Wed Sep 30 2026 ObiWindu <Obi.wandu@proton.me> - 0.1.0-1
- Initial package.
